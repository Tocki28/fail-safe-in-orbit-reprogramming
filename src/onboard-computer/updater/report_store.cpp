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
    case 0x0001: return "fault detected";
    case 0x0002: return "fall-back to the previous copy";
    case 0x0003: return "fall-back to the last-resort firmware";
    case 0x0004: return "self-test failed";
    case 0x0005: return "change in memory detected";
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
 * @param[in]     result  the check-result byte of the detail
 * @return the check that failed ("none: the copy passed" for 0x00); "unknown" for a value not in the table
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
    default:   return "unknown";   // input from the link: a raw byte off the wire; any other value is unknown
  }
}

/**
 * @brief Writes the log text of one EVENT report: its code, its name and its decoded detail.
 *
 * Events 0x0002 and 0x0003 carry a 2-byte detail, refused copy (1) and check result (1), which
 * are decoded with slotName() and checkResultName() so the operator reads which copy failed which
 * check (Interfaces Sec. 5.4 event table and check-result table; REQ-040). A detail of another
 * length is shown as what it is, never padded. Event 0x000B shows its detail byte (which test).
 * Event 0x0106 shows its reason byte (decision H, accepted by the owner, 13 Oct 2026).
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
  if (fallback && detail_length >= report::kFallbackDetailBytes) {
    n = std::snprintf(out, room, "EVENT 0x%04x (%s), refused copy 0x%02x (%s), check result 0x%02x (%s)",
      code, eventName(code), detail[0], slotName(detail[0]), detail[1], checkResultName(detail[1]));
  } else if (fallback) {
    n = std::snprintf(out, room, "EVENT 0x%04x (%s), detail of %zu byte(s) instead of the 2 of Sec. 5.4%s",
      code, eventName(code), detail_length, (detail_length == 1) ? ": refused copy only, no check result" : "");
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
