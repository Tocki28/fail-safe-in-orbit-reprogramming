/*
 * health_monitor.h — the HEALTH report (Interfaces Sec. 5.4, report table), minimal version of
 * build step 2c: built from the boot state and the running image's header. The rest of the
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
 * No hardware here: main.c reads the boot state and the header and passes them in, so the
 * same file compiles for the unit tests (test/controller/unit).
 */
#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "boot_state.h"

#define HEALTH_REPORT_BYTES   24U
/* One HEALTH report per second. Bench value: REQ-037's rate is TBD-rate in the SRS
 * (Interfaces Sec. 5.4, open item "rate"). */
#define HEALTH_PERIOD_MS      1000U

#define REPORT_KIND_HEALTH    0x01U
#define HEALTH_MODE_OPERATE   0x01U
#define HEALTH_MODE_TRIAL     0x02U
#define HEALTH_MODE_FALLBACK  0x03U
#define HEALTH_SLOT_A         0x01U
#define HEALTH_SLOT_B         0x02U
#define HEALTH_SLOT_LAST_RESORT 0x03U
/* Not in the table: this build runs only in slot A or B, so it cannot occur. */
#define HEALTH_SLOT_UNKNOWN   0x00U

typedef struct {
    uint32_t now_ms;                /* milliseconds since start (core_clock.h, tick_ms) */
    bool state_valid;               /* the boot state could be read */
    boot_state_t state;
    uint8_t running_slot;           /* BOOT_SLOT_A or _B (update_slot_of); BOOT_SLOT_NONE if not known */
    const uint8_t *running_header;  /* the running image's 64-byte header; NULL if not known */
} health_inputs_t;

/* The mode for the report: TRIAL if the boot state names the running slot as the trial. */
uint8_t health_mode(const health_inputs_t *in);

/* Write the 24-byte HEALTH payload. */
void health_report(const health_inputs_t *in, uint8_t out[HEALTH_REPORT_BYTES]);

#endif /* HEALTH_MONITOR_H */
