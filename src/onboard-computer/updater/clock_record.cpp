#include "clock_record.hpp"

#include <array>

#include "bytes.hpp"
#include "config.hpp"

namespace clock_record
{

namespace
{

// The record is one 64-bit number: two little-endian 32-bit words, low word first.
constexpr size_t kOffsetLowWord = 0;
constexpr size_t kOffsetHighWord = 4;
constexpr unsigned kWordBits = 32U;
static_assert(kOffsetHighWord + 4 == config::kClockOffsetRecordBytes,
  "the record is the offset: 8 bytes, low word then high word");
static_assert(config::kClockOffsetRecordBytes <= DurableRecord::kRecordMaxBytes,
  "the record must fit a DurableRecord");

}  // namespace

/**
 * @brief Reads the clock-offset record and turns what was found into an offset.
 *
 * A good record gives its signed 64-bit offset, read as two little-endian 32-bit words (bytes.hpp)
 * joined in unsigned arithmetic, then converted to signed (two's complement). No file, a damaged
 * file and an unreadable file all give offset 0 (Proposal (TBR), awaiting the owner's acceptance;
 * clock_record.hpp says why). A record is never made up from a damaged file. A status outside the
 * four (corrupted memory, code rules B1) is treated as unreadable: the updater has no fault
 * reaction yet (open), and for a clock the safe value is the system clock itself, offset 0.
 *
 * @param[in]     record  the "clock-offset" record of the data directory
 * @param[out]    offset  always written: seconds to add to the system clock; 0 unless kStored
 * @param[out]    error   errno value for kUnreadable; 0 otherwise
 * @return where the offset came from
 */
Source load(const DurableRecord & record, int64_t & offset, int & error)
{
  std::array<uint8_t, config::kClockOffsetRecordBytes> bytes_in{};
  error = 0;
  offset = 0;
  switch (record.read(bytes_in.data(), bytes_in.size(), error)) {
    case DurableRecord::RECORD_OK: {
      uint64_t low = bytes::readU32(&bytes_in[kOffsetLowWord]);
      uint64_t high = bytes::readU32(&bytes_in[kOffsetHighWord]);
      offset = static_cast<int64_t>((high << kWordBits) | low);
      return Source::kStored;
    }
    case DurableRecord::RECORD_ABSENT:
      return Source::kNoRecord;
    case DurableRecord::RECORD_DAMAGED:
      return Source::kDamaged;
    case DurableRecord::RECORD_ERROR:
      return Source::kUnreadable;
  }
  return Source::kUnreadable;   // a status that cannot exist is corrupted memory: offset 0 stays
}

/**
 * @brief Writes the offset as the durable record.
 *
 * The signed offset is converted to unsigned (two's complement) and written as two little-endian
 * 32-bit words, low word first. The write is DurableRecord's: temporary file, flush, rename,
 * directory flush. The caller answers DONE only after this returned true (REQ-034).
 *
 * @param[in,out] record  the "clock-offset" record of the data directory
 * @param[in]     offset  seconds to add to the system clock to give onboard ground time
 * @param[out]    error   errno value when false is returned; 0 otherwise
 * @return true: written and flushed; false: not written, or not shown to be durable
 */
bool save(DurableRecord & record, int64_t offset, int & error)
{
  std::array<uint8_t, config::kClockOffsetRecordBytes> bytes_out{};
  uint64_t bits = static_cast<uint64_t>(offset);
  bytes::writeU32(&bytes_out[kOffsetLowWord], static_cast<uint32_t>(bits));
  bytes::writeU32(&bytes_out[kOffsetHighWord], static_cast<uint32_t>(bits >> kWordBits));
  return record.write(bytes_out.data(), bytes_out.size(), error);
}

/**
 * @brief Names a Source for the log.
 *
 * @param[in]     source  what load() found
 * @return a short text; never null
 */
const char * sourceName(Source source)
{
  switch (source) {
    case Source::kStored:     return "stored";
    case Source::kNoRecord:   return "no record";
    case Source::kDamaged:    return "damaged";
    case Source::kUnreadable: return "unreadable";
  }
  return "unknown";   // cannot exist: corrupted memory (code rules B1); named for the log only
}

}  // namespace clock_record
