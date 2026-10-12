#include "report_store.hpp"

#include <cstring>

#include "bytes.hpp"
#include "log.hpp"
#include "protocol.hpp"

const char * modeName(uint8_t mode)
{
  switch (mode) {
    case report::kModeOperate:  return "OPERATE";
    case report::kModeTrial:    return "TRIAL";
    case report::kModeFallback: return "FALLBACK";
    default:                    return "unknown";
  }
}

const char * slotName(uint8_t slot)
{
  switch (slot) {
    case 0x01: return "A";
    case 0x02: return "B";
    case 0x03: return "last-resort";
    default:   return "unknown";
  }
}

ReportStore::ReportStore()
: next_number_(1), have_health_(false), health_{}, health_ms_(0)
{
}

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
    logging::line("controller: EVENT 0x%04x", bytes::readU16(&payload[report::kOffsetKind + 1]));
  }
  return true;
}

// Logs the first HEALTH report and every change of what it says, not every report: one a
// second would bury everything else in the log.
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

bool ReportStore::haveHealth() const
{
  return have_health_;
}

const HealthReport & ReportStore::lastHealth() const
{
  return health_;
}

uint64_t ReportStore::lastHealthMs() const
{
  return health_ms_;
}

uint32_t ReportStore::lastNumber() const
{
  return next_number_ - 1U;
}
