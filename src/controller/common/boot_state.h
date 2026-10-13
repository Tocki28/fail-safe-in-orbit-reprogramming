/*
 * boot_state.h — the data kept over restarts: which slot is confirmed, which one is on trial,
 * and how often the trial has been started.
 *
 * Stored twice (layout.h, BOOT_STATE_COPY_0 and _1), each copy with a CRC-32 that is checked
 * before use (REQ-136). A read takes the valid copy with the higher sequence number. A write
 * goes to the other copy with the sequence number one higher, so a power cut in the middle
 * of a write leaves the previous state readable (REQ-017).
 */
#ifndef BOOT_STATE_H
#define BOOT_STATE_H

#include <stdbool.h>
#include <stdint.h>

/* C linkage when a C++ file includes this (the main firmware): the functions are compiled as C,
 * so their names carry no C++ mangling. Nothing changes for the C compiler. */
#ifdef __cplusplus
extern "C" {
#endif

/* Slot codes as stored in flash. 0 means "none" for the trial field. */
#define BOOT_SLOT_NONE 0U
#define BOOT_SLOT_A    1U
#define BOOT_SLOT_B    2U

/* 16 bytes: two 64-bit flash words, the unit the chip programs [RM0440 Rev 9, Sec. 5.3.7,
 * p. 196]. Field order fixes the byte layout (no padding: 4 + 4 x 1 + 4 + 4). */
typedef struct {
    uint32_t sequence;      /* one higher on every write; the higher valid copy is current */
    uint8_t confirmed;      /* BOOT_SLOT_A or BOOT_SLOT_B */
    uint8_t trial;          /* BOOT_SLOT_NONE, or the slot that is not confirmed */
    uint8_t failed_starts;  /* trial starts so far; reset to 0 when the trial ends */
    uint8_t unused;         /* written as 0 */
    uint32_t reserved;      /* written as 0; fills the second flash word */
    uint32_t check;         /* CRC-32 of the 12 bytes above (REQ-136) */
} boot_state_t;

/* Fill *state from the newest valid copy. False if neither copy is valid. */
bool boot_state_read(boot_state_t *state);

/* Store confirmed, trial and failed_starts from *state. Sets state->sequence and
 * state->check to the values written. False if the flash reported an error or the
 * read-back did not match. */
bool boot_state_write(boot_state_t *state);

/* The other slot of the pair: A for B, B for A, BOOT_SLOT_NONE for any other code. */
uint8_t boot_state_other_slot(uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_STATE_H */
