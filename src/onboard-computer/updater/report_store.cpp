#include "report_store.hpp"

#include <array>
#include <cstdio>
#include <cstring>

#include "bytes.hpp"
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
constexpr std::array<CodeText, 25> kFaultLocations = {{
  {0x01U, "bootloader backup: pair number past the last pair"},                 // VALUE_INDEX
  {0x02U, "bootloader: slot code is not A, B or none"},                         // SLOT_LETTER
  {0x03U, "bootloader: reset cause outside its list"},                          // RESET_CAUSE_TEXT
  {0x04U, "bootloader: boot state load result outside its list"},               // LOAD_STATE
  {0x05U, "bootloader: slot code is not A or B"},                               // SLOT_REGION
  {0x06U, "bootloader: start role is not confirmed, trial or last resort"},     // ROLE_TEXT
  {0x07U, "bootloader: image check result outside its list"},                   // CHECK_RESULT_CODE
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
 * too, never dropped). The location is the lower seven bits, named with faultLocationName(); one
 * in no table prints as "unknown code 0x.." with the number. With one byte only the text is as
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
    return std::snprintf(tail, tail_room, ", found by the %s, %u more after it",
      bootloader ? "bootloader" : "firmware", static_cast<unsigned>(later));
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
 * "unknown code 0x.." with the number, never left out.
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

/**
 * @brief Builds an empty store.
 *
 * Report numbering starts at 1 (report_store.hpp: the number starts at 1 at every start of the
 * program); no HEALTH report is held.
 * @return nothing (constructor)
 */
ReportStore::ReportStore()
: next_number_(1), have_health_(false), health_{}, health_ms_(0)
{
}

/**
 * @brief Checks one report from the controller (length and kind, Sec. 5.4) and writes the
 *        report for the ground: the same bytes with the report number and onboard time filled in.
 *
 * Reports that do not fit Sec. 5.4 are dropped and take no number. A HEALTH report is kept for
 * DELIVER's trial check (REQ-050) and for the test-image rule; an EVENT is logged with its name
 * from the Sec. 5.4 table, and the one detail byte of "trial tests failed" (0x000B, which test).
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
