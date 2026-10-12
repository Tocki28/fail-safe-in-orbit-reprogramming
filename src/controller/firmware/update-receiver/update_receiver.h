/*
 * update_receiver.h — the update receiver (Architecture Sec. 6.0): takes UPLOAD BEGIN, CHUNK,
 * END and CANCEL from the onboard computer and writes the image into the controller's slot
 * that is NOT confirmed, never the one it runs from (REQ-007, Interfaces Sec. 5.1).
 *
 * Commands (Interfaces Sec. 5.1, command table; multi-byte fields little-endian, Sec. 4.0):
 *   0x01 UPLOAD BEGIN   image number (2), image length (4)
 *   0x02 UPLOAD CHUNK   image number (2), offset (4), data (1 to 256)
 *   0x03 UPLOAD END     image number (2)
 *   0x04 UPLOAD CANCEL  image number (2)
 *
 * STOP-AND-WAIT: the sender sends the next frame only after the DONE (or REFUSED) of the
 * previous one. While the flash is erased or written, every read of the flash stalls
 * [RM0440 Rev 9, Sec. 5.3.5, p. 195]; the interrupt vectors and handlers are in flash, so the
 * receive interrupt cannot run and bytes that arrive meanwhile are lost (USART overrun). A
 * page erase stalls up to 24.47 ms [DS12589 Rev 6, Table 46, p. 107]; UPLOAD BEGIN erases 20.
 *
 * Each command is handled in two calls, matching its two answers (REQ-033, REQ-034):
 *   update_accept()  the checks on arrival; a refusal here is answered REFUSED only;
 *   update_run()     the work, after RECEIVED; its result is answered DONE 0x00 or REFUSED.
 */
#ifndef UPDATE_RECEIVER_H
#define UPDATE_RECEIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "boot_state.h"
#include "image_check.h"

#define CMD_UPLOAD_BEGIN   0x01U
#define CMD_UPLOAD_CHUNK   0x02U
#define CMD_UPLOAD_END     0x03U
#define CMD_UPLOAD_CANCEL  0x04U
/* Most data bytes in one UPLOAD CHUNK (Interfaces Sec. 5.1). */
#define UPLOAD_CHUNK_MAX   256U
/* Accepted, or done with success: the DONE code 0x00 (Interfaces Sec. 4.2). Any other result
 * is a refusal code of the Interfaces Sec. 4.2 table (frame.h, REFUSED_...). */
#define UPDATE_OK          0x00U

typedef enum {
    UPDATE_IDLE = 0,    /* no transfer since start, or the last one was cancelled */
    UPDATE_RECEIVING,   /* BEGIN done; chunks expected */
    UPDATE_CLOSED       /* END has run (image_passed says how), or a flash operation failed */
} update_phase_t;

typedef struct {
    uint8_t running;        /* BOOT_SLOT_A or _B: the slot this firmware runs from */
    update_phase_t phase;
    uint16_t image_number;  /* from the last UPLOAD BEGIN */
    uint8_t target;         /* the slot that transfer writes */
    uint32_t length;        /* image length from that UPLOAD BEGIN */
    uint32_t written;       /* bytes written so far, counted from the slot's first byte */
    bool image_passed;      /* UPDATE_CLOSED only: END's checks passed (for SWITCH, step 2d) */
} update_receiver_t;

/* One upload command, read from the payload by update_accept(). */
typedef struct {
    uint8_t id;
    uint16_t image_number;
    uint32_t value;           /* BEGIN: image length; CHUNK: offset; END, CANCEL: 0 */
    const uint8_t *data;      /* CHUNK: its data, inside the frame payload */
    uint32_t data_bytes;      /* CHUNK: 1 to 256 */
    uint8_t target;           /* the slot the command acts on */
} update_command_t;

/* running_slot: update_slot_of(the firmware's vector table address). */
void update_init(update_receiver_t *rx, uint8_t running_slot);

/* True if the payload's identifier is one of the four upload commands. */
bool update_is_upload(const uint8_t *payload, uint16_t length);

/* Checks on arrival. UPDATE_OK, with *cmd filled in, or the refusal code. */
uint8_t update_accept(const update_receiver_t *rx, const uint8_t *payload, uint16_t length,
                      update_command_t *cmd);

/* Run a command that update_accept() accepted. UPDATE_OK or the refusal code. */
uint8_t update_run(update_receiver_t *rx, const update_command_t *cmd);

/* ---- The rules, as plain functions (unit-tested on the host) ------------------------ */

/* BOOT_SLOT_A or _B if the address lies in that slot, otherwise BOOT_SLOT_NONE. */
uint8_t update_slot_of(uint32_t address);

/* First byte of slot A or B; 0 for any other value. */
uint32_t update_slot_start(uint8_t slot);

/* The slot an upload may write: not confirmed, not running. UPDATE_OK and *target set, or the
 * refusal code with *target = BOOT_SLOT_NONE. */
uint8_t update_choose_target(bool state_valid, const boot_state_t *state, uint8_t running,
                             uint8_t *target);

/* The second barrier, asked before every erase and write: true only if [address,
 * address + bytes) lies inside slot `target`, and `target` is slot A or B and not `running`. */
bool update_write_allowed(uint8_t target, uint8_t running, uint32_t address, uint32_t bytes);

/* The answer code for a result of the image checks (common/image_check.h). */
uint8_t update_refusal_for(image_result_t result);

#endif /* UPDATE_RECEIVER_H */
