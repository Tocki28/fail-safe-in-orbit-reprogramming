/*
 * health_monitor.cpp — the HEALTH report payload (see health_monitor.hpp for the layout).
 */
#include "health_monitor.hpp"

#include "image_check.h"
#include "image_format.h"

namespace {

/* Offsets in the payload, one per row of the Interfaces Sec. 5.4 report table. */
constexpr uint32_t kAtReportNumber = 0U;
constexpr uint32_t kAtOnboardTime = 4U;
constexpr uint32_t kAtControllerTime = 8U;
constexpr uint32_t kAtKind = 12U;
constexpr uint32_t kAtMode = 13U;
constexpr uint32_t kAtFirmwareKind = 14U;
constexpr uint32_t kAtFirmwareVersion = 15U;
constexpr uint32_t kAtRunningSlot = 19U;
constexpr uint32_t kAtFailedStarts = 20U;
constexpr uint32_t kAtTrialTimeLeft = 21U;
constexpr uint32_t kAtLastSelfTest = 23U;
constexpr uint32_t kMsPerSecond = 1000U;
constexpr uint8_t kSelfTestPassed = 0x00U;

void writeU16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
}

void writeU32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
    bytes[2] = static_cast<uint8_t>(value >> 16);
    bytes[3] = static_cast<uint8_t>(value >> 24);
}

ReportSlot slotCode(Slot slot)
{
    if (slot == Slot::kA) {
        return ReportSlot::kA;
    }
    return (slot == Slot::kB) ? ReportSlot::kB : ReportSlot::kUnknown;
}

}  // namespace

bool HealthMonitor::reportDue(uint32_t now_ms)
{
    /* Unsigned subtraction stays right when the millisecond count wraps. */
    if ((now_ms - last_report_ms_) < kHealthPeriodMs) {
        return false;
    }
    last_report_ms_ = now_ms;
    return true;
}

HealthMode HealthMonitor::mode(const HealthInputs &in)
{
    /* FALLBACK needs the mode manager (step 2d) to know a trial was given up. */
    bool trial = in.state_valid && (toSlot(in.state.trial) != Slot::kNone) &&
                 (toSlot(in.state.trial) == in.running_slot);
    return trial ? HealthMode::kTrial : HealthMode::kOperate;
}

void HealthMonitor::report(const HealthInputs &in, std::array<uint8_t, kHealthReportBytes> &out)
{
    const uint8_t *header = in.running_header;

    /* The onboard computer fills these two before it stores the report (REQ-070, REQ-071,
     * Interfaces Sec. 5.4). */
    writeU32(&out[kAtReportNumber], 0U);
    writeU32(&out[kAtOnboardTime], 0U);
    /* Seconds since start: a placeholder until the controller clock component (after step 2e,
     * REQ-131). Erases stall the millisecond tick (update_receiver.hpp), so it runs slow by up
     * to 0.49 s per UPLOAD BEGIN. */
    writeU32(&out[kAtControllerTime], in.now_ms / kMsPerSecond);
    out[kAtKind] = kReportKindHealth;
    out[kAtMode] = static_cast<uint8_t>(mode(in));
    /* From the header the bootloader checked before it started this firmware. */
    out[kAtFirmwareKind] = (header != nullptr) ? header[IMAGE_OFFSET_KIND] : uint8_t{0U};
    writeU32(&out[kAtFirmwareVersion],
             (header != nullptr) ? image_read_u32(&header[IMAGE_OFFSET_VERSION]) : 0U);
    out[kAtRunningSlot] = static_cast<uint8_t>(slotCode(in.running_slot));
    out[kAtFailedStarts] = in.state_valid ? in.state.failed_starts : uint8_t{0U};
    /* TODO step 2d (mode manager): the trial's time left. 0 until then. */
    writeU16(&out[kAtTrialTimeLeft], 0U);
    /* TODO step 2e (health monitor): the result of the last self-test. No self-test runs yet,
     * so 0x00 ("PASSED" in the table) is a placeholder, not a result. */
    out[kAtLastSelfTest] = kSelfTestPassed;
}
