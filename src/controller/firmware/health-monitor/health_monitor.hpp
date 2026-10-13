/*
 * health_monitor.hpp — the HEALTH and EVENT reports (Interfaces Sec. 5.4, report table and
 * event codes). HEALTH, minimal version of build step 2c: built from the boot state, the running
 * image's header and, from step 2d, the mode manager's mode, trial time left and self-test
 * result. EVENT from step 2d: the layout only; the mode manager decides when one is sent. The
 * rest of the health monitor (fault detection, FALLBACK) comes in step 2e.
 *
 * Payload, in the table's row order (multi-byte fields little-endian, Interfaces Sec. 4.0):
 *   offset  bytes  field                                set by
 *        0      4  report number                        onboard computer (sent as 0)
 *        4      4  onboard computer clock time          onboard computer (sent as 0)
 *        8      4  controller clock time                controller
 *       12      1  report kind: 0x01 HEALTH             controller
 *       13      1  mode: 0x01 OPERATE, 0x02 TRIAL, 0x03 FALLBACK
 *       14      1  running firmware kind
 *       15      4  running firmware version
 *       19      1  running slot: 0x01 A, 0x02 B, 0x03 last-resort
 *       20      1  failed starts of the new copy
 *       21      2  trial time left, in seconds
 *       23      1  last self-test: 0x00 PASSED, 0x01 FAILED
 * Sent in a frame of Type 0x03 (report) to Destination 0x01 (onboard computer).
 *
 * EVENT payload: the ALL rows of the same table, then the EVENT rows:
 *   offset  bytes  field
 *        0      4  report number                        onboard computer (sent as 0)
 *        4      4  onboard computer clock time          onboard computer (sent as 0)
 *        8      4  controller clock time                controller
 *       12      1  report kind: 0x02 EVENT              controller
 *       13      2  event code (event code table)
 *       15  0..32  detail
 *
 * No hardware here: main.cpp reads the boot state and the header and passes them in, so the
 * same file compiles for the unit tests (test/controller/unit).
 *
 * C++: class HealthMonitor (C: health_monitor.c, plus the report timing that was in main.c's
 * send_health_when_due, whose last-report time is now a member).
 */
#ifndef HEALTH_MONITOR_HPP
#define HEALTH_MONITOR_HPP

#include <array>
#include <cstdint>

#include "boot_slot.hpp"
#include "boot_state.h"

constexpr uint32_t kHealthReportBytes = 24U;
/* One HEALTH report per second. Bench value: REQ-037's rate is TBD-rate in the SRS
 * (Interfaces Sec. 5.4, open item "rate"). */
constexpr uint32_t kHealthPeriodMs = 1000U;

constexpr uint8_t kReportKindHealth = 0x01U;
constexpr uint8_t kReportKindEvent = 0x02U;

/* EVENT payload sizes (layout above): 15 bytes before the detail, at most 32 detail bytes. */
constexpr uint32_t kEventHeadBytes = 15U;
constexpr uint32_t kEventDetailMax = 32U;
constexpr uint32_t kEventReportMaxBytes = kEventHeadBytes + kEventDetailMax;

/* The event codes the controller sends in this step (Interfaces Sec. 5.4, event code table). */
enum class EventCode : uint16_t {
    kFaultDetected = 0x0001U,      /* no detail: the clock fault of decision C (core_clock.hpp) */
    kFallbackPrevious = 0x0002U,   /* detail: refused copy (1), check result (1) */
    kSelfTestFailed = 0x0004U,     /* no detail: decision L, a failed self-test in OPERATE */
    kMemoryChanged = 0x0005U,      /* no detail: the fault reaction (firmware/fault.hpp) */
    kTrialStarted = 0x0006U,
    kConfirmed = 0x0007U,
    kGivenUp = 0x0008U,
    kTestsPassed = 0x000AU,
    kTestsFailed = 0x000BU,   /* detail: which test (mode-manager/mode_manager.hpp, TrialTest) */
};

/* The mode, coded as in the HEALTH report's mode field. The mode manager owns it
 * (mode-manager/mode_manager.hpp) and reports it in these terms. */
enum class HealthMode : uint8_t {
    kOperate = 0x01U,
    kTrial = 0x02U,
    kFallback = 0x03U,
};

/* The running-slot field's codes (report table). Not the boot state's codes (boot_slot.hpp),
 * even where the numbers agree: each table owns its own. */
enum class ReportSlot : uint8_t {
    /* Not in the table: this build runs only in slot A or B, so it cannot occur. */
    kUnknown = 0x00U,
    kA = 0x01U,
    kB = 0x02U,
    kLastResort = 0x03U,
};

struct HealthInputs {
    uint32_t now_ms;                /* milliseconds since start (core_clock.hpp, tickMs) */
    bool state_valid;               /* the boot state could be read */
    boot_state_t state;
    Slot running_slot;              /* kA or kB (UpdateReceiver::slotOf); kNone if not known */
    const uint8_t *running_header;  /* the running image's 64-byte header; nullptr if not known */
    HealthMode mode;                /* the mode manager's mode (ModeManager::mode) */
    uint16_t trial_left_s;          /* seconds of trial left; 0 outside a trial */
    bool self_test_passed;          /* result of the self-test at start (ModeManager) */
};

class HealthMonitor {
public:
    /* True once every kHealthPeriodMs, and remembers now_ms as the time of this report
     * (C: the first lines of main.c's send_health_when_due). */
    [[nodiscard]] bool reportDue(uint32_t now_ms);

    /** @brief Write the 24-byte HEALTH payload. */
    static void report(const HealthInputs &in, std::array<uint8_t, kHealthReportBytes> &out);

    /** @brief Write an EVENT payload; returns its length, 0 if the detail is too long. */
    [[nodiscard]] static uint16_t eventReport(uint32_t now_ms, EventCode code,
                                              const uint8_t *detail, uint32_t detail_bytes,
                                              std::array<uint8_t, kEventReportMaxBytes> &out);

private:
    uint32_t last_report_ms_ = 0U;
};

#endif /* HEALTH_MONITOR_HPP */
