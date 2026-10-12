/*
 * update_receiver.c — see update_receiver.h. No register access: the chip is reached through
 * update_port.h, so the same file runs in the host unit tests (test/controller/unit).
 *
 * The checks, in order:
 *   1. the payload's size fits the command (0x06);
 *   2. the slot: neither confirmed nor running (update_choose_target); none during a trial
 *      (0x07). Chosen again for every command from the boot state as it is now, so a trial set
 *      after BEGIN stops the next CHUNK instead of writing into the trial slot;
 *   3. the command's values (0x06): BEGIN's length fits a slot; a CHUNK continues the transfer
 *      at exactly the next byte; END comes after the last byte;
 *   4. before every erase and write, the address range again (update_write_allowed).
 *
 * Flash faults: Interfaces Sec. 4.2 has no code for "the flash did not take the write"; 0x08
 * "image damaged" is the nearest, since the copy in the slot can no longer be trusted. The
 * transfer is then closed; CANCEL or a new BEGIN starts over.
 */
#include "update_receiver.h"

#include <string.h>

#include "frame.h"
#include "image_format.h"
#include "layout.h"
#include "update_port.h"

/* Payload layout (Interfaces Sec. 5.1): identifier (1), image number (2), then for BEGIN the
 * image length (4) and for CHUNK the offset (4) and the data. */
#define ID_BYTES           1U
#define NUMBER_BYTES       2U
#define VALUE_BYTES        4U
#define SHORT_PAYLOAD      (ID_BYTES + NUMBER_BYTES)        /* END, CANCEL: 3 bytes */
#define BEGIN_PAYLOAD      (SHORT_PAYLOAD + VALUE_BYTES)    /* 7 bytes */
#define CHUNK_HEAD         BEGIN_PAYLOAD                    /* CHUNK data starts here */
/* The chip programs one 64-bit flash word at a time [RM0440 Rev 9, Sec. 5.3.7, p. 196]. */
#define FLASH_WORD_BYTES   8U
#define CHUNK_WORDS        (UPLOAD_CHUNK_MAX / sizeof(uint32_t))
/* A last flash word only partly filled is completed with the erased value 0xFF, which leaves
 * those bytes erased [RM0440 Rev 9, Sec. 5.3.7, p. 197]. */
#define ERASED_BYTE        0xFFU
/* The smallest image: header and two signatures (Interfaces Sec. 5.5, Figure 5). */
#define IMAGE_MIN_BYTES    ((uint32_t)IMAGE_HEADER_BYTES + (uint32_t)IMAGE_SIGNATURES_BYTES)

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)((uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8));
}

/* ---- The rules ---------------------------------------------------------------------- */

uint8_t update_slot_of(uint32_t address)
{
    /* Unsigned: an address below the slot wraps to a large number and fails the test. */
    if ((address - (uint32_t)SLOT_A_START) < (uint32_t)SLOT_BYTES) {
        return BOOT_SLOT_A;
    }
    if ((address - (uint32_t)SLOT_B_START) < (uint32_t)SLOT_BYTES) {
        return BOOT_SLOT_B;
    }
    return BOOT_SLOT_NONE;
}

uint32_t update_slot_start(uint8_t slot)
{
    if (slot == BOOT_SLOT_A) {
        return (uint32_t)SLOT_A_START;
    }
    if (slot == BOOT_SLOT_B) {
        return (uint32_t)SLOT_B_START;
    }
    return 0U;
}

uint8_t update_choose_target(bool state_valid, const boot_state_t *state, uint8_t running,
                             uint8_t *target)
{
    *target = BOOT_SLOT_NONE;
    /* Without a boot state the confirmed slot is not known, so no slot is safe to write. */
    if (!state_valid) {
        return REFUSED_IMPOSSIBLE;
    }
    /* The only slot that may be written is the trial's own (REQ-050, Interfaces Sec. 5.1). */
    if (state->trial != BOOT_SLOT_NONE) {
        return REFUSED_TRIAL;
    }
    /* Running a slot that is not confirmed, with no trial: the other slot is the confirmed
     * one, so no slot is both not confirmed and not running (REQ-007). */
    if (((running != BOOT_SLOT_A) && (running != BOOT_SLOT_B)) || (state->confirmed != running)) {
        return REFUSED_IMPOSSIBLE;
    }
    *target = (running == BOOT_SLOT_A) ? BOOT_SLOT_B : BOOT_SLOT_A;
    return UPDATE_OK;
}

bool update_write_allowed(uint8_t target, uint8_t running, uint32_t address, uint32_t bytes)
{
    uint32_t start = update_slot_start(target);

    if ((start == 0U) || (target == running) || (bytes == 0U) || (bytes > (uint32_t)SLOT_BYTES)) {
        return false;
    }
    /* As offsets from the slot start, so no sum can wrap past 2^32. */
    return (address >= start) && ((address - start) <= ((uint32_t)SLOT_BYTES - bytes));
}

uint8_t update_refusal_for(image_result_t result)
{
    switch (result) {
    case IMAGE_OK:
        return UPDATE_OK;
    case IMAGE_BAD_SIGNATURE_A:
    case IMAGE_BAD_SIGNATURE_B:
        return REFUSED_NOT_OPERATOR;
    case IMAGE_BAD_TARGET:
        return REFUSED_OTHER_CONTROLLER;
    case IMAGE_BAD_START:
        return REFUSED_OTHER_SLOT;   /* reset handler outside this slot: linked for the other */
    default:
        /* Body check value, and every other fault of the image's structure: empty, proprietary
         * string, format, kind, length. */
        return REFUSED_DAMAGED;
    }
}

/* ---- Checks on arrival -------------------------------------------------------------- */

bool update_is_upload(const uint8_t *payload, uint16_t length)
{
    return (length >= ID_BYTES) && (payload[0] >= CMD_UPLOAD_BEGIN) &&
           (payload[0] <= CMD_UPLOAD_CANCEL);
}

static bool size_fits(uint8_t id, uint16_t length)
{
    switch (id) {
    case CMD_UPLOAD_BEGIN:
        return length == BEGIN_PAYLOAD;
    case CMD_UPLOAD_CHUNK:
        return (length > CHUNK_HEAD) && (length <= (CHUNK_HEAD + UPLOAD_CHUNK_MAX));
    case CMD_UPLOAD_END:
    case CMD_UPLOAD_CANCEL:
        return length == SHORT_PAYLOAD;
    default:
        return false;
    }
}

/* The payload's fields; false if its size does not fit the command. */
static bool parse(const uint8_t *payload, uint16_t length, update_command_t *cmd)
{
    memset(cmd, 0, sizeof *cmd);
    if ((length < ID_BYTES) || !size_fits(payload[0], length)) {
        return false;
    }
    cmd->id = payload[0];
    cmd->image_number = read_u16(&payload[ID_BYTES]);
    if (length >= BEGIN_PAYLOAD) {
        cmd->value = image_read_u32(&payload[SHORT_PAYLOAD]);
    }
    if (cmd->id == CMD_UPLOAD_CHUNK) {
        cmd->data = &payload[CHUNK_HEAD];
        cmd->data_bytes = (uint32_t)length - CHUNK_HEAD;
    }
    return true;
}

/* The command names the transfer that BEGIN opened, into the same slot. */
static bool same_transfer(const update_receiver_t *rx, const update_command_t *cmd)
{
    return (rx->phase != UPDATE_IDLE) && (cmd->image_number == rx->image_number) &&
           (cmd->target == rx->target);
}

/* A chunk starts at the next byte not yet written and stays inside the announced length.
 * Every chunk but the last is a whole number of flash words, so the next chunk starts on an
 * unwritten flash word: a written one cannot be written again [RM0440 Rev 9, Sec. 5.3.7,
 * p. 196]. */
static bool chunk_fits(const update_receiver_t *rx, const update_command_t *cmd)
{
    uint32_t room = rx->length - rx->written;   /* written never exceeds length */
    bool last = (cmd->data_bytes == room);

    return (cmd->value == rx->written) && (cmd->data_bytes <= room) &&
           (last || ((cmd->data_bytes % FLASH_WORD_BYTES) == 0U));
}

static bool values_fit(const update_receiver_t *rx, const update_command_t *cmd)
{
    switch (cmd->id) {
    case CMD_UPLOAD_BEGIN:
        /* Accepted whatever the phase: a BEGIN restarts an open transfer (run_begin). */
        return (cmd->value >= IMAGE_MIN_BYTES) && (cmd->value <= (uint32_t)SLOT_BYTES);
    case CMD_UPLOAD_CHUNK:
        return (rx->phase == UPDATE_RECEIVING) && same_transfer(rx, cmd) && chunk_fits(rx, cmd);
    case CMD_UPLOAD_END:
        return (rx->phase == UPDATE_RECEIVING) && same_transfer(rx, cmd) &&
               (rx->written == rx->length);
    case CMD_UPLOAD_CANCEL:
        return same_transfer(rx, cmd);
    default:
        return false;
    }
}

uint8_t update_accept(const update_receiver_t *rx, const uint8_t *payload, uint16_t length,
                      update_command_t *cmd)
{
    boot_state_t state = {0};

    if (!parse(payload, length, cmd)) {
        return REFUSED_IMPOSSIBLE;
    }
    bool state_valid = update_port_boot_state(&state);
    uint8_t refusal = update_choose_target(state_valid, &state, rx->running, &cmd->target);
    if (refusal != UPDATE_OK) {
        return refusal;
    }
    return values_fit(rx, cmd) ? UPDATE_OK : REFUSED_IMPOSSIBLE;
}

/* ---- Running a command -------------------------------------------------------------- */

static bool erase_page(const update_receiver_t *rx, uint32_t page)
{
    return update_write_allowed(rx->target, rx->running, page, (uint32_t)FLASH_PAGE_BYTES) &&
           update_port_erase_page(page);
}

/* A BEGIN while a transfer is open restarts it. The onboard computer is the only sender and
 * runs one transfer at a time, so a new BEGIN means it started over (after its own restart, or
 * for a new upload from the ground). Refusing it would block the slot until a CANCEL the
 * sender may not know it owes. Erasing the whole slot removes every byte of the old transfer. */
static uint8_t run_begin(update_receiver_t *rx, const update_command_t *cmd)
{
    uint32_t start = update_slot_start(cmd->target);

    rx->image_number = cmd->image_number;
    rx->target = cmd->target;
    rx->length = cmd->value;
    rx->written = 0U;
    rx->image_passed = false;
    rx->phase = UPDATE_CLOSED;   /* until the whole slot is erased */
    for (uint32_t page = start; page < (start + (uint32_t)SLOT_BYTES); page += FLASH_PAGE_BYTES) {
        /* One page erase stalls the core up to 24.47 ms [DS12589 Rev 6, Table 46, p. 107]; the
         * 20 pages up to 489 ms. Reloaded after each page, so the watchdog never sees the
         * whole erase as one pause. */
        bool erased = erase_page(rx, page);
        update_port_watchdog_reload();
        if (!erased) {
            return REFUSED_DAMAGED;
        }
    }
    rx->phase = UPDATE_RECEIVING;
    return UPDATE_OK;
}

/* Program the chunk at `address`, then read it back through the ECC check. Byte i of the data
 * lands at address + i: the chip stores words little-endian [RM0440 Rev 9, Sec. 2.2.1, p. 82]. */
static bool write_chunk(const update_receiver_t *rx, uint32_t address, const update_command_t *cmd)
{
    uint32_t words[CHUNK_WORDS];
    uint32_t pairs = (cmd->data_bytes + FLASH_WORD_BYTES - 1U) / FLASH_WORD_BYTES;

    memset(words, ERASED_BYTE, sizeof words);
    memcpy(words, cmd->data, cmd->data_bytes);
    if (!update_write_allowed(rx->target, rx->running, address, pairs * FLASH_WORD_BYTES) ||
        !update_port_program(address, words, pairs)) {
        return false;
    }
    update_port_ecc_clear();
    bool same = (memcmp(update_port_flash(address), cmd->data, cmd->data_bytes) == 0);
    return same && !update_port_ecc_error_seen();
}

static uint8_t run_chunk(update_receiver_t *rx, const update_command_t *cmd)
{
    if (!write_chunk(rx, update_slot_start(rx->target) + cmd->value, cmd)) {
        rx->phase = UPDATE_CLOSED;   /* a half-written flash word cannot be written again */
        return REFUSED_DAMAGED;
    }
    rx->written += cmd->data_bytes;
    return UPDATE_OK;
}

/* The bootloader's own checks, in its order (bootloader/main.c, image_passes): contents, then
 * both signatures, then any flash read error seen on the way. */
static uint8_t check_slot(uint8_t slot)
{
    uint32_t start = update_slot_start(slot);
    const uint8_t *image = update_port_flash(start);
    uint32_t signed_bytes = 0U;

    update_port_ecc_clear();
    image_result_t result = image_check_contents(image, start, (uint32_t)SLOT_BYTES,
                                                 IMAGE_KIND_MAIN, &signed_bytes);
    if (result == IMAGE_OK) {
        result = image_check_signatures(image, signed_bytes);
    }
    if (update_port_ecc_error_seen()) {
        return REFUSED_DAMAGED;
    }
    return update_refusal_for(result);
}

static uint8_t run_end(update_receiver_t *rx)
{
    uint8_t result = check_slot(rx->target);

    rx->phase = UPDATE_CLOSED;
    rx->image_passed = (result == UPDATE_OK);
    return result;
}

/* The header is in the slot's first page. Erased, the slot reads as empty to the bootloader
 * (common/image_check.c, IMAGE_EMPTY), which never starts it. On a failed erase the transfer
 * stays as it was, so CANCEL can be sent again. */
static uint8_t run_cancel(update_receiver_t *rx)
{
    if (!erase_page(rx, update_slot_start(rx->target))) {
        return REFUSED_DAMAGED;
    }
    rx->phase = UPDATE_IDLE;
    rx->image_passed = false;
    return UPDATE_OK;
}

void update_init(update_receiver_t *rx, uint8_t running_slot)
{
    memset(rx, 0, sizeof *rx);
    rx->running = running_slot;
    rx->phase = UPDATE_IDLE;
    rx->target = BOOT_SLOT_NONE;
}

uint8_t update_run(update_receiver_t *rx, const update_command_t *cmd)
{
    switch (cmd->id) {
    case CMD_UPLOAD_BEGIN:
        return run_begin(rx, cmd);
    case CMD_UPLOAD_CHUNK:
        return run_chunk(rx, cmd);
    case CMD_UPLOAD_END:
        return run_end(rx);
    case CMD_UPLOAD_CANCEL:
        return run_cancel(rx);
    default:
        return REFUSED_IMPOSSIBLE;   /* not reached: update_accept() takes only these four */
    }
}
