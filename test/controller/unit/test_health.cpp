/*
 * test_health.cpp — host unit tests of the HEALTH and EVENT report payloads
 * (src/controller/firmware/health-monitor/health_monitor.cpp) against the report table of
 * Interfaces Sec. 5.4: fields in the table's row order, little-endian (Sec. 4.0). The expected
 * bytes below are written out by hand from that table, not computed by the code under test.
 * From step 2d the mode, trial time left and self-test come in as inputs (from the mode
 * manager); the first three cases keep their expected bytes from step 2c.
 *
 * Run: make -C test/controller unit. Prints PASS or FAIL per test; exit status 1 on a failure.
 */
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>

#include "boot_slot.hpp"
#include "boot_state.h"
#include "health_monitor.hpp"
#include "image_format.h"
#include "layout.h"

namespace {

using Header = std::array<uint8_t, IMAGE_HEADER_BYTES>;
using Payload = std::array<uint8_t, kHealthReportBytes>;

uint32_t failures;
uint32_t tests;

/**
 * @brief Counts one test result and prints a PASS or FAIL line for it.
 *
 * Increments the global test count, and the failure count when the check failed.
 *
 * @param[in] ok    true if the check passed
 * @param[in] name  text printed after PASS or FAIL
 */
void report(bool ok, const char *name)
{
    tests++;
    if (!ok) {
        failures++;
    }
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
}

/**
 * @brief Fills an image header with kind main firmware and a fixed version.
 *
 * A header with kind 0x02 (main firmware) and version 0x01020304 (Interfaces Sec. 5.5). All
 * other bytes are zero; the version is stored little-endian.
 *
 * @param[out] header  header buffer to overwrite
 */
void makeHeader(Header &header)
{
    header.fill(0U);
    header[IMAGE_OFFSET_KIND] = IMAGE_KIND_MAIN;
    header[IMAGE_OFFSET_VERSION] = 0x04U;
    header[IMAGE_OFFSET_VERSION + 1U] = 0x03U;
    header[IMAGE_OFFSET_VERSION + 2U] = 0x02U;
    header[IMAGE_OFFSET_VERSION + 3U] = 0x01U;
}

/**
 * @brief Builds a HEALTH report from the inputs and compares it with the expected bytes.
 *
 * The output buffer is pre-filled with 0xEE so that a byte the report does not write is seen.
 *
 * @param[in] in        inputs handed to HealthMonitor::report
 * @param[in] expected  expected report bytes, written out by hand from the report table
 * @return true if the report equals expected, false otherwise
 */
bool payloadIs(const HealthInputs &in, const Payload &expected)
{
    Payload out{};

    out.fill(0xEEU);
    HealthMonitor::report(in, out);
    return out == expected;
}

}  // namespace

/**
 * @brief Runs the HEALTH and EVENT report tests and prints the totals.
 *
 * Checks four HEALTH cases (trial on B, operate on A, nothing known, trial time left with a
 * failed self-test) and the EVENT report with no detail, a one-byte detail, the largest detail
 * and a detail one byte too long.
 *
 * @return 0 if all tests passed, 1 if any failed
 */
int main()
{
    Header header{};
    HealthInputs in{};

    makeHeader(header);
    in.now_ms = 12345U;
    in.state_valid = true;
    in.state.confirmed = BOOT_SLOT_A;
    in.state.trial = BOOT_SLOT_B;
    in.state.failed_starts = 2U;
    in.running_slot = Slot::kB;
    in.running_header = header.data();
    in.mode = HealthMode::kTrial;
    in.trial_left_s = 0U;
    in.self_test_passed = true;

    /* report no. | onboard time | controller time 12 s | kind | mode | fw kind | version |
     * slot | failed starts | trial time left | self-test */
    const Payload trial_b = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00,
        0x01, 0x02, 0x02, 0x04, 0x03, 0x02, 0x01, 0x02, 0x02, 0x00, 0x00, 0x00};
    report(payloadIs(in, trial_b),
           "HEALTH, running B on trial: 24 bytes in table order, mode TRIAL, slot 0x02, "
           "2 failed starts, 12 s, version 0x01020304");

    in.running_slot = Slot::kA;
    in.mode = HealthMode::kOperate;
    const Payload operate_a = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00,
        0x01, 0x01, 0x02, 0x04, 0x03, 0x02, 0x01, 0x01, 0x02, 0x00, 0x00, 0x00};
    report(payloadIs(in, operate_a),
           "HEALTH, running A while B is the trial: mode OPERATE, slot 0x01");

    in.state_valid = false;
    in.running_header = nullptr;
    in.running_slot = Slot::kNone;
    in.now_ms = 0xFFFFFFFFU;   /* 4 294 967 s = 0x00418937 */
    const Payload unknown = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x37, 0x89, 0x41, 0x00,
        0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    report(payloadIs(in, unknown),
           "HEALTH with no boot state, no header, slot not known: OPERATE, zeros, slot 0x00; "
           "time from the largest millisecond count");

    in.now_ms = 33000U;
    in.state_valid = true;
    in.running_header = header.data();
    in.running_slot = Slot::kB;
    in.mode = HealthMode::kTrial;
    in.trial_left_s = 87U;
    in.self_test_passed = false;
    const Payload trial_left = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00,
        0x01, 0x02, 0x02, 0x04, 0x03, 0x02, 0x01, 0x02, 0x02, 0x57, 0x00, 0x01};
    report(payloadIs(in, trial_left),
           "HEALTH from the mode manager: TRIAL, trial time left 87 s (0x0057), last self-test "
           "0x01 FAILED, 33 s");

    std::array<uint8_t, kEventReportMaxBytes> event{};
    event.fill(0xEEU);
    const uint8_t which = 0x03U;
    uint16_t length = HealthMonitor::eventReport(12999U, EventCode::kTestsFailed, &which, 1U, event);
    /* report no. | onboard time | controller time 12 s | kind EVENT | code 0x000B | detail */
    const std::array<uint8_t, 17> failed = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C,
                                            0x00, 0x00, 0x00, 0x02, 0x0B, 0x00, 0x03, 0xEE};
    report((length == 16U) && std::equal(failed.begin(), failed.end(), event.begin()),
           "EVENT 0x000B with detail 0x03: 16 bytes in table order, kind 0x02, code little-endian, "
           "nothing written past the detail");

    std::array<uint8_t, kEventDetailMax + 1U> detail{};
    detail.fill(0x5AU);
    length = HealthMonitor::eventReport(0U, EventCode::kTrialStarted, nullptr, 0U, event);
    bool none = (length == 15U) && (event[13] == 0x06U) && (event[14] == 0x00U);
    length = HealthMonitor::eventReport(0U, EventCode::kGivenUp, detail.data(), kEventDetailMax,
                                        event);
    bool most = (length == 47U) && (event[13] == 0x08U) && (event[46] == 0x5AU);
    event.fill(0xEEU);
    length = HealthMonitor::eventReport(0U, EventCode::kGivenUp, detail.data(),
                                        kEventDetailMax + 1U, event);
    bool over = (length == 0U) && (event[0] == 0xEEU);
    report(none && most && over,
           "EVENT without detail: 15 bytes; with 32 detail bytes: 47; 33 detail bytes: refused "
           "(length 0, nothing written)");

    if (failures == 0U) {
        std::printf("health report unit tests: all %u passed\n", tests);
        return 0;
    }
    std::printf("health report unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
