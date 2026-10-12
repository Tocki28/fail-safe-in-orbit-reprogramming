/*
 * main.c — the controller's main firmware, step 2a: it starts, keeps the watchdog reloaded,
 * runs data link A, and answers GET STATUS.
 *
 * Commands handled (Interfaces Sec. 5.1): GET STATUS (0x25, no fields) is answered RECEIVED,
 * then DONE with code 0x00. Any other command fails the checks on arrival and is answered
 * REFUSED 0x06 only (Interfaces Sec. 4.2). A frame not addressed to the controller, or not a
 * command, is ignored.
 *
 * Not yet: it does not confirm itself in the boot state (step 2d), so on the bench it must
 * run as the confirmed slot. Linked for slot A or B (Makefile: SLOT=A or B).
 */
#include <stdbool.h>
#include <stdint.h>

#include "core_clock.h"
#include "frame.h"
#include "layout.h"
#include "stm32g4xx.h"
#include "uart_link.h"

/* GET STATUS: identifier 0x25, no fields (Interfaces Sec. 5.1). */
#define CMD_GET_STATUS        0x25U
#define GET_STATUS_BYTES      1U
/* Code byte of a RECEIVED answer. Interfaces Sec. 4.2 gives codes only for DONE and REFUSED;
 * 0x00 here is a placeholder awaiting the owner's decision. */
#define RECEIVED_CODE         0x00U
/* Reload key [RM0440 Rev 9, Sec. 35.3.2, p. 1557]. */
#define IWDG_KEY_RELOAD       0xAAAAU
/* The bootloader starts the watchdog with 1.88 s at the shortest (bootloader/handover.c).
 * Reloaded every 100 ms: with this step's longest loop pass, ~5 ms (two answers sent), the
 * reloads are never more than ~105 ms apart, well inside 500 ms. A loop stuck for 1.88 s is
 * restarted (REQ-022). The reload follows the millisecond tick, so a stopped tick also ends
 * in a restart. */
#define RELOAD_PERIOD_MS      100U
#define ANSWER_FRAME_BYTES    (FRAME_HEADER_BYTES + ANSWER_PAYLOAD_BYTES + FRAME_CHECK_BYTES)

/* Sequence number of the controller's next frame: one higher than its last (Interfaces
 * Sec. 4.0), from 1 at every start. Open for step 2b (REQ-065): keeping it over restarts, and
 * what follows 2^32 - 1. */
static uint32_t next_sequence = 1U;
static uint32_t last_reload_ms;

static void reload_watchdog_when_due(void)
{
    uint32_t now = tick_ms();

    /* Unsigned subtraction stays right when the millisecond count wraps. */
    if ((now - last_reload_ms) >= RELOAD_PERIOD_MS) {
        IWDG->KR = IWDG_KEY_RELOAD;
        last_reload_ms = now;
    }
}

/* One answer frame to the onboard computer, which passes the ground's answers on (Interfaces
 * Sec. 4.2, Figure 4). Open: the Interfaces document does not say whether an answer to a
 * ground command carries Destination 0x01 or 0x03; 0x01 here awaits the owner's decision. */
static void send_answer(uint32_t answered, uint8_t kind, uint8_t code)
{
    uint8_t payload[ANSWER_PAYLOAD_BYTES];
    uint8_t bytes[ANSWER_FRAME_BYTES];

    answer_payload(answered, kind, code, payload);
    frame_t answer = {
        .type = FRAME_TYPE_ANSWER,
        .destination = FRAME_DEST_ONBOARD,
        .flags = 0U,
        .length = ANSWER_PAYLOAD_BYTES,
        .sequence = next_sequence,
        .expiry = 0U,   /* 0 in answers and reports (Interfaces Sec. 4.0) */
        .payload = payload,
    };
    uint32_t size = frame_build(&answer, bytes, sizeof bytes);
    next_sequence++;
    /* A stalled transmitter cuts the frame short; the onboard computer drops it and, missing
     * answers, reports the link lost (REQ-081). Nothing more to do here in this step. */
    (void)uart_link_send(bytes, size);
}

static bool is_get_status(const frame_t *command)
{
    return (command->length == GET_STATUS_BYTES) && (command->payload[0] == CMD_GET_STATUS);
}

/* Step 2b replaces this with the command checker (firmware/command-checker), which adds the
 * checks on arrival that come before RECEIVED: sequence numbers (REQ-065; ALREADY DONE,
 * REQ-031), expiry (REQ-066, REQ-133), modes (REQ-027), arming (REQ-029, REQ-030), Flags and
 * scheduled commands (REQ-134). Here only the command itself is checked (REQ-028). */
static void handle(const frame_t *frame)
{
    if ((frame->destination != FRAME_DEST_CONTROLLER) || (frame->type != FRAME_TYPE_COMMAND)) {
        return;   /* Destination names who acts on a frame (Interfaces Sec. 4.0) */
    }
    if (!is_get_status(frame)) {
        send_answer(frame->sequence, ANSWER_REFUSED, REFUSED_IMPOSSIBLE);
        return;
    }
    send_answer(frame->sequence, ANSWER_RECEIVED, RECEIVED_CODE);
    send_answer(frame->sequence, ANSWER_DONE, ANSWER_CODE_SUCCESS);
}

int main(void)
{
    /* Own vector table, also when started by a debugger instead of the bootloader. */
    SCB->VTOR = (uint32_t)CODE_START;
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
    }
}
