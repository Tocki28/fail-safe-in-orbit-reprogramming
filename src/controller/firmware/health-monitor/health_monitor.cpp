/*
 * health_monitor.c — the HEALTH report payload (see health_monitor.h for the layout).
 */
#include "health_monitor.h"

#include <stddef.h>

#include "image_check.h"
#include "image_format.h"

/* Offsets in the payload, one per row of the Interfaces Sec. 5.4 report table. */
#define AT_REPORT_NUMBER     0U
#define AT_ONBOARD_TIME      4U
#define AT_CONTROLLER_TIME   8U
#define AT_KIND              12U
#define AT_MODE              13U
#define AT_FIRMWARE_KIND     14U
#define AT_FIRMWARE_VERSION  15U
#define AT_RUNNING_SLOT      19U
#define AT_FAILED_STARTS     20U
#define AT_TRIAL_TIME_LEFT   21U
#define AT_LAST_SELF_TEST    23U
#define MS_PER_SECOND        1000U
#define SELF_TEST_PASSED     0x00U

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

uint8_t health_mode(const health_inputs_t *in)
{
    /* FALLBACK needs the mode manager (step 2d) to know a trial was given up. */
    bool trial = in->state_valid && (in->state.trial != BOOT_SLOT_NONE) &&
                 (in->state.trial == in->running_slot);
    return trial ? HEALTH_MODE_TRIAL : HEALTH_MODE_OPERATE;
}

static uint8_t slot_code(uint8_t slot)
{
    if (slot == BOOT_SLOT_A) {
        return HEALTH_SLOT_A;
    }
    return (slot == BOOT_SLOT_B) ? HEALTH_SLOT_B : HEALTH_SLOT_UNKNOWN;
}

void health_report(const health_inputs_t *in, uint8_t out[HEALTH_REPORT_BYTES])
{
    const uint8_t *header = in->running_header;

    /* The onboard computer fills these two before it stores the report (REQ-070, REQ-071,
     * Interfaces Sec. 5.4). */
    write_u32(&out[AT_REPORT_NUMBER], 0U);
    write_u32(&out[AT_ONBOARD_TIME], 0U);
    /* Seconds since start: a placeholder until the controller clock component (after step 2e,
     * REQ-131). Erases stall the millisecond tick (update_receiver.h), so it runs slow by up
     * to 0.49 s per UPLOAD BEGIN. */
    write_u32(&out[AT_CONTROLLER_TIME], in->now_ms / MS_PER_SECOND);
    out[AT_KIND] = REPORT_KIND_HEALTH;
    out[AT_MODE] = health_mode(in);
    /* From the header the bootloader checked before it started this firmware. */
    out[AT_FIRMWARE_KIND] = (header != NULL) ? header[IMAGE_OFFSET_KIND] : 0U;
    write_u32(&out[AT_FIRMWARE_VERSION],
              (header != NULL) ? image_read_u32(&header[IMAGE_OFFSET_VERSION]) : 0U);
    out[AT_RUNNING_SLOT] = slot_code(in->running_slot);
    out[AT_FAILED_STARTS] = in->state_valid ? in->state.failed_starts : 0U;
    /* TODO step 2d (mode manager): the trial's time left. 0 until then. */
    write_u16(&out[AT_TRIAL_TIME_LEFT], 0U);
    /* TODO step 2e (health monitor): the result of the last self-test. No self-test runs yet,
     * so 0x00 ("PASSED" in the table) is a placeholder, not a result. */
    out[AT_LAST_SELF_TEST] = SELF_TEST_PASSED;
}
