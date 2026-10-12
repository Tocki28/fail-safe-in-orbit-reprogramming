#include "report_store.hpp"

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
    default:                    return "unknown";
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
    case 0x0009: return "restart";
    case report::kEventTrialTestsPassed: return "trial tests passed";
    case report::kEventTrialTestsFailed: return "trial tests failed";
    case 0x0101: return "data link lost";
    case 0x0102: return "data link working again";
    case 0x0103: return "restore done";
    case 0x0104: return "health reports dropped";
    case 0x0105: return "onboard computer clock differs from ground time";
    default:     return "unknown";
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
    default:   return "unknown";
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
    if (code == report::kEventTrialTestsFailed && length > report::kEventDetailOffset) {
      logging::line("controller: EVENT 0x%04x (%s), detail 0x%02x", code, eventName(code),
        payload[report::kEventDetailOffset]);
    } else {
      logging::line("controller: EVENT 0x%04x (%s)", code, eventName(code));
    }
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
