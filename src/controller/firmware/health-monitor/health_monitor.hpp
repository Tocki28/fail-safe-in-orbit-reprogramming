/*
 * health_monitor.hpp — the HEALTH report (Interfaces Sec. 5.4, report table), minimal version
 * of build step 2c: built from the boot state and the running image's header. The rest of the
 * health monitor (events, self-test, FALLBACK) comes in step 2e.
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
};

class HealthMonitor {
public:
    /* True once every kHealthPeriodMs, and remembers now_ms as the time of this report
     * (C: the first lines of main.c's send_health_when_due). */
    [[nodiscard]] bool reportDue(uint32_t now_ms);

    /* The mode for the report: TRIAL if the boot state names the running slot as the trial. */
    static HealthMode mode(const HealthInputs &in);

    /* Write the 24-byte HEALTH payload. */
    static void report(const HealthInputs &in, std::array<uint8_t, kHealthReportBytes> &out);

private:
    uint32_t last_report_ms_ = 0U;
};

#endif /* HEALTH_MONITOR_HPP */
