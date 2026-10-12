/*
 * main.c — the controller's main firmware, step 2c: it starts, keeps the watchdog reloaded,
 * runs data link A, answers GET STATUS, receives firmware updates into the slot that is not
 * confirmed, and sends a HEALTH report every second.
 *
 * Commands handled (Interfaces Sec. 5.1):
 *   GET STATUS (0x25, no fields): RECEIVED, then DONE 0x00.
 *   UPLOAD BEGIN, CHUNK, END, CANCEL (0x01-0x04): update receiver (update-receiver/). A
 *     refusal on arrival is answered REFUSED only; otherwise RECEIVED, the work, then DONE 0x00
 *     or REFUSED with the reason (Interfaces Sec. 4.2, REQ-033, REQ-034).
 *   Anything else: REFUSED 0x06 only (Interfaces Sec. 4.2).
 * A frame not addressed to the controller, or not a command, is ignored.
 *
 * Not yet: it does not confirm itself in the boot state (step 2d). Linked for slot A or B
 * (Makefile: SLOT=A or B).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "boot_state.h"
#include "core_clock.h"
#include "frame.h"
#include "health_monitor.h"
#include "layout.h"
#include "stm32g4xx.h"
#include "uart_link.h"
#include "update_receiver.h"
#include "watchdog.h"

/* GET STATUS: identifier 0x25, no fields (Interfaces Sec. 5.1). */
#define CMD_GET_STATUS        0x25U
#define GET_STATUS_BYTES      1U
/* Code byte of a RECEIVED answer. Interfaces Sec. 4.2 gives codes only for DONE and REFUSED;
 * 0x00 here is a placeholder awaiting the owner's decision. */
#define RECEIVED_CODE         0x00U
/* The bootloader starts the watchdog with 1.88 s at the shortest (bootloader/handover.c).
 * Reloaded every 100 ms from the main loop. The longest pass without a reload is UPLOAD END:
 * the CRC-32 and two Ed25519 checks of the slot (about 40 ms at -O2 in the bootloader,
 * test/controller/results/2026-10-12; slower at -Os here; upload_probe.py prints END's time).
 * UPLOAD BEGIN's erase (up to 489 ms) reloads after every page itself. A loop stuck for
 * 1.88 s is restarted (REQ-022). The reload follows the millisecond tick, so a stopped tick
 * also ends in a restart. */
#define RELOAD_PERIOD_MS      100U
/* The largest frame the controller sends: a HEALTH report (24-byte payload). */
#define OUT_FRAME_BYTES       (FRAME_HEADER_BYTES + HEALTH_REPORT_BYTES + FRAME_CHECK_BYTES)

_Static_assert(HEALTH_REPORT_BYTES >= ANSWER_PAYLOAD_BYTES, "OUT_FRAME_BYTES must hold an answer");

/* Sequence number of the controller's next frame: one higher than its last (Interfaces
 * Sec. 4.0), from 1 at every start, answers and reports alike. Open for step 2b (REQ-065):
 * keeping it over restarts, and what follows 2^32 - 1. */
static uint32_t next_sequence = 1U;
static uint32_t last_reload_ms;
static uint32_t last_health_ms;
static uint8_t running_slot;
static update_receiver_t updater;

static void reload_watchdog_when_due(void)
{
    uint32_t now = tick_ms();

    /* Unsigned subtraction stays right when the millisecond count wraps. */
    if ((now - last_reload_ms) >= RELOAD_PERIOD_MS) {
        watchdog_reload();
        last_reload_ms = now;
    }
}

/* One frame to the onboard computer, which passes the ground's answers on (Interfaces
 * Sec. 4.2, Figure 4). Open: the Interfaces document does not say whether an answer to a
 * ground command carries Destination 0x01 or 0x03; 0x01 here awaits the owner's decision. */
static void send_frame(uint8_t type, const uint8_t *payload, uint16_t length)
{
    uint8_t bytes[OUT_FRAME_BYTES];
    frame_t frame = {
        .type = type,
        .destination = FRAME_DEST_ONBOARD,
        .flags = 0U,
        .length = length,
        .sequence = next_sequence,
        .expiry = 0U,   /* 0 in answers and reports (Interfaces Sec. 4.0) */
        .payload = payload,
    };
    uint32_t size = frame_build(&frame, bytes, sizeof bytes);

    next_sequence++;
    /* A stalled transmitter cuts the frame short; the onboard computer drops it and, missing
     * answers, reports the link lost (REQ-081). Nothing more to do here in this step. */
    (void)uart_link_send(bytes, size);
}

static void send_answer(uint32_t answered, uint8_t kind, uint8_t code)
{
    uint8_t payload[ANSWER_PAYLOAD_BYTES];

    answer_payload(answered, kind, code, payload);
    send_frame(FRAME_TYPE_ANSWER, payload, ANSWER_PAYLOAD_BYTES);
}

/* RECEIVED goes out before the work starts: the onboard computer learns the command arrived
 * even when the work (an erase) takes half a second. */
static void handle_upload(const frame_t *frame)
{
    update_command_t command;
    uint8_t result = update_accept(&updater, frame->payload, frame->length, &command);

    if (result != UPDATE_OK) {
        send_answer(frame->sequence, ANSWER_REFUSED, result);
        return;
    }
    send_answer(frame->sequence, ANSWER_RECEIVED, RECEIVED_CODE);
    result = update_run(&updater, &command);
    send_answer(frame->sequence, (result == UPDATE_OK) ? ANSWER_DONE : ANSWER_REFUSED, result);
}

static bool is_get_status(const frame_t *command)
{
    return (command->length == GET_STATUS_BYTES) && (command->payload[0] == CMD_GET_STATUS);
}

/* Step 2b puts the command checker (firmware/command-checker) in front of this, with the
 * checks on arrival that come before RECEIVED: sequence numbers (REQ-065; ALREADY DONE,
 * REQ-031), expiry (REQ-066, REQ-133), modes (REQ-027), arming (REQ-029, REQ-030), Flags and
 * scheduled commands (REQ-134). Here only the command itself is checked (REQ-028). */
static void handle(const frame_t *frame)
{
    if ((frame->destination != FRAME_DEST_CONTROLLER) || (frame->type != FRAME_TYPE_COMMAND)) {
        return;   /* Destination names who acts on a frame (Interfaces Sec. 4.0) */
    }
    if (update_is_upload(frame->payload, frame->length)) {
        handle_upload(frame);
        return;
    }
    if (!is_get_status(frame)) {
        send_answer(frame->sequence, ANSWER_REFUSED, REFUSED_IMPOSSIBLE);
        return;
    }
    send_answer(frame->sequence, ANSWER_RECEIVED, RECEIVED_CODE);
    send_answer(frame->sequence, ANSWER_DONE, ANSWER_CODE_SUCCESS);
}

/* One HEALTH report every HEALTH_PERIOD_MS (health-monitor/health_monitor.h). */
static void send_health_when_due(void)
{
    uint32_t now = tick_ms();
    health_inputs_t in = {0};
    uint8_t payload[HEALTH_REPORT_BYTES];

    if ((now - last_health_ms) < HEALTH_PERIOD_MS) {
        return;
    }
    last_health_ms = now;
    in.now_ms = now;
    in.state_valid = boot_state_read(&in.state);
    in.running_slot = running_slot;
    /* The image header sits at the slot's first byte (Interfaces Sec. 5.5, layout.h). */
    in.running_header = (running_slot == BOOT_SLOT_NONE)
                            ? NULL
                            : (const uint8_t *)(uintptr_t)update_slot_start(running_slot);
    health_report(&in, payload);
    send_frame(FRAME_TYPE_REPORT, payload, HEALTH_REPORT_BYTES);
}

int main(void)
{
    /* Own vector table, also when started by a debugger instead of the bootloader. This build
     * runs only at the address it is linked for, so VTOR also names the slot it runs from. */
    SCB->VTOR = (uint32_t)CODE_START;
    running_slot = update_slot_of(SCB->VTOR);
    update_init(&updater, running_slot);
    if (!core_clock_start()) {
        /* No usable clock, so no usable link. Stop reloading the watchdog: it restarts the
         * chip within ~2 s (REQ-022). */
        for (;;) {
        }
    }
    tick_start();
    uart_link_init();

    /* The one endless loop; the watchdog bounds every pass. */
    for (;;) {
        frame_t frame;

        reload_watchdog_when_due();
        if (uart_link_receive(&frame)) {
            handle(&frame);
        }
        send_health_when_due();
    }
}
