/*
 * test_health.c — host unit tests of the HEALTH report payload
 * (src/controller/firmware/health-monitor/health_monitor.c) against the report table of
 * Interfaces Sec. 5.4: fields in the table's row order, little-endian (Sec. 4.0). The expected
 * bytes below are written out by hand from that table, not computed by the code under test.
 *
 * Run: make -C test/controller unit. Prints PASS or FAIL per test; exit status 1 on a failure.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "boot_state.h"
#include "health_monitor.h"
#include "image_format.h"
#include "layout.h"

static uint32_t failures;
static uint32_t tests;

static void report(bool ok, const char *name)
{
    tests++;
    if (!ok) {
        failures++;
    }
    printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
}

/* A header with kind 0x02 (main firmware) and version 0x01020304 (Interfaces Sec. 5.5). */
static void make_header(uint8_t header[IMAGE_HEADER_BYTES])
{
    memset(header, 0, IMAGE_HEADER_BYTES);
    header[IMAGE_OFFSET_KIND] = IMAGE_KIND_MAIN;
    header[IMAGE_OFFSET_VERSION] = 0x04U;
    header[IMAGE_OFFSET_VERSION + 1U] = 0x03U;
    header[IMAGE_OFFSET_VERSION + 2U] = 0x02U;
    header[IMAGE_OFFSET_VERSION + 3U] = 0x01U;
}

static bool payload_is(const health_inputs_t *in, const uint8_t expected[HEALTH_REPORT_BYTES])
{
    uint8_t out[HEALTH_REPORT_BYTES];

    memset(out, 0xEE, sizeof out);
    health_report(in, out);
    return memcmp(out, expected, sizeof out) == 0;
}

int main(void)
{
    uint8_t header[IMAGE_HEADER_BYTES];
    health_inputs_t in = {0};

    make_header(header);
    in.now_ms = 12345U;
    in.state_valid = true;
    in.state.confirmed = BOOT_SLOT_A;
    in.state.trial = BOOT_SLOT_B;
    in.state.failed_starts = 2U;
    in.running_slot = BOOT_SLOT_B;
    in.running_header = header;

    /* report no. | onboard time | controller time 12 s | kind | mode | fw kind | version |
     * slot | failed starts | trial time left | self-test */
    static const uint8_t trial_b[HEALTH_REPORT_BYTES] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00,
        0x01, 0x02, 0x02, 0x04, 0x03, 0x02, 0x01, 0x02, 0x02, 0x00, 0x00, 0x00};
    report(payload_is(&in, trial_b),
           "HEALTH, running B on trial: 24 bytes in table order, mode TRIAL, slot 0x02, "
           "2 failed starts, 12 s, version 0x01020304");

    in.running_slot = BOOT_SLOT_A;
    static const uint8_t operate_a[HEALTH_REPORT_BYTES] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00,
        0x01, 0x01, 0x02, 0x04, 0x03, 0x02, 0x01, 0x01, 0x02, 0x00, 0x00, 0x00};
    report(payload_is(&in, operate_a),
           "HEALTH, running A while B is the trial: mode OPERATE, slot 0x01");

    in.state_valid = false;
    in.running_header = NULL;
    in.running_slot = BOOT_SLOT_NONE;
    in.now_ms = 0xFFFFFFFFU;   /* 4 294 967 s = 0x00418937 */
    static const uint8_t unknown[HEALTH_REPORT_BYTES] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x37, 0x89, 0x41, 0x00,
        0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    report(payload_is(&in, unknown),
           "HEALTH with no boot state, no header, slot not known: OPERATE, zeros, slot 0x00; "
           "time from the largest millisecond count");

    if (failures == 0U) {
        printf("health report unit tests: all %u passed\n", tests);
        return 0;
    }
    printf("health report unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
