#include "report_store.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bytes.hpp"
#include "config.hpp"
#include "crc32.hpp"
#include "log.hpp"
#include "protocol.hpp"

/**
 * @brief Names a mode code of a HEALTH report, for the log.
 *
 * @param[in] mode  mode byte of a HEALTH report
 * @return "OPERATE", "TRIAL" or "FALLBACK"; "unknown" for any other value
 */
const char * modeName(uint8_t mode)
{
  switch (mode) {
    case report::kModeOperate:  return "OPERATE";
    case report::kModeTrial:    return "TRIAL";
    case report::kModeFallback: return "FALLBACK";
    default:                    return "unknown";   // input from the link: a raw byte off the wire; any other value is unknown
  }
}

/**
 * @brief Names an event code as the Sec. 5.4 table does, for the log.
 *
 * @param[in]     code  the event code of an EVENT report (Interfaces Sec. 5.4)
 * @return the event's name, or "unknown" for a code not in the table
 */
const char * eventName(uint16_t code)
{
  switch (code) {
    case report::kEventFaultDetected: return "fault detected";
    case 0x0002: return "fall-back to the previous copy";
    case 0x0003: return "fall-back to the last-resort firmware";
    case 0x0004: return "self-test failed";
    case report::kEventMemoryChange: return "change in memory detected";
    case report::kEventTrialStarted: return "trial started";
    case report::kEventFirmwareConfirmed: return "firmware confirmed";
    case report::kEventFirmwareGivenUp: return "firmware given up";
    case 0x0009: return "restart";   // the controller's own event; the onboard computer's is 0x0106
    case report::kEventTrialTestsPassed: return "trial tests passed";
    case report::kEventTrialTestsFailed: return "trial tests failed";
    case 0x0101: return "data link lost";
    case 0x0102: return "data link working again";
    case 0x0103: return "restore done";
    case 0x0104: return "health reports dropped";
    case 0x0105: return "onboard computer clock differs from ground time";
    case report::kEventControllerRestarted: return "controller restarted by the onboard computer";
    default:     return "unknown";   // input from the link: a raw byte off the wire; any other value is unknown
  }
}

/**
 * @brief Names a refusal reason as the Interfaces Sec. 4.2 table does, for the log.
 *
 * Decision (owner, 13 Oct 2026): 0x10 "Image of the wrong kind for its place" and 0x11 "Boot state
 * could not be stored" are named like the rest. 0xFD to 0xFF are the bench codes of protocol.hpp
 * (not in the table).
 *
 * @param[in]     code  the refusal reason (Interfaces Sec. 4.2; protocol.hpp)
 * @return the reason's name; "unknown" for a code not in the table or protocol.hpp
 */
const char * refusalName(uint8_t code)
{
  switch (code) {
    case answer::kNotArmed:           return "dangerous command without a valid arming command";
    case answer::kArmExpired:         return "arming command expired";
    case 0x03:                        return "command past its expiry";
    case answer::kOldCommand:         return "old command sent again";
    case answer::kNotInThisMode:      return "not allowed in the current mode";
    case answer::kImpossible:         return "impossible values, or an unknown command";
    case answer::kTrialRunning:       return "no new firmware during a trial";
    case answer::kImageDamaged:       return "image damaged";
    case answer::kNotFromOperator:    return "image not from the operator";
    case 0x0A:                        return "image older than the minimum version";
    case answer::kOtherController:    return "image built for another controller";
    case 0x0C:                        return "the controller's clock disagrees with the scheduled time";
    case 0x0D:                        return "a dangerous command cannot be scheduled";
    case 0x0E:                        return "image built for the other slot";
    case answer::kTrialTestsNotPassed: return "trial tests not passed";
    case answer::kWrongKind:          return "image of the wrong kind for its place";
    case answer::kBootStateNotStored: return "boot state could not be stored";
    // REQ-051; owner decision 21, 15 Oct 2026; Interfaces Sec. 4.2 row pending (TBD)
    case answer::kCancelled:          return "delivery cancelled by the operator";
    // owner decision 30, 15 Oct 2026; Interfaces Sec. 4.2 row pending (TBR)
    case answer::kImageGivenUp:       return "the image was given up; START FAILED starts it again";
    case answer::kNoAnswerBench:      return "controller did not answer (bench code)";
    case answer::kStorageBench:       return "onboard storage failed (bench code)";
    case answer::kCannotSendBench:    return "data link A cannot send (bench code)";
    default:                          return "unknown";   // input from the link: a raw byte off the wire; any other value is unknown
  }
}

/**
 * @brief Names a check result of events 0x0002 and 0x0003 as the Interfaces Sec. 5.4 check-result table does.
 *
 * The table is a copy of the CHECK_* codes of src/controller/common/backup_layout.h, where the
 * bootloader that writes them defines them; the unit test (testCheckResultCodes) reads the header
 * and fails when a code is missing here. 0x0E to 0x10 are a trial or copy passed over without a
 * check of its image (owner's decisions of 13 Oct 2026, second round). A value in no row is
 * returned as null, as faultKindName() does, so the caller prints "unknown code 0x.." with the
 * number and never a bare "unknown".
 *
 * @param[in]     result  the check-result byte of the detail
 * @return the check that failed ("none: the copy passed" for 0x00); null for a value not in the table
 */
const char * checkResultName(uint8_t result)
{
  switch (result) {
    case 0x00: return "none: the copy passed";
    case 0x01: return "the copy is empty";
    case 0x02: return "proprietary string";
    case 0x03: return "format";
    case 0x04: return "kind";
    case 0x05: return "target";
    case 0x06: return "length";
    case 0x07: return "start address outside the region";
    case 0x08: return "body check value";
    case 0x09: return "signature A";
    case 0x0A: return "signature B";
    case 0x0B: return "flash read error";   // Sec. 5.4 check-result table, decision 4 (13 Oct 2026)
    case 0x0C: return "trial started too often";
    case 0x0D: return "restarted too often";
    case 0x0E: return "not started: watchdog not confirmed running";   // CHECK_WATCHDOG_NOT_RUNNING
    case 0x0F: return "not started: start count could not be saved";   // CHECK_START_NOT_COUNTED
    case 0x10: return "boot state damaged, only the last-resort tried";   // CHECK_STATE_DAMAGED
    default:   return nullptr;   // input from the link: a raw byte off the wire; any other value is not listed
  }
}

/**
 * @brief One row of a code table: a number the controller sends and the words for it.
 *
 * Plain data, constant-initialized; the tables below are arrays of these.
 */
struct CodeText {
  uint8_t code;        ///< the number as the controller sends it
  const char * text;   ///< its words for the log
};

/**
 * Fault kinds, the 1-byte detail of EVENT 0x0001. COPIED from src/controller/common/fault_codes.h
 * (FAULT_KIND_*), where the controller defines them; keep the two in step. The unit test
 * (test_updater.cpp, testFaultCodes) reads the header and fails when a code is missing here.
 */
constexpr std::array<CodeText, 1> kFaultKinds = {{
  {0x01U, "core clock failed, running at 16 MHz"},   // FAULT_KIND_CORE_CLOCK
}};

/**
 * Fault locations, the 1-byte detail of EVENT 0x0005. COPIED from
 * src/controller/common/fault_codes.h (FAULT_AT_*): one code per function that holds a fault
 * site; ranges 0x01-0x0F bootloader, 0x10-0x1F common, 0x20-0x4F firmware. Keep in step with
 * the header; testFaultCodes in test_updater.cpp fails when a code is missing or extra here.
 * The words are short: the program and function, then what is impossible there.
 */
constexpr std::array<CodeText, 28> kFaultLocations = {{
  {0x01U, "bootloader backup: pair number past the last pair"},                 // VALUE_INDEX
  {0x02U, "bootloader: slot code is not A, B or none"},                         // SLOT_LETTER
  {0x03U, "bootloader: reset cause outside its list"},                          // RESET_CAUSE_TEXT
  {0x04U, "bootloader: boot state load result outside its list"},               // LOAD_STATE
  {0x05U, "bootloader: slot code is not A or B"},                               // SLOT_REGION
  {0x06U, "bootloader: start role is not confirmed, trial or last resort"},     // ROLE_TEXT
  {0x07U, "bootloader: image check result outside its list"},                   // CHECK_RESULT_CODE
  // 0x08-0x0A: fault_codes.h lines 82-91 (REQ-119, owner's decision 35, 15 Oct 2026).
  {0x08U, "bootloader boot state: slot code is not A or B"},                    // BL_BOOT_STATE_OTHER_SLOT
  {0x09U, "bootloader image check: image check result outside its list"},       // BL_IMAGE_RESULT_TEXT
  {0x0AU, "bootloader image check: region size smaller than the image header"},  // BL_IMAGE_CHECK_CONTENTS
  {0x10U, "image check (both programs): image check result outside its list"},  // IMAGE_RESULT_TEXT
  {0x11U, "boot state (both programs): slot code is not A or B"},               // BOOT_STATE_OTHER_SLOT
  {0x20U, "boot slot: stored slot code is not none, A or B"},                   // TO_SLOT
  {0x21U, "boot slot: other slot of none, or of a value outside the slots"},    // OTHER_SLOT
  {0x22U, "boot slot: state code of a value outside the slots"},                // STATE_CODE
  {0x23U, "core clock: clock is not 170 MHz or 16 MHz (start tick)"},           // CORE_CLOCK_START_TICK
  {0x24U, "UART link: clock is not 170 MHz or 16 MHz (init)"},                  // UART_LINK_INIT
  {0x25U, "health monitor: slot code outside the slots"},                       // SLOT_CODE
  {0x26U, "update receiver: slot start of a value outside the slots"},          // UPDATE_RECEIVER_SLOT_START
  {0x27U, "update receiver: image check result outside its list"},              // UPDATE_RECEIVER_REFUSAL_FOR
  {0x28U, "update receiver: upload identifier outside its list (values fit)"},  // UPDATE_RECEIVER_VALUES_FIT
  {0x29U, "update receiver: upload identifier outside its list (run)"},         // UPDATE_RECEIVER_RUN
  {0x2AU, "arm state: a word differs from its complement, or is no code word"},  // ARM_STATE_CHECK_INTACT
  {0x2BU, "test image: upload identifier outside its list (accept)"},           // TEST_IMAGE_ACCEPT
  {0x2CU, "test image: upload identifier outside its list (run)"},              // TEST_IMAGE_RUN
  {0x2DU, "mode manager: slot code outside the slots (copy code)"},             // COPY_CODE
  {0x2EU, "mode manager: fall-back mode (never set) or a mode outside its list (poll)"},  // MODE_MANAGER_POLL
  {0x2FU, "mode manager: dangerous command that is neither switch nor confirm"},  // MODE_MANAGER_RUN_DANGEROUS
}};

/**
 * @brief Looks a number up in a code table.
 *
 * A bounded walk over the table (A9: fixed upper bound, no recursion); the number comes off the
 * link, so a number that is in no row is not an error here, it is reported to the caller.
 *
 * @param[in]     rows   the first row of the table
 * @param[in]     count  the number of rows
 * @param[in]     code   the number received
 * @return the row's words, or null when the table has no such number
 */
const char * lookupCode(const CodeText * rows, size_t count, uint8_t code)
{
  for (size_t i = 0; i < count; i++) {
    if (rows[i].code == code) {
      return rows[i].text;
    }
  }
  return nullptr;
}

/**
 * @brief Names a fault kind, the detail byte of EVENT 0x0001 "fault detected".
 *
 * @param[in]     kind  the fault-kind byte (fault_codes.h FAULT_KIND_*)
 * @return the kind in words; null for a value not in the table, so the caller prints the number
 */
const char * faultKindName(uint8_t kind)
{
  return lookupCode(kFaultKinds.data(), kFaultKinds.size(), kind);
}

/**
 * @brief Names a fault location, the detail byte of EVENT 0x0005 "change in memory detected".
 *
 * @param[in]     where  the fault-location byte (fault_codes.h FAULT_AT_*)
 * @return the location in words; null for a value not in the table, so the caller prints the number
 */
const char * faultLocationName(uint8_t where)
{
  return lookupCode(kFaultLocations.data(), kFaultLocations.size(), where);
}

/**
 * @brief Writes the log text of an EVENT 0x0002 or 0x0003: refused copy and check result.
 *
 * A helper of describeEvent(), split out to keep that function short (A9). The check result is
 * decoded with checkResultName(); one in no table prints as "unknown code 0x.." with the number
 * (Interfaces Sec. 5.4 check-result table; REQ-040).
 *
 * @param[in]     code           the event code, 0x0002 or 0x0003
 * @param[in]     detail         the detail bytes
 * @param[in]     detail_length  number of detail bytes
 * @param[out]    out            receives the text, always terminated by snprintf
 * @param[in]     room           size of out, in bytes (at least 1)
 * @return the snprintf result: characters that would be written, or negative on failure
 */
static int describeFallback(uint16_t code, const uint8_t * detail, size_t detail_length, char * out,
  size_t room)
{
  if (detail_length < report::kFallbackDetailBytes) {
    return std::snprintf(out, room, "EVENT 0x%04x (%s), detail of %zu byte(s) instead of the 2 of Sec. 5.4%s",
      code, eventName(code), detail_length, (detail_length == 1) ? ": refused copy only, no check result" : "");
  }
  const char * result = checkResultName(detail[1]);
  if (result != nullptr) {
    return std::snprintf(out, room, "EVENT 0x%04x (%s), refused copy 0x%02x (%s), check result 0x%02x (%s)",
      code, eventName(code), detail[0], slotName(detail[0]), detail[1], result);
  }
  return std::snprintf(out, room, "EVENT 0x%04x (%s), refused copy 0x%02x (%s), check result unknown code 0x%02x",
    code, eventName(code), detail[0], slotName(detail[0]), detail[1]);
}

/**
 * @brief Writes the log text of an EVENT 0x0005 "change in memory detected" with its detail.
 *
 * The detail is where (1) and later faults (1) (Interfaces Sec. 5.4 event table; fault_codes.h,
 * FAULT_FOUND_BY_BOOTLOADER). With both bytes, bit 7 of where says who found the fault: set, the
 * bootloader at the last start ("found by the bootloader", then "N more after it", N being the
 * second byte); clear, the firmware ("found by the firmware"; a second byte other than 0 is shown
 * too, never dropped). A second byte of 255 is the saturated count (BACKUP_FAULT_LATER_MAX) and
 * reads "255 or more after it", so it is not taken for an exact count. The location is the lower
 * seven bits, named with faultLocationName(); one in no table prints as "unknown code 0x.." with the number. With one byte only the text is as
 * before the second byte existed: the byte as a location, nothing about who found it.
 *
 * @param[in]     code           the event code, 0x0005
 * @param[in]     detail         the detail bytes, at least one
 * @param[in]     detail_length  number of detail bytes, at least 1
 * @param[out]    out            receives the text, always terminated by snprintf
 * @param[in]     room           size of out, in bytes (at least 1)
 * @return the snprintf result: characters that would be written, or negative on failure
 */
static int describeMemoryChange(uint16_t code, const uint8_t * detail, size_t detail_length,
  char * out, size_t room)
{
  const bool two_bytes = detail_length >= report::kMemoryChangeDetailBytes;
  const bool bootloader = two_bytes && (detail[0] & report::kFoundByBootloader) != 0U;
  const uint8_t where = two_bytes ? static_cast<uint8_t>(detail[0] & ~report::kFoundByBootloader) : detail[0];
  const char * words = faultLocationName(where);
  int n = 0;
  if (words != nullptr) {
    n = std::snprintf(out, room, "EVENT 0x%04x (%s) at 0x%02x (%s)", code, eventName(code), where, words);
  } else {
    n = std::snprintf(out, room, "EVENT 0x%04x (%s) at unknown code 0x%02x", code, eventName(code), where);
  }
  if (!two_bytes || n < 0 || static_cast<size_t>(n) >= room) {
    return n;
  }
  const uint8_t later = detail[1];
  char * tail = out + n;
  const size_t tail_room = room - static_cast<size_t>(n);
  if (bootloader || later != 0U) {
    return std::snprintf(tail, tail_room, ", found by the %s, %u%s more after it",
      bootloader ? "bootloader" : "firmware", static_cast<unsigned>(later),
      later == report::kLaterFaultsMax ? " or" : "");
  }
  return std::snprintf(tail, tail_room, ", found by the firmware");
}

/**
 * @brief Writes the log text of one EVENT report: its code, its name and its decoded detail.
 *
 * Events 0x0002 and 0x0003 carry a 2-byte detail, refused copy (1) and check result (1), which
 * are decoded with slotName() and checkResultName() so the operator reads which copy failed which
 * check (Interfaces Sec. 5.4 event table and check-result table; REQ-040); describeFallback()
 * writes it. A detail of another length is shown as what it is, never padded. Event 0x000B shows its
 * detail byte (which test).
 * Event 0x0106 shows its reason byte (decision H, accepted by the owner, 13 Oct 2026). Event
 * 0x0001 shows its fault kind and event 0x0005 its fault location in words (owner's decisions 2
 * and 3, 13 Oct 2026; faultKindName(), faultLocationName()). Event 0x0005 has a second detail
 * byte and a bit 7 in the first, decoded by describeMemoryChange(). A value in no table prints as
 * "unknown code 0x.." with the number, never left out. Event 0x0104 shows its count and the onboard
 * times of the first and last dropped HEALTH report (owner decision 29, 15 Oct 2026; the detail
 * layout is a Proposal, TBR, protocol.hpp).
 *
 * @param[in]     code           the event code
 * @param[in]     detail         the detail bytes after the code; may be null when detail_length is 0
 * @param[in]     detail_length  number of detail bytes, 0 to report::kEventMaxBytes - kEventDetailOffset
 * @param[out]    out            receives the text, always terminated
 * @param[in]     room           size of out, in bytes (at least 1)
 * @return nothing
 */
void describeEvent(uint16_t code, const uint8_t * detail, size_t detail_length, char * out,
  size_t room)
{
  if (room == 0) {
    return;
  }
  out[0] = '\0';
  int n = 0;
  bool fallback = code == report::kEventFallbackPrevious || code == report::kEventFallbackLastResort;
  if (fallback) {
    n = describeFallback(code, detail, detail_length, out, room);
  } else if (code == report::kEventFaultDetected && detail_length >= 1) {
    const char * kind = faultKindName(detail[0]);
    if (kind != nullptr) {
      n = std::snprintf(out, room, "EVENT 0x%04x (%s), fault kind 0x%02x (%s)", code, eventName(code),
        detail[0], kind);
    } else {
      n = std::snprintf(out, room, "EVENT 0x%04x (%s), fault kind unknown code 0x%02x", code,
        eventName(code), detail[0]);
    }
  } else if (code == report::kEventMemoryChange && detail_length >= 1) {
    n = describeMemoryChange(code, detail, detail_length, out, room);
  } else if ((code == report::kEventTrialTestsFailed || code == report::kEventControllerRestarted) &&
             detail_length >= 1) {
    n = std::snprintf(out, room, "EVENT 0x%04x (%s), detail 0x%02x", code, eventName(code), detail[0]);
  } else if (code == report::kEventHealthDropped &&
             detail_length >= report::kHealthDroppedDetailBytes) {
    n = std::snprintf(out, room, "EVENT 0x%04x (%s), %u HEALTH reports, onboard time %u to %u",
      code, eventName(code), bytes::readU32(&detail[report::kDroppedCountAt]),
      bytes::readU32(&detail[report::kDroppedFirstTimeAt]),
      bytes::readU32(&detail[report::kDroppedLastTimeAt]));
  } else {
    n = std::snprintf(out, room, "EVENT 0x%04x (%s)", code, eventName(code));
  }
  if (n < 0) {
    out[0] = '\0';   // snprintf failed: no text rather than a half-written one
  }
}

/**
 * @brief Names a slot code of a HEALTH report, for the log.
 *
 * @param[in] slot  slot byte of a HEALTH report
 * @return "A", "B" or "last-resort"; "unknown" for any other value
 */
const char * slotName(uint8_t slot)
{
  switch (slot) {
    case 0x01: return "A";
    case 0x02: return "B";
    case 0x03: return "last-resort";
    default:   return "unknown";   // input from the link: a raw byte off the wire; any other value is unknown
  }
}

// ---- The store's file (owner decision 29, 15 Oct 2026; layout in report_store.hpp) ----------

namespace
{

// Record tags. Proposal (TBR), awaiting the owner's acceptance: any two values other than 0x00
// would do; 0x00 is what a hole or a write cut short reads as, so it is never a tag. The letters
// 'R' and 'S' read in a hex dump.
constexpr uint8_t kTagReport = 0x52U;
constexpr uint8_t kTagState = 0x53U;
// Framing of a record: tag (1) and body length (1) before the body, CRC-32 (4) after it.
constexpr size_t kHeadBytes = 2;
constexpr size_t kCrcBytes = 4;
constexpr size_t kRecordOverhead = kHeadBytes + kCrcBytes;
// The state body: six numbers of 4 bytes (report_store.hpp), at these offsets.
constexpr size_t kStateBytes = 24;
constexpr size_t kStateNextAt = 0;
constexpr size_t kStateDeletedBelowAt = 4;
constexpr size_t kStateDroppedThroughAt = 8;
constexpr size_t kStateDroppedCountAt = 12;
constexpr size_t kStateFirstTimeAt = 16;
constexpr size_t kStateLastTimeAt = 20;
// The shortest and longest records: an EVENT without detail (15 bytes, Sec. 5.4) and an EVENT
// with 32 bytes of detail (47), each plus the framing. Every loop over the file is bounded by
// the file's length over the shortest record.
constexpr size_t kRecordMinBytes = kRecordOverhead + report::kEventMinBytes;
constexpr size_t kRecordMaxBytes = kRecordOverhead + report::kEventMaxBytes;
static_assert(report::kEventMaxBytes <= 0xFFU, "a body length must fit its 1-byte field");
static_assert(kStateBytes <= report::kEventMaxBytes, "the state body must fit a record buffer");
static_assert(kStateBytes >= report::kEventMinBytes, "no record is shorter than kRecordMinBytes");
// Upper bound on pread()/pwrite() calls for one record. A call that makes progress moves at least
// one byte and a record is at most 53 bytes; the rest allows for interrupted calls (EINTR). The
// same bound as durable_record.cpp; no other source: bench choice.
constexpr size_t kMaxIoCalls = 128;
// Permission bits of the store's file: read and write for the owner only, as durable_record.cpp
// (bench choice, TBR: no source requires a value).
constexpr mode_t kFileMode = 0600U;
// The suffix of the file a rewrite goes to before the rename (as durable_record.cpp).
constexpr const char * kTempSuffix = ".tmp";

// What reading a record found.
enum class Read : uint8_t
{
  kRecord,    // a good record: length, CRC-32 and (for a report) the Sec. 5.4 kind all fit
  kEnd,       // the offset is the end of the good records
  kDamaged,   // the bytes there are no good record (a power cut, or damage)
  kError,     // the file could not be read; errno holds why
};

// One record read from the file.
struct Record
{
  uint8_t tag;                // kTagReport or kTagState
  size_t body_bytes;          // n, the body length
  size_t record_bytes;        // the whole record: framing and body
  std::array<uint8_t, report::kEventMaxBytes> body;
};

/**
 * @brief Reads size bytes at an offset of a file, with a bounded number of calls.
 *
 * pread() moves no file position, so reads at several offsets of one descriptor do not interfere.
 *
 * @param[in]     fd      file descriptor open for reading
 * @param[out]    out     receives the bytes
 * @param[in]     size    number of bytes, at most kRecordMaxBytes
 * @param[in]     offset  where they start in the file, in bytes
 * @return Read::kRecord: all read; Read::kDamaged: the file ended first; Read::kError: a read
 *         failed or the call bound was reached (errno holds why)
 */
Read readAt(int fd, uint8_t * out, size_t size, size_t offset)
{
  size_t have = 0;
  for (size_t i = 0; i < kMaxIoCalls && have < size; i++) {
    ssize_t n = ::pread(fd, &out[have], size - have, static_cast<off_t>(offset + have));
    if (n < 0 && errno == EINTR) {
      continue;   // interrupted before any byte moved: try again, counted by the bound
    }
    if (n < 0) {
      return Read::kError;
    }
    if (n == 0) {
      return Read::kDamaged;   // the file ends inside the record: it was cut short
    }
    have += static_cast<size_t>(n);
  }
  if (have != size) {
    errno = EIO;   // the bound was reached: a file system that keeps reading nothing
    return Read::kError;
  }
  return Read::kRecord;
}

/**
 * @brief Writes size bytes at an offset of a file, with a bounded number of calls.
 *
 * pwrite() at a given offset, not an append: a record always goes right after the last good
 * one, so the bytes of an earlier failed write there are overwritten, never built upon.
 *
 * @param[in]     fd      file descriptor open for writing, without O_APPEND
 * @param[in]     data    the bytes
 * @param[in]     size    number of bytes, at most kRecordMaxBytes
 * @param[in]     offset  where they go in the file, in bytes
 * @param[out]    error   errno value when false is returned; 0 otherwise
 * @return true: all written; false: a write failed or the call bound was reached
 */
bool writeAt(int fd, const uint8_t * data, size_t size, size_t offset, int & error)
{
  error = 0;
  size_t done = 0;
  for (size_t i = 0; i < kMaxIoCalls && done < size; i++) {
    ssize_t n = ::pwrite(fd, &data[done], size - done, static_cast<off_t>(offset + done));
    if (n < 0 && errno == EINTR) {
      continue;   // interrupted before any byte moved: try again, counted by the bound
    }
    if (n < 0) {
      error = errno;
      return false;
    }
    done += static_cast<size_t>(n);
  }
  if (done != size) {
    error = EIO;   // the bound was reached: a file system that keeps writing nothing
    return false;
  }
  return true;
}

/**
 * @brief Flushes a directory so that a rename in it survives a power cut.
 *
 * A rename is stored in the directory, not in the file; the file's own fsync does not cover it
 * (the same step as durable_record.cpp, whose helper is private to that file).
 *
 * @param[in]     directory  path of the directory
 * @param[out]    error      errno value when false is returned; 0 otherwise
 * @return true: flushed; false: the directory could not be opened or flushed
 */
bool syncDirectory(const char * directory, int & error)
{
  error = 0;
  int fd = ::open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    error = errno;
    return false;
  }
  bool ok = (::fsync(fd) == 0);
  error = ok ? 0 : errno;
  if (::close(fd) != 0 && ok) {
    error = errno;
    ok = false;
  }
  return ok;
}

/**
 * @brief Tells whether a body length read from the file fits the record's tag.
 *
 * A report body is 15 to 47 bytes (Sec. 5.4); a state body is kStateBytes. Any other tag is
 * damage: the byte was read from the file, so it is checked like input (code rules B4).
 *
 * @param[in]     tag         the tag byte
 * @param[in]     body_bytes  the body length byte
 * @return true: a length the tag can have; false: damage
 */
bool bodyLengthFits(uint8_t tag, size_t body_bytes)
{
  if (tag == kTagReport) {
    return body_bytes >= report::kEventMinBytes && body_bytes <= report::kEventMaxBytes;
  }
  if (tag == kTagState) {
    return body_bytes == kStateBytes;
  }
  return false;   // read from the file: any other tag is damage
}

/**
 * @brief Tells whether a report body has the kind and length of Sec. 5.4.
 *
 * The same check as ReportStore::accept() makes on a report from the controller: HEALTH is 24
 * bytes, EVENT 15 to 47.
 *
 * @param[in]     body        the report, at least report::kEventMinBytes long
 * @param[in]     body_bytes  its length
 * @return true: a HEALTH or EVENT report of a length Sec. 5.4 allows; false: damage
 */
bool reportFits(const uint8_t * body, size_t body_bytes)
{
  uint8_t kind = body[report::kOffsetKind];
  return (kind == report::kHealth && body_bytes == report::kHealthBytes) ||
         (kind == report::kEvent && body_bytes >= report::kEventMinBytes &&
          body_bytes <= report::kEventMaxBytes);
}

/**
 * @brief Reads and checks the record at an offset of the store's file.
 *
 * The head (tag, length) is read first and the length checked against the tag before any byte it
 * covers is read (code rules B4); then the body and the CRC-32 are read, the CRC-32 is compared,
 * and a report's kind and length are checked against Sec. 5.4. Only then is the body copied out.
 *
 * @param[in]     fd      the store's file, open for reading
 * @param[in]     offset  where the record starts, in bytes
 * @param[in]     end     the end of the bytes to read, in bytes (the file size, or the good end)
 * @param[out]    r       receives the record when Read::kRecord is returned
 * @return Read::kRecord, Read::kEnd (offset is end), Read::kDamaged or Read::kError
 */
Read readRecord(int fd, size_t offset, size_t end, Record & r)
{
  if (offset >= end) {
    return Read::kEnd;
  }
  if (end - offset < kHeadBytes) {
    return Read::kDamaged;
  }
  std::array<uint8_t, kRecordMaxBytes> raw{};
  Read got = readAt(fd, raw.data(), kHeadBytes, offset);
  if (got != Read::kRecord) {
    return got;
  }
  const size_t n = raw[1];
  if (!bodyLengthFits(raw[0], n) || end - offset < kRecordOverhead + n) {
    return Read::kDamaged;   // a length the tag cannot have, or a record cut short
  }
  got = readAt(fd, &raw[kHeadBytes], n + kCrcBytes, offset + kHeadBytes);
  if (got != Read::kRecord) {
    return got;
  }
  if (bytes::readU32(&raw[kHeadBytes + n]) != crc::crc32(raw.data(), kHeadBytes + n)) {
    return Read::kDamaged;
  }
  if (raw[0] == kTagReport && !reportFits(&raw[kHeadBytes], n)) {
    return Read::kDamaged;
  }
  r.tag = raw[0];
  r.body_bytes = n;
  r.record_bytes = kRecordOverhead + n;
  std::memcpy(r.body.data(), &raw[kHeadBytes], n);
  return Read::kRecord;
}

/**
 * @brief Builds one record: tag, body length, body, CRC-32 of the three.
 *
 * @param[in]     tag         kTagReport or kTagState
 * @param[in]     body        the body bytes
 * @param[in]     body_bytes  the body length, at most report::kEventMaxBytes
 * @param[out]    out         receives the record; room for kRecordMaxBytes
 * @return the record's length in bytes
 */
size_t encodeRecord(uint8_t tag, const uint8_t * body, size_t body_bytes, uint8_t * out)
{
  out[0] = tag;
  out[1] = static_cast<uint8_t>(body_bytes);
  std::memcpy(&out[kHeadBytes], body, body_bytes);
  bytes::writeU32(&out[kHeadBytes + body_bytes], crc::crc32(out, kHeadBytes + body_bytes));
  return kRecordOverhead + body_bytes;
}

}  // namespace

/**
 * @brief Builds a store with no file: reports are checked and numbered, nothing is stored.
 *
 * Numbering starts at 1; no HEALTH report is held. Before owner decision 29 (15 Oct 2026) this was
 * the only store, and the number started at 1 at every start of the program; the store on disk
 * (the other constructor and load()) now keeps the number across restarts (REQ-071, REQ-077).
 * @return nothing (constructor)
 */
ReportStore::ReportStore()
: next_number_(1), have_health_(false), health_{}, health_ms_(0), configured_(false),
  disk_(false), capacity_(0), file_bytes_(0), live_bytes_(0), deleted_below_(0),
  health_dropped_through_(0), dropped_{}, health_scan_from_(0), generation_(0),
  events_over_logged_(false), send_{}, directory_{}, path_{}, temp_path_{}
{
}

/**
 * @brief Builds a store kept in the file config::kReportStoreName of a directory; touches no file.
 *
 * The paths are built into fixed buffers (as durable_record.cpp). A directory that is null or
 * empty, a capacity of 0, or a path that does not fit leaves the store not configured: load()
 * then returns false and reports are not stored.
 *
 * @param[in]     directory       the data directory (--data-dir)
 * @param[in]     capacity_bytes  room for stored report records, in bytes
 *                                (config::kReportStoreBytes in the program)
 * @return nothing (constructor)
 */
ReportStore::ReportStore(const char * directory, size_t capacity_bytes)
: ReportStore()
{
  capacity_ = capacity_bytes;
  if (directory == nullptr || directory[0] == '\0' || capacity_bytes == 0) {
    return;
  }
  int dir_n = std::snprintf(directory_.data(), directory_.size(), "%s", directory);
  int path_n = std::snprintf(path_.data(), path_.size(), "%s/%s", directory,
    config::kReportStoreName);
  int temp_n = std::snprintf(temp_path_.data(), temp_path_.size(), "%s/%s%s", directory,
    config::kReportStoreName, kTempSuffix);
  configured_ = dir_n > 0 && path_n > 0 && temp_n > 0 &&
                static_cast<size_t>(dir_n) < directory_.size() &&
                static_cast<size_t>(path_n) < path_.size() &&
                static_cast<size_t>(temp_n) < temp_path_.size();   // snprintf did not cut
}

/**
 * @brief Checks one report from the controller (length and kind, Sec. 5.4) and writes the
 *        report for the ground: the same bytes with the report number and onboard time filled in.
 *
 * Reports that do not fit Sec. 5.4 are dropped and take no number. A HEALTH report is kept for
 * DELIVER's trial check (REQ-050) and for the test-image rule; an EVENT is logged with its name
 * from the Sec. 5.4 table, and the one detail byte of "trial tests failed" (0x000B, which test).
 * Every numbered report is then stored on disk when the store has a file (store(); owner decision
 * 29, 15 Oct 2026; REQ-074). Storing does not change the result: a report that could not be
 * stored still goes to the ground live (the failure is logged).
 *
 * @param[in]     payload        the report as the controller sent it
 * @param[in]     length         its size in bytes
 * @param[in]     onboard_time_s onboard computer clock time to stamp, in seconds
 * @param[in]     now_ms         arrival time, ms (clocks::nowMs), kept with a HEALTH report
 * @param[out]    out            the report for the ground
 * @param[in]     room           size of out, in bytes
 * @return true: the report fits Sec. 5.4 and out holds the report for the ground; false: dropped
 */
bool ReportStore::accept(const uint8_t * payload, size_t length, uint32_t onboard_time_s,
  uint64_t now_ms, uint8_t * out, size_t room)
{
  if (length <= report::kOffsetKind || length > room) {
    logging::line("controller: report of %zu bytes is too short or too long; dropped", length);
    return false;
  }
  uint8_t kind = payload[report::kOffsetKind];
  bool health = kind == report::kHealth && length == report::kHealthBytes;
  bool event = kind == report::kEvent && length >= report::kEventMinBytes &&
               length <= report::kEventMaxBytes;
  if (!health && !event) {
    logging::line("controller: report kind 0x%02x of %zu bytes does not fit Sec. 5.4; dropped",
      kind, length);
    return false;
  }

  std::memcpy(out, payload, length);
  bytes::writeU32(&out[report::kOffsetNumber], next_number_);
  bytes::writeU32(&out[report::kOffsetOnboardTime], onboard_time_s);
  next_number_ = (next_number_ == UINT32_MAX) ? 1U : next_number_ + 1U;

  if (health) {
    noteHealth(payload, now_ms);
  } else {
    uint16_t code = bytes::readU16(&payload[report::kOffsetKind + 1]);
    std::array<char, 192> text{};
    size_t detail_length = (length > report::kEventDetailOffset) ? length - report::kEventDetailOffset : 0;
    describeEvent(code, &payload[report::kEventDetailOffset], detail_length, text.data(), text.size());
    logging::line("controller: %s", text.data());
  }
  store(out, length);
  return true;
}

/**
 * @brief Keeps a HEALTH report and logs it when it differs from the last.
 *
 * Logs the first HEALTH report and every change of what it says, not every report: one a
 * second would bury everything else in the log. A change is a different mode, slot, firmware
 * kind, firmware version, failed-starts count or self-test result.
 *
 * @param[in] p  the HEALTH report as the controller sent it, already length-checked
 * @param[in] now_ms  arrival time, ms (clocks::nowMs)
 * @return nothing
 */
void ReportStore::noteHealth(const uint8_t * p, uint64_t now_ms)
{
  HealthReport h{};
  h.controller_time = bytes::readU32(&p[report::kOffsetControllerTime]);
  h.mode = p[report::kOffsetMode];
  h.firmware_kind = p[report::kOffsetFirmwareKind];
  h.firmware_version = bytes::readU32(&p[report::kOffsetFirmwareVersion]);
  h.slot = p[report::kOffsetSlot];
  h.failed_starts = p[report::kOffsetFailedStarts];
  h.trial_left_s = bytes::readU16(&p[report::kOffsetTrialLeft]);
  h.self_test = p[report::kOffsetSelfTest];

  bool changed = !have_health_ || h.mode != health_.mode || h.slot != health_.slot ||
                 h.firmware_kind != health_.firmware_kind ||
                 h.firmware_version != health_.firmware_version ||
                 h.failed_starts != health_.failed_starts || h.self_test != health_.self_test;
  if (changed) {
    logging::line("controller: HEALTH mode %s, slot %s, firmware kind 0x%02x v%u, failed starts "
      "%u, trial time left %u s, self-test 0x%02x", modeName(h.mode), slotName(h.slot),
      h.firmware_kind, h.firmware_version, h.failed_starts, h.trial_left_s, h.self_test);
  }
  health_ = h;
  have_health_ = true;
  health_ms_ = now_ms;
}

/**
 * @brief Tells whether a HEALTH report is held.
 * @return true: at least one HEALTH report was accepted; false: none yet
 */
bool ReportStore::haveHealth() const
{
  return have_health_;
}

/**
 * @brief Gives the last HEALTH report.
 *
 * Meaningful only when haveHealth() is true.
 * @return reference to the last HEALTH report, valid until the next accepted HEALTH report
 */
const HealthReport & ReportStore::lastHealth() const
{
  return health_;
}

/**
 * @brief Tells when the last HEALTH report arrived.
 *
 * When it arrived (clocks::nowMs).
 * @return arrival time of the last HEALTH report, ms; 0 before the first
 */
uint64_t ReportStore::lastHealthMs() const
{
  return health_ms_;
}

/**
 * @brief Tells the last report number given.
 *
 * The last number given; 0 before the first.
 * @return the last report number given
 */
uint32_t ReportStore::lastNumber() const
{
  return next_number_ - 1U;
}

// ---- The store on disk (owner decision 29, 15 Oct 2026; REQ-074 to REQ-078) ------------------

/**
 * @brief Tells whether the store on disk is in use.
 *
 * @return true: load() succeeded and reports are stored; false: they are not
 */
bool ReportStore::onDisk() const
{
  return disk_;
}

/**
 * @brief Gives the number the next report gets.
 *
 * At SEND REPORTS it marks the end of the contact: reports numbered from it on go to the ground
 * live and are not sent again from the store in that contact.
 *
 * @return the next report number
 */
uint32_t ReportStore::nextNumber() const
{
  return next_number_;
}

/**
 * @brief Gives the dropped-HEALTH counters and sets them to zero.
 *
 * Called at SEND REPORTS, after deleteBelow(), to make EVENT 0x0104 (REQ-076). The counters are
 * zero in RAM at once, so HEALTH reports dropped while EVENT 0x0104 itself is stored count towards
 * the next contact. The zero reaches the file only with startSend(), after EVENT 0x0104 is stored:
 * a power cut between the two makes the next contact report the same drops again, beside the
 * stored EVENT 0x0104. The ground is then told twice, never not at all (TBR, awaiting the owner's
 * acceptance).
 *
 * @return the counters as they were
 */
DroppedHealth ReportStore::takeDropped()
{
  const DroppedHealth taken = dropped_;
  dropped_ = DroppedHealth{};
  return taken;
}

/**
 * @brief Tells whether a report in the file is still stored.
 *
 * A report numbered below deleted_below_ was deleted at a SEND REPORTS. A HEALTH report numbered
 * up to health_dropped_through_ was dropped from the full store: the oldest go first, so the
 * dropped ones are exactly those (REQ-075). Events are never dropped.
 *
 * @param[in]     number  the report number
 * @param[in]     kind    report::kHealth or report::kEvent
 * @return true: stored; false: deleted or dropped
 */
bool ReportStore::live(uint32_t number, uint8_t kind) const
{
  if (number < deleted_below_) {
    return false;
  }
  return kind != report::kHealth || number > health_dropped_through_;
}

/**
 * @brief Counts one dropped HEALTH report for EVENT 0x0104 (REQ-076).
 *
 * The count stops at UINT32_MAX rather than wrap to a small number. The first time is set when
 * the count leaves 0; the last time always.
 *
 * @param[in]     onboard_time_s  the dropped report's onboard computer clock time, in seconds
 * @return nothing
 */
void ReportStore::countDropped(uint32_t onboard_time_s)
{
  if (dropped_.count == 0U) {
    dropped_.first_time_s = onboard_time_s;
  }
  if (dropped_.count < UINT32_MAX) {
    dropped_.count++;
  }
  dropped_.last_time_s = onboard_time_s;
}

/**
 * @brief Stores one numbered report on disk, dropping the oldest HEALTH reports when full.
 *
 * Owner decision 29 (REQ-074, REQ-075). When the report would take the stored report records past
 * capacity_, the oldest stored HEALTH reports are dropped, as many as needed (dropOldestHealth).
 * If that is not enough, only events are left: an EVENT is stored all the same and the store
 * passes its size, since events are never dropped; a HEALTH is not stored, is counted as dropped
 * and the count is written at once as a state record (TBR, awaiting the owner's acceptance). Before
 * the append, the file is rewritten when the space in it that stores nothing has passed
 * config::kReportDeadMaxBytes (compact()).
 *
 * @param[in]     numbered  the report as it goes to the ground, numbered and stamped (accept())
 * @param[in]     length    its length in bytes, 15 to 47, checked by accept()
 * @return nothing; a failure is logged, and the report still goes to the ground live
 */
void ReportStore::store(const uint8_t * numbered, size_t length)
{
  if (!disk_) {
    return;
  }
  const size_t record_bytes = kRecordOverhead + length;
  if (live_bytes_ + record_bytes > capacity_) {
    dropOldestHealth(live_bytes_ + record_bytes - capacity_);
    if (live_bytes_ + record_bytes > capacity_) {
      if (!events_over_logged_) {
        logging::line("reports: events alone fill the store (%zu of %zu bytes): new events are "
          "kept past it, new HEALTH reports are not stored and count as dropped", live_bytes_,
          capacity_);
        events_over_logged_ = true;
      }
      if (numbered[report::kOffsetKind] == report::kHealth) {
        countDropped(bytes::readU32(&numbered[report::kOffsetOnboardTime]));
        (void)appendState();   // the count survives a power cut; a failure is logged there
        return;
      }
    }
  }
  if (file_bytes_ > live_bytes_ + config::kReportDeadMaxBytes) {
    (void)compact();   // a failure is logged; the record then goes to the end of the old file
  }
  if (appendRecord(kTagReport, numbered, length)) {
    live_bytes_ += record_bytes;
  }
}

/**
 * @brief Drops the oldest stored HEALTH reports until need bytes are freed, or none is left.
 *
 * REQ-075: the oldest HEALTH first, events kept. Reads the file from health_scan_from_ (no stored
 * HEALTH lies before it) in file order, which is number order, and drops each stored HEALTH by
 * moving health_dropped_through_ to its number; each is counted with its onboard time (REQ-076).
 * Nothing is rewritten: one state record is appended, so the drop survives a power cut
 * (REQ-077). Only the first drop since the counters were last taken is logged; at one HEALTH
 * report a second a line per drop would bury the rest of the log.
 *
 * @param[in]     need  bytes of stored report records to free
 * @return nothing; live_bytes_ is lower by what was freed
 */
void ReportStore::dropOldestHealth(size_t need)
{
  int fd = ::open(path_.data(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    logging::line("reports: cannot open %s to drop HEALTH reports: %s", path_.data(),
      std::strerror(errno));
    return;
  }
  const bool first_drop = dropped_.count == 0U;
  size_t offset = health_scan_from_;
  size_t freed = 0;
  uint32_t through = health_dropped_through_;
  const size_t bound = file_bytes_ / kRecordMinBytes + 1U;
  Record r{};
  for (size_t i = 0; i < bound && freed < need; i++) {
    Read got = readRecord(fd, offset, file_bytes_, r);
    if (got != Read::kRecord) {
      if (got != Read::kEnd) {
        logging::line("reports: cannot read the store at offset %zu to drop HEALTH reports",
          offset);
      }
      break;   // the end: no stored HEALTH is left
    }
    if (r.tag == kTagReport && r.body[report::kOffsetKind] == report::kHealth) {
      const uint32_t number = bytes::readU32(&r.body[report::kOffsetNumber]);
      if (live(number, report::kHealth)) {
        through = number;
        countDropped(bytes::readU32(&r.body[report::kOffsetOnboardTime]));
        freed += r.record_bytes;
      }
    }
    offset += r.record_bytes;
  }
  (void)::close(fd);   // read only: nothing to recover from a failed close
  if (freed == 0) {
    return;
  }
  health_dropped_through_ = through;
  health_scan_from_ = offset;
  live_bytes_ -= freed;
  if (first_drop) {
    logging::line("reports: store full (%zu bytes): the oldest HEALTH reports are dropped, through "
      "number %u so far (REQ-075); counted, not logged, until SEND REPORTS", capacity_, through);
  }
  (void)appendState();   // a failure is logged there
}

/**
 * @brief Appends one record after the last good one and flushes it.
 *
 * One pwrite() at file_bytes_, then fsync, before the record counts: a power cut spoils at most
 * this record, which load() then cuts off (REQ-077). The file is opened without O_CREAT: load()
 * made it, and one that has vanished is not made again with a hole before this record. A failed
 * write leaves file_bytes_ as it was, so the next record overwrites what it left.
 *
 * @param[in]     tag         kTagReport or kTagState
 * @param[in]     body        the body bytes
 * @param[in]     body_bytes  the body length, at most report::kEventMaxBytes
 * @return true: written and flushed; false: not (logged)
 */
bool ReportStore::appendRecord(uint8_t tag, const uint8_t * body, size_t body_bytes)
{
  if (body_bytes > report::kEventMaxBytes) {
    logging::line("reports: a record body of %zu bytes is longer than %zu; not stored", body_bytes,
      report::kEventMaxBytes);
    return false;
  }
  std::array<uint8_t, kRecordMaxBytes> raw{};
  const size_t size = encodeRecord(tag, body, body_bytes, raw.data());
  int error = 0;
  int fd = ::open(path_.data(), O_WRONLY | O_CLOEXEC);
  bool ok = fd >= 0;
  if (!ok) {
    error = errno;
  }
  ok = ok && writeAt(fd, raw.data(), size, file_bytes_, error);
  if (ok && ::fsync(fd) != 0) {
    error = errno;
    ok = false;
  }
  if (fd >= 0 && ::close(fd) != 0 && ok) {
    error = errno;
    ok = false;
  }
  if (!ok) {
    logging::line("reports: a record of %zu bytes could not be stored at offset %zu of %s: %s",
      size, file_bytes_, path_.data(), std::strerror(error));
    return false;
  }
  file_bytes_ += size;
  return true;
}

/**
 * @brief Writes the six numbers of the state record (layout in report_store.hpp).
 *
 * @param[out]    body  room for kStateBytes
 * @return nothing
 */
void ReportStore::stateBody(uint8_t * body) const
{
  bytes::writeU32(&body[kStateNextAt], next_number_);
  bytes::writeU32(&body[kStateDeletedBelowAt], deleted_below_);
  bytes::writeU32(&body[kStateDroppedThroughAt], health_dropped_through_);
  bytes::writeU32(&body[kStateDroppedCountAt], dropped_.count);
  bytes::writeU32(&body[kStateFirstTimeAt], dropped_.first_time_s);
  bytes::writeU32(&body[kStateLastTimeAt], dropped_.last_time_s);
}

/**
 * @brief Appends a state record: the next number, the deletion, the drops and their counters.
 *
 * @return true: written and flushed; false: not (logged by appendRecord)
 */
bool ReportStore::appendState()
{
  std::array<uint8_t, kStateBytes> body{};
  stateBody(body.data());
  return appendRecord(kTagState, body.data(), body.size());
}

/**
 * @brief Rewrites the file with a state record and the stored reports only.
 *
 * Reclaims the space of deleted and dropped reports and of old state records. The order of
 * durable_record.cpp: the new file to "<name>.tmp", flush, rename over the file, flush the
 * directory; a power cut leaves the old file or the new one (REQ-077). A failure before the rename
 * keeps the old file as it is and removes the temporary one. After the rename the new file is in
 * force even if the directory flush fails (logged); a power cut may then bring back the old file,
 * which is still good, only older.
 *
 * @return true: the new file is in force; false: the old file is kept (logged)
 */
bool ReportStore::compact()
{
  int error = 0;
  size_t written = 0;
  size_t stored = 0;
  int fd = ::open(temp_path_.data(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kFileMode);
  bool ok = fd >= 0;
  if (!ok) {
    error = errno;
  }
  ok = ok && writeCompacted(fd, written, stored, error);
  if (ok && ::fsync(fd) != 0) {
    error = errno;
    ok = false;
  }
  if (fd >= 0 && ::close(fd) != 0 && ok) {
    error = errno;
    ok = false;
  }
  if (ok && ::rename(temp_path_.data(), path_.data()) != 0) {
    error = errno;
    ok = false;
  }
  if (!ok) {
    (void)::unlink(temp_path_.data());   // best effort: load() removes a leftover at the next start
    logging::line("reports: rewrite of %s failed: %s; the file is kept as it was", path_.data(),
      std::strerror(error));
    return false;
  }
  generation_++;
  file_bytes_ = written;
  live_bytes_ = stored;
  health_scan_from_ = 0;
  if (!syncDirectory(directory_.data(), error)) {
    logging::line("reports: %s rewritten, but its directory could not be flushed: %s; a power cut "
      "may bring back the older file", path_.data(), std::strerror(error));
  }
  return true;
}

/**
 * @brief Writes the new file of a rewrite: a state record, then every stored report in order.
 *
 * The reports are read from the file as it is (up to file_bytes_), checked again by readRecord,
 * and copied when live(). Anything that is not a good record stops the rewrite, so the old file is
 * kept rather than replaced by a shorter one.
 *
 * @param[in]     out_fd   the temporary file, open for writing, empty
 * @param[out]    written  bytes written to it
 * @param[out]    stored   of those, bytes of stored report records
 * @param[out]    error    errno value when false is returned
 * @return true: written; false: a read or a write failed
 */
bool ReportStore::writeCompacted(int out_fd, size_t & written, size_t & stored, int & error)
{
  std::array<uint8_t, kStateBytes> state{};
  stateBody(state.data());
  std::array<uint8_t, kRecordMaxBytes> raw{};
  size_t size = encodeRecord(kTagState, state.data(), state.size(), raw.data());
  written = 0;
  stored = 0;
  if (!writeAt(out_fd, raw.data(), size, 0, error)) {
    return false;
  }
  written = size;
  if (file_bytes_ == 0) {
    return true;   // a new file: the state only
  }
  int in_fd = ::open(path_.data(), O_RDONLY | O_CLOEXEC);
  if (in_fd < 0) {
    error = errno;
    return false;
  }
  bool ok = true;
  size_t offset = 0;
  const size_t bound = file_bytes_ / kRecordMinBytes + 1U;
  Record r{};
  for (size_t i = 0; i < bound && ok && offset < file_bytes_; i++) {
    Read got = readRecord(in_fd, offset, file_bytes_, r);
    if (got != Read::kRecord) {
      error = (got == Read::kError) ? errno : EIO;   // the good records changed under us
      ok = false;
      break;
    }
    if (r.tag == kTagReport &&
               live(bytes::readU32(&r.body[report::kOffsetNumber]), r.body[report::kOffsetKind])) {
      size = encodeRecord(kTagReport, r.body.data(), r.body_bytes, raw.data());
      ok = writeAt(out_fd, raw.data(), size, written, error);
      written += size;
      stored += size;
    }
    offset += r.record_bytes;
  }
  (void)::close(in_fd);   // read only: nothing to recover from a failed close
  return ok;
}

/**
 * @brief Loads the store's file at start: the state in force, the stored reports, the next number.
 *
 * Owner decision 29 (REQ-077). A leftover "<name>.tmp" (a rewrite cut short) is removed; the file
 * itself is whole. No file (first start): a new one is written with the state only. Otherwise the
 * records are read in order (loadScan); the file is cut (ftruncate, flushed) at the first record
 * that is cut short or fails its checks, logged with the offset and the bytes cut, and the records
 * before it are kept. A cut that fails is logged and not fatal: the next record is written at the
 * good end over the damage. Then the stored reports are counted (countLive). A file that cannot be
 * opened or read is left as it is and the store is not used in this run; reports still go to the
 * ground live (TBR, awaiting the owner's acceptance).
 *
 * @return true: the store is in use; false: it is not (logged)
 */
bool ReportStore::load()
{
  if (!configured_) {
    logging::line("reports: no usable data directory for the report store; reports are not stored");
    return false;
  }
  (void)::unlink(temp_path_.data());   // a rewrite cut short; ENOENT is the usual answer
  int fd = ::open(path_.data(), O_RDWR | O_CLOEXEC);
  if (fd < 0 && errno == ENOENT) {
    disk_ = compact();
    if (disk_) {
      logging::line("reports: no store file; %s made, empty, next report number %u", path_.data(),
        next_number_);
    }
    return disk_;
  }
  if (fd < 0) {
    logging::line("reports: cannot open %s: %s; reports are not stored", path_.data(),
      std::strerror(errno));
    return false;
  }
  struct stat info{};
  size_t good_end = 0;
  bool ok = ::fstat(fd, &info) == 0 && info.st_size >= 0 &&
            loadScan(fd, static_cast<size_t>(info.st_size), good_end);
  int error = ok ? 0 : errno;
  if (ok && good_end < static_cast<size_t>(info.st_size)) {
    const size_t cut = static_cast<size_t>(info.st_size) - good_end;
    logging::line("reports: damaged tail of %zu bytes cut off at offset %zu of %s (a power cut "
      "while a record was written, or damage)", cut, good_end, path_.data());
    if (::ftruncate(fd, static_cast<off_t>(good_end)) != 0 || ::fsync(fd) != 0) {
      logging::line("reports: the cut failed: %s; the next record is written over the damage",
        std::strerror(errno));
    }
  }
  (void)::close(fd);   // the cut, if any, was flushed; nothing to recover from a failed close
  if (!ok) {
    logging::line("reports: cannot read %s: %s; reports are not stored", path_.data(),
      std::strerror(error));
    return false;
  }
  file_bytes_ = good_end;
  disk_ = countLive();
  return disk_;
}

/**
 * @brief Reads the records of the file in order, up to the first that is not good.
 *
 * Applies each state record (the last is in force) and notes the highest report number, so the
 * numbering goes on after it (REQ-071, REQ-077): the next number is the last state's, or one past
 * the highest stored report when that is higher (reports appended after the last state record).
 * A Read value outside the four is corrupted memory (code rules B1); the updater has no fault
 * reaction yet (open), so it is treated as a failed read: the file is not cut.
 *
 * @param[in]     fd         the file, open for reading
 * @param[in]     file_size  its size in bytes
 * @param[out]    good_end   the end of the good records, in bytes
 * @return true: read to the end or to a record that is not good; false: a read failed (errno)
 */
bool ReportStore::loadScan(int fd, size_t file_size, size_t & good_end)
{
  good_end = 0;
  uint32_t highest = 0;
  uint32_t state_next = 1U;
  const size_t bound = file_size / kRecordMinBytes + 1U;
  Record r{};
  for (size_t i = 0; i < bound; i++) {
    Read got = readRecord(fd, good_end, file_size, r);
    if (got == Read::kEnd || got == Read::kDamaged) {
      break;   // the end, or the first record that is not good: the tail from here is cut
    }
    if (got != Read::kRecord) {
      return false;   // Read::kError, or a value that cannot exist (see above)
    }
    if (r.tag == kTagState) {
      state_next = bytes::readU32(&r.body[kStateNextAt]);
      deleted_below_ = bytes::readU32(&r.body[kStateDeletedBelowAt]);
      health_dropped_through_ = bytes::readU32(&r.body[kStateDroppedThroughAt]);
      dropped_.count = bytes::readU32(&r.body[kStateDroppedCountAt]);
      dropped_.first_time_s = bytes::readU32(&r.body[kStateFirstTimeAt]);
      dropped_.last_time_s = bytes::readU32(&r.body[kStateLastTimeAt]);
    } else {
      const uint32_t number = bytes::readU32(&r.body[report::kOffsetNumber]);
      highest = (number > highest) ? number : highest;
    }
    good_end += r.record_bytes;
  }
  next_number_ = (highest >= state_next) ? highest + 1U : state_next;
  if (next_number_ == 0U) {
    next_number_ = 1U;   // past UINT32_MAX, as accept() wraps; 0 is never a report number
  }
  return true;
}

/**
 * @brief Counts the stored reports in the file with the state in force, and logs the store.
 *
 * Sets live_bytes_ (the stored report records, framing included) from the records up to
 * file_bytes_, which were read and found good before; one that is not good now means the file
 * changed under the updater or cannot be read.
 *
 * @return true: counted; false: the file could not be read (logged)
 */
bool ReportStore::countLive()
{
  int fd = ::open(path_.data(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    logging::line("reports: cannot open %s: %s", path_.data(), std::strerror(errno));
    return false;
  }
  size_t stored = 0;
  uint32_t events = 0;
  uint32_t health = 0;
  size_t offset = 0;
  bool ok = true;
  const size_t bound = file_bytes_ / kRecordMinBytes + 1U;
  Record r{};
  for (size_t i = 0; i < bound && ok && offset < file_bytes_; i++) {
    if (readRecord(fd, offset, file_bytes_, r) != Read::kRecord) {
      ok = false;
      break;
    }
    const uint8_t kind = r.body[report::kOffsetKind];
    if (r.tag == kTagReport && live(bytes::readU32(&r.body[report::kOffsetNumber]), kind)) {
      stored += r.record_bytes;
      events += (kind == report::kEvent) ? 1U : 0U;
      health += (kind == report::kHealth) ? 1U : 0U;
    }
    offset += r.record_bytes;
  }
  (void)::close(fd);   // read only: nothing to recover from a failed close
  if (!ok) {
    logging::line("reports: %s could not be read again at offset %zu", path_.data(), offset);
    return false;
  }
  live_bytes_ = stored;
  logging::line("reports: store %s: %u EVENT and %u HEALTH reports stored (%zu of %zu bytes), "
    "next report number %u, %u dropped HEALTH reports not yet reported", path_.data(), events,
    health, live_bytes_, capacity_, next_number_, dropped_.count);
  return true;
}

/**
 * @brief Deletes the stored reports numbered below first: the ground has them.
 *
 * SEND REPORTS (owner decision 29; REQ-074): the ground asked from first on, so it has the
 * reports below it (TBR, awaiting the owner's acceptance). Done before EVENT 0x0104 is stored, so
 * the room it frees is not taken by dropping HEALTH reports the ground still lacks. The deletion
 * only moves up: a lower first deletes nothing more and brings nothing back. The file is rewritten
 * (compact()); if that fails, a state record is appended instead and the stored reports are
 * counted again. The dropped-HEALTH counters are written as they are, not yet zeroed.
 *
 * @param[in]     first  the first report number of SEND REPORTS
 * @return nothing
 */
void ReportStore::deleteBelow(uint32_t first)
{
  if (!disk_) {
    return;
  }
  if (first > deleted_below_) {
    deleted_below_ = first;
  }
  events_over_logged_ = false;
  if (!compact()) {
    (void)appendState();   // the deletion survives a power cut all the same
    (void)countLive();     // live_bytes_ without the deleted reports; a failure is logged there
  }
}

/**
 * @brief Starts the sending of a contact's stored reports.
 *
 * SEND REPORTS (owner decision 29; REQ-074, REQ-078), after deleteBelow(), takeDropped() and
 * EVENT 0x0104. First a state record is appended: it writes the counters zeroed by takeDropped(),
 * now that EVENT 0x0104 is stored. Then nextToSend() gives the stored reports numbered first to
 * end - 1, all EVENT reports first, then the HEALTH reports, each in number order. A contact still
 * running is replaced by the new one.
 *
 * @param[in]     first  the first report number of SEND REPORTS
 * @param[in]     end    nextNumber() when SEND REPORTS arrived: the later reports went live
 * @return nothing
 */
void ReportStore::startSend(uint32_t first, uint32_t end)
{
  send_ = SendCursor{};
  if (!disk_) {
    return;
  }
  (void)appendState();   // a failure is logged there; the next state record writes the zero
  send_.active = true;
  send_.first = first;
  send_.end = end;
  send_.generation = generation_;
  logging::line("reports: SEND REPORTS from number %u: sending the stored reports numbered below "
    "%u, EVENT reports first (%zu bytes stored)", first, end, live_bytes_);
}

/**
 * @brief Tells whether a contact's stored reports are still being sent.
 *
 * @return true: nextToSend() has reports to give; false: none is being sent
 */
bool ReportStore::sending() const
{
  return send_.active;
}

/**
 * @brief Finds the next stored report of the running contact and copies it out.
 *
 * Reads on from where the last search stopped. A report is due when it is stored (live()), its
 * number is first to end - 1 and above the last one sent in this pass, and its kind is the pass's:
 * EVENT in the first pass, HEALTH in the second (REQ-078). When the first pass reaches the end of
 * the file the second starts at the beginning; when the second does, the contact has ended. If
 * the file was rewritten meanwhile, the search starts again at the beginning and the number of the
 * last one sent skips what has gone. The report counts as sent only when markSent() is called, so
 * one the ground link did not take is given again.
 *
 * @param[out]    out     receives the report as it goes to the ground
 * @param[in]     room    size of out, in bytes (report::kEventMaxBytes is enough)
 * @param[out]    length  its length in bytes; 0 when false is returned
 * @return true: a report is in out; false: none is left, or the file cannot be read (logged)
 */
bool ReportStore::nextToSend(uint8_t * out, size_t room, size_t & length)
{
  length = 0;
  if (!send_.active) {
    return false;
  }
  if (send_.generation != generation_) {
    send_.offset = 0;
    send_.generation = generation_;
  }
  int fd = ::open(path_.data(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    stopSend("the store's file cannot be opened");
    return false;
  }
  const size_t bound = 2U * (file_bytes_ / kRecordMinBytes + 1U);   // two passes over the file
  const char * why = "the search bound was reached";
  bool found = false;
  bool ended = false;
  Record r{};
  for (size_t i = 0; i < bound && !found && !ended; i++) {
    Read got = readRecord(fd, send_.offset, file_bytes_, r);
    if (got == Read::kRecord) {
      const uint32_t number = bytes::readU32(&r.body[report::kOffsetNumber]);
      const uint8_t kind = r.body[report::kOffsetKind];
      const uint8_t due_kind = send_.health_pass ? report::kHealth : report::kEvent;
      found = r.tag == kTagReport && kind == due_kind && number >= send_.first &&
              number < send_.end && number > send_.last_sent && live(number, kind);
      send_.offset += found ? 0U : r.record_bytes;
    } else if (got == Read::kEnd && !send_.health_pass) {
      send_.health_pass = true;   // every EVENT is out: now the HEALTH reports (REQ-078)
      send_.offset = 0;
      send_.last_sent = 0;
    } else {
      why = (got == Read::kEnd) ? "all sent" : "the store's file cannot be read";
      ended = true;
    }
  }
  (void)::close(fd);   // read only: nothing to recover from a failed close
  if (!found || r.body_bytes > room) {
    stopSend(found ? "a stored report is longer than the buffer" : why);
    return false;
  }
  std::memcpy(out, r.body.data(), r.body_bytes);
  length = r.body_bytes;
  send_.pending_next = send_.offset + r.record_bytes;
  send_.pending_number = bytes::readU32(&r.body[report::kOffsetNumber]);
  return true;
}

/**
 * @brief Notes that the report nextToSend() gave went to the ground.
 *
 * @return nothing
 */
void ReportStore::markSent()
{
  if (!send_.active) {
    return;
  }
  send_.offset = send_.pending_next;
  send_.last_sent = send_.pending_number;
  send_.failures = 0;
  if (send_.health_pass) {
    send_.health_sent++;
  } else {
    send_.events_sent++;
  }
}

/**
 * @brief Notes that the ground link did not take the report nextToSend() gave.
 *
 * The report is given again on the next call. The first failure in a row is logged; after
 * config::kStoredSendTriesMax in a row the sending stops (B2: a bounded wait with a defined end).
 * The reports stay stored, and the ground asks again with SEND REPORTS.
 *
 * @return nothing
 */
void ReportStore::noteSendFailed()
{
  if (!send_.active) {
    return;
  }
  send_.failures++;
  if (send_.failures == 1U) {
    logging::line("reports: the ground link did not take stored report %u; trying again",
      send_.pending_number);
  }
  if (send_.failures >= config::kStoredSendTriesMax) {
    logging::line("reports: stored report %u not taken %u times; SEND REPORTS again from a number "
      "at or below it", send_.pending_number, send_.failures);
    stopSend("the ground link did not take a report");
  }
}

/**
 * @brief Ends the sending of a contact and logs what went.
 *
 * @param[in]     why  the reason, for the log
 * @return nothing
 */
void ReportStore::stopSend(const char * why)
{
  logging::line("reports: sending ended, %s: %u EVENT and %u HEALTH reports sent from the store",
    why, send_.events_sent, send_.health_sent);
  send_.active = false;
}
