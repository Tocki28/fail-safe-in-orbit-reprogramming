#include "limits_record.hpp"

#include <array>

#include "bytes.hpp"
#include "config.hpp"

namespace limits_record
{

namespace
{

// Offsets of the two fields in the record (limits_record.hpp).
constexpr size_t kOffsetMinimumVersion = 0;
constexpr size_t kOffsetIdentity = 4;
static_assert(kOffsetIdentity + 2 == config::kLimitsRecordBytes,
  "the record is the minimum version (4) then the identity (2): SET LIMITS' fields");
static_assert(config::kLimitsRecordBytes <= DurableRecord::kRecordMaxBytes,
  "the record must fit a DurableRecord");

}  // namespace

/**
 * @brief Reads the limits record and turns what was found into limits.
 *
 * A good record gives its two fields. No file gives the defaults of owner decision 24 (minimum
 * version 1, identity 0x0431 as a Proposal, TBR). A damaged or unreadable record gives the
 * defaults' values with usable = false: the image checker then refuses every image 0x0A, until
 * SET LIMITS writes a good record (fail closed, TBR). A record is never made up from a damaged
 * file. A status outside the four (corrupted memory, code rules B1) is treated as unreadable:
 * the updater has no fault reaction yet (open), so this fails closed.
 *
 * @param[in]     record  the "limits" record of the data directory
 * @param[out]    limits  always written, as described
 * @param[out]    error   errno value for kUnreadable; 0 otherwise
 * @return where the limits came from
 */
Source load(const DurableRecord & record, ImageLimits & limits, int & error)
{
  std::array<uint8_t, config::kLimitsRecordBytes> bytes_in{};
  error = 0;
  limits = defaultImageLimits();
  switch (record.read(bytes_in.data(), bytes_in.size(), error)) {
    case DurableRecord::RECORD_OK:
      limits.minimum_version = bytes::readU32(&bytes_in[kOffsetMinimumVersion]);
      limits.identity = bytes::readU16(&bytes_in[kOffsetIdentity]);
      return Source::kStored;
    case DurableRecord::RECORD_ABSENT:
      return Source::kNoRecord;
    case DurableRecord::RECORD_DAMAGED:
      limits.usable = false;
      return Source::kDamaged;
    case DurableRecord::RECORD_ERROR:
      limits.usable = false;
      return Source::kUnreadable;
  }
  limits.usable = false;   // a status that cannot exist is corrupted memory: fail closed
  return Source::kUnreadable;
}

/**
 * @brief Writes the limits as the durable record.
 *
 * Minimum version (4) then identity (2), little-endian. The write is DurableRecord's: temporary
 * file, flush, rename, directory flush. The caller answers DONE only after this returned true
 * (REQ-034).
 *
 * @param[in,out] record  the "limits" record of the data directory
 * @param[in]     limits  the limits to keep; usable is not stored (a stored record is a good one)
 * @param[out]    error   errno value when false is returned; 0 otherwise
 * @return true: written and flushed; false: not written, or not shown to be durable
 */
bool save(DurableRecord & record, const ImageLimits & limits, int & error)
{
  std::array<uint8_t, config::kLimitsRecordBytes> bytes_out{};
  bytes::writeU32(&bytes_out[kOffsetMinimumVersion], limits.minimum_version);
  bytes::writeU16(&bytes_out[kOffsetIdentity], limits.identity);
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

}  // namespace limits_record
