/*
 * boot_state.c — two-copy storage of the boot state (see boot_state.h).
 *
 * Wear: each write erases one page. A page lasts 10 000 erases [DS12589 Rev 6, Table 47,
 * p. 107] and writes alternate between the two copies, so the state survives about 20 000
 * writes. Writes happen once per trial start and once per confirmation, not on normal starts.
 * The same limit means the 32-bit sequence number never wraps.
 */
#include "boot_state.h"

#include <stddef.h>
#include <string.h>

#include "crc32.h"
#include "flash.h"
#include "layout.h"

/* The check value covers every byte before it. */
#define CHECKED_BYTES  ((uint32_t)offsetof(boot_state_t, check))
/* The record is programmed as 64-bit flash words, two 32-bit words each. */
#define RECORD_WORDS   (sizeof(boot_state_t) / sizeof(uint32_t))
#define RECORD_PAIRS   (sizeof(boot_state_t) / sizeof(uint64_t))
/* Returned by newest_copy() when neither copy is valid. */
#define NO_VALID_COPY  2U

_Static_assert(sizeof(boot_state_t) == 16U, "boot state must be two 64-bit flash words");

static const uint32_t COPY_START[2] = {BOOT_STATE_COPY_0, BOOT_STATE_COPY_1};

static uint32_t record_check(const boot_state_t *state)
{
    return crc32_compute((const uint8_t *)state, CHECKED_BYTES);
}

/* A record with a good check value must still name a real slot, and the trial slot can only
 * be the slot that is not confirmed. */
static bool fields_valid(const boot_state_t *state)
{
    bool confirmed_ok = (state->confirmed == BOOT_SLOT_A) || (state->confirmed == BOOT_SLOT_B);
    bool trial_ok = (state->trial == BOOT_SLOT_NONE) ||
                    (state->trial == boot_state_other_slot(state->confirmed));
    return confirmed_ok && trial_ok;
}

/* Copy one stored record to RAM and check it there, just before use (REQ-136). False if the
 * copy is blank, damaged, or was cut short by a power loss (two-bit flash error). */
static bool read_copy(uint32_t copy, boot_state_t *out)
{
    flash_ecc_clear();
    memcpy(out, (const void *)(uintptr_t)COPY_START[copy], sizeof(*out));
    if (flash_ecc_error_seen()) {
        return false;
    }
    return (out->check == record_check(out)) && fields_valid(out);
}

/* The copy to trust: the valid one with the higher sequence number. */
static uint32_t newest_copy(boot_state_t *out)
{
    boot_state_t copy0;
    boot_state_t copy1;
    bool ok0 = read_copy(0U, &copy0);
    bool ok1 = read_copy(1U, &copy1);

    if (ok0 && (!ok1 || (copy0.sequence >= copy1.sequence))) {
        *out = copy0;
        return 0U;
    }
    if (ok1) {
        *out = copy1;
        return 1U;
    }
    return NO_VALID_COPY;
}

bool boot_state_read(boot_state_t *state)
{
    return newest_copy(state) != NO_VALID_COPY;
}

bool boot_state_write(boot_state_t *state)
{
    boot_state_t current;
    uint32_t newest = newest_copy(&current);
    /* Overwrite the copy that is NOT current, so the current one survives a power cut. */
    uint32_t target = (newest == 0U) ? 1U : 0U;
    uint32_t words[RECORD_WORDS];
    boot_state_t back;

    if (!fields_valid(state)) {
        return false;
    }
    state->sequence = (newest == NO_VALID_COPY) ? 1U : (current.sequence + 1U);
    state->unused = 0U;
    state->reserved = 0U;
    state->check = record_check(state);
    memcpy(words, state, sizeof(words));

    /* The record sits at the start of the copy, so erasing the copy's first page is enough. */
    bool ok = flash_erase_page(COPY_START[target]) &&
              flash_program(COPY_START[target], words, RECORD_PAIRS);
    /* Read back through the same checks a later start will use. */
    return ok && read_copy(target, &back) && (memcmp(&back, state, sizeof(back)) == 0);
}

uint8_t boot_state_other_slot(uint8_t slot)
{
    return (slot == BOOT_SLOT_A) ? BOOT_SLOT_B : BOOT_SLOT_A;
}
