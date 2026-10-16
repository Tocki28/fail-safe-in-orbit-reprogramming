/*
 * bl_boot_state.h — the bootloader's own reader and writer of the boot state (REQ-119): which
 * slot is confirmed, which is on trial, how often the trial has started. Same meanings as
 * common/boot_state.h, which the firmware runs; written apart from it (bl_boot_state.c says what
 * differs), so a defect in one is not in both (bl_crc32.h says why: owner's decision 35,
 * 15 Oct 2026).
 *
 * The record and its codes are still defined once, in common/boot_state.h (boot_state_t,
 * boot_state_load_t, BOOT_SLOT_..., BOOT_STATE_MAX_STARTS): definitions only, no code. The
 * firmware and the bootloader read each other's records, so the bytes in flash must be the same:
 * the layout of boot_state_t, its CRC-32 (Interfaces Sec. 4.0, bl_crc32.h) and the two copies of
 * layout.h. Stored twice, each copy with a CRC-32 checked before use (REQ-136); a read takes the
 * valid copy with the higher sequence number; a write goes to the other copy with the sequence
 * number one higher, so a power cut in a write leaves the previous state readable (REQ-017).
 */
#ifndef BL_BOOT_STATE_H
#define BL_BOOT_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "boot_state.h"

/* Fill *state from the newest valid copy. False if neither copy is valid. */
bool bl_boot_state_read(boot_state_t *state);

/* Read both copies and say which case holds (decision D): on BOOT_STATE_VALID *state is the
 * newest valid copy, otherwise *state is not written. */
boot_state_load_t bl_boot_state_load(boot_state_t *state);

/* The decision of bl_boot_state_load on two records already read: readable[i] is false when the
 * read of copy i saw a flash ECC error. No flash access, so the host tests call it directly. */
boot_state_load_t bl_boot_state_classify(const boot_state_t copies[2], const bool readable[2],
                                         boot_state_t *state);

/* Store confirmed, trial, failed_starts, flags and given_up_check from *state (owner's decision
 * 30, 15 Oct 2026: flags and given_up_check are no longer written as 0). Sets state->sequence and
 * check to the values written. False if the fields are not allowed, the flash reported an error,
 * or the read-back did not match. */
bool bl_boot_state_write(boot_state_t *state);

/* The other slot of the pair: A for B, B for A. Any other code is a corrupted value: the fault
 * reaction (common/fault.h). */
uint8_t bl_boot_state_other_slot(uint8_t slot);

#endif /* BL_BOOT_STATE_H */
