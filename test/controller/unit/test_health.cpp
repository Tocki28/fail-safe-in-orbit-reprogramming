/*
 * test_health.cpp — host unit tests of the HEALTH report payload
 * (src/controller/firmware/health-monitor/health_monitor.cpp) against the report table of
 * Interfaces Sec. 5.4: fields in the table's row order, little-endian (Sec. 4.0). The expected
 * bytes below are written out by hand from that table, not computed by the code under test.
 *
 * Run: make -C test/controller unit. Prints PASS or FAIL per test; exit status 1 on a failure.
 */
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

void report(bool ok, const char *name)
{
    tests++;
    if (!ok) {
        failures++;
    }
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
}

/* A header with kind 0x02 (main firmware) and version 0x01020304 (Interfaces Sec. 5.5). */
void makeHeader(Header &header)
{
    header.fill(0U);
    header[IMAGE_OFFSET_KIND] = IMAGE_KIND_MAIN;
    header[IMAGE_OFFSET_VERSION] = 0x04U;
    header[IMAGE_OFFSET_VERSION + 1U] = 0x03U;
    header[IMAGE_OFFSET_VERSION + 2U] = 0x02U;
    header[IMAGE_OFFSET_VERSION + 3U] = 0x01U;
}

bool payloadIs(const HealthInputs &in, const Payload &expected)
{
    Payload out{};

    out.fill(0xEEU);
    HealthMonitor::report(in, out);
    return out == expected;
}

}  // namespace

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

    /* report no. | onboard time | controller time 12 s | kind | mode | fw kind | version |
     * slot | failed starts | trial time left | self-test */
    const Payload trial_b = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00,
        0x01, 0x02, 0x02, 0x04, 0x03, 0x02, 0x01, 0x02, 0x02, 0x00, 0x00, 0x00};
    report(payloadIs(in, trial_b),
           "HEALTH, running B on trial: 24 bytes in table order, mode TRIAL, slot 0x02, "
           "2 failed starts, 12 s, version 0x01020304");

    in.running_slot = Slot::kA;
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

    if (failures == 0U) {
        std::printf("health report unit tests: all %u passed\n", tests);
        return 0;
    }
    std::printf("health report unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
