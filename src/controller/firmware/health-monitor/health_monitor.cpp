/*
 * health_monitor.cpp — the HEALTH and EVENT report payloads (see health_monitor.hpp for the
 * layouts).
 *
 * The switch over Slot has no default label. No default: the compiler checks every case
 * (-Wswitch, -Werror); only a corrupted value reaches the line after the switch, which calls the
 * fault reaction. A deliberate deviation from MISRA C:2012 Rule 16.4, whose purpose, handling a
 * value outside the cases, the fault reaction meets (firmware/fault.hpp).
 */
#include "health_monitor.hpp"

#include <cstring>

#include "fault.hpp"
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
/* EVENT offsets: the ALL rows as above, then the event code and the detail. */
constexpr uint32_t kAtEventCode = 13U;
constexpr uint32_t kAtEventDetail = 15U;
constexpr uint32_t kMsPerSecond = 1000U;
/* Last self-test field (report table): 0x00 PASSED, 0x01 FAILED. */
constexpr uint8_t kSelfTestPassed = 0x00U;
constexpr uint8_t kSelfTestFailed = 0x01U;

static_assert(kAtEventDetail == kEventHeadBytes, "EVENT detail follows the 15-byte head");

/**
 * @brief Write a 16-bit value little-endian.
 *
 * @param[out] bytes  two bytes, least significant first
 * @param[in]  value  the value to write
 */
void writeU16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
}

/**
 * @brief Write a 32-bit value little-endian.
 *
 * @param[out] bytes  four bytes, least significant first
 * @param[in]  value  the value to write
 */
void writeU32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
    bytes[2] = static_cast<uint8_t>(value >> 16);
    bytes[3] = static_cast<uint8_t>(value >> 24);
}

/**
 * @brief Convert a boot slot to its report code.
 *
 * Slot::kNone is the running slot when the vector table lies in neither slot
 * (UpdateReceiver::slotOf); this build always runs from A or B, and the report then carries the
 * code 0x00, which the report table does not list (health_monitor.hpp, ReportSlot::kUnknown). A
 * value outside the Slot enum is a corrupted variable: the fault reaction (firmware/fault.hpp).
 * Before, an if-chain gave kUnknown for it too, a normal-looking report field.
 *
 * @param[in] slot  the slot
 * @return the report code for slot A or B; ReportSlot::kUnknown for Slot::kNone
 * @note Does not return for a value outside Slot.
 */
ReportSlot slotCode(Slot slot)
{
    switch (slot) {
    case Slot::kA:
        return ReportSlot::kA;
    case Slot::kB:
        return ReportSlot::kB;
    case Slot::kNone:
        return ReportSlot::kUnknown;
    }
    /* only a corrupted value reaches here */
    faultImpossibleValue(FAULT_AT_SLOT_CODE, "slotCode");
}

}  // namespace

/**
 * @brief Tell whether a HEALTH report is due; if so, restart the period.
 *
 * Due when kHealthPeriodMs or more have passed since the last report that was due.
 *
 * @param[in] now_ms  milliseconds since start
 * @return true if a report is due now (the period restarts); false otherwise
 */
bool HealthMonitor::reportDue(uint32_t now_ms)
{
    /* Unsigned subtraction stays right when the millisecond count wraps. */
    if ((now_ms - last_report_ms_) < kHealthPeriodMs) {
        return false;
    }
    last_report_ms_ = now_ms;
    return true;
}

/**
 * @brief Writes the 24-byte HEALTH payload, fields in the order of the Interfaces Sec. 5.4
 * report table, multi-byte fields little-endian (Sec. 4.0).
 *
 * Step 2d: mode, trial time left and last self-test come from the mode manager, the one owner
 * of the mode. Rejected: deriving the mode here from the boot state, as step 2c did; it would
 * disagree with the mode manager after a CONFIRM whose boot-state write failed, or when the
 * boot state cannot be read.
 *
 * @param[in]     in   the values to report (see HealthInputs)
 * @param[out]    out  the payload
 */
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
    out[kAtMode] = static_cast<uint8_t>(in.mode);
    /* From the header the bootloader checked before it started this firmware. */
    out[kAtFirmwareKind] = (header != nullptr) ? header[IMAGE_OFFSET_KIND] : uint8_t{0U};
    writeU32(&out[kAtFirmwareVersion],
             (header != nullptr) ? image_read_u32(&header[IMAGE_OFFSET_VERSION]) : 0U);
    out[kAtRunningSlot] = static_cast<uint8_t>(slotCode(in.running_slot));
    out[kAtFailedStarts] = in.state_valid ? in.state.failed_starts : uint8_t{0U};
    writeU16(&out[kAtTrialTimeLeft], in.trial_left_s);
    /* The self-test at start (REQ-039, ModeManager::selfTest). Its repetition at TBD-selftest
     * comes with the health monitor (step 2e). */
    out[kAtLastSelfTest] = in.self_test_passed ? kSelfTestPassed : kSelfTestFailed;
}

/**
 * @brief Writes an EVENT payload: the ALL rows of the Interfaces Sec. 5.4 report table, then the
 * event code (2 bytes, little-endian, Sec. 4.0) and up to 32 detail bytes.
 *
 * Report number and onboard computer time are sent as 0, as in HEALTH: the onboard computer
 * fills them before it stores the report (REQ-070, REQ-071). Controller clock time as in
 * HEALTH: seconds since start, a placeholder until the controller clock (REQ-131).
 *
 * @param[in]     now_ms        milliseconds since start
 * @param[in]     code          the event code (Interfaces Sec. 5.4, event code table)
 * @param[in]     detail        detail bytes; may be nullptr when detail_bytes is 0
 * @param[in]     detail_bytes  number of detail bytes, 0 to 32
 * @param[out]    out           the payload; bytes past the returned length are left as they were
 * @return the payload length, 15 + detail_bytes; 0 if detail_bytes is above 32 (nothing written)
 */
uint16_t HealthMonitor::eventReport(uint32_t now_ms, EventCode code, const uint8_t *detail,
                                    uint32_t detail_bytes,
                                    std::array<uint8_t, kEventReportMaxBytes> &out)
{
    if (detail_bytes > kEventDetailMax) {
        return 0U;
    }
    writeU32(&out[kAtReportNumber], 0U);
    writeU32(&out[kAtOnboardTime], 0U);
    writeU32(&out[kAtControllerTime], now_ms / kMsPerSecond);
    out[kAtKind] = kReportKindEvent;
    writeU16(&out[kAtEventCode], static_cast<uint16_t>(code));
    if (detail_bytes > 0U) {
        std::memcpy(&out[kAtEventDetail], detail, detail_bytes);
    }
    return static_cast<uint16_t>(kEventHeadBytes + detail_bytes);
}
