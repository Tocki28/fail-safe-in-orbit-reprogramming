/*
 * boot_state.h — the data kept over restarts: which slot is confirmed, which one is on trial,
 * how often the trial has been started, whether the controller is in FALLBACK, and which image
 * was given up last (owner's decision 30, 15 Oct 2026).
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

/* Trial starts allowed before the trial is given up. Bench value; the requirement's value is
 * TBD-starts in the SRS (REQ-015). One number for both sides: the bootloader counts to it
 * (bootloader/main.c, count_trial_start) and boot_state.c refuses a stored count above it, so a
 * damaged count cannot be read as valid. */
#define BOOT_STATE_MAX_STARTS 3U

/* Bit 0 of the flags field: the controller is in FALLBACK (owner's decision 30, 15 Oct 2026;
 * ConOps mode table, FALLBACK; REQ-117). Set by every give-up of a trial, cleared by CONFIRM,
 * SWITCH, START FAILED and LEAVE FALLBACK. No other bit is defined: a stored record with any other
 * bit set fails its checks (damage, decision D), never a fault and never a guessed value. */
#define BOOT_STATE_FLAG_FALLBACK 0x01U

/* 16 bytes: two 64-bit flash words, the unit the chip programs [RM0440 Rev 9, Sec. 5.3.7,
 * p. 196]. Field order fixes the byte layout (no padding: 4 + 4 x 1 + 4 + 4).
 * Bytes 7 and 8-11 were "unused" and "reserved", written as 0, until owner's decision 30: a record
 * written before it (flags 0, given_up_check 0) stays valid and means no FALLBACK and no given-up
 * image. */
typedef struct {
    uint32_t sequence;      /* one higher on every write; the higher valid copy is current */
    uint8_t confirmed;      /* BOOT_SLOT_A or BOOT_SLOT_B */
    uint8_t trial;          /* BOOT_SLOT_NONE, or the slot that is not confirmed */
    uint8_t failed_starts;  /* trial starts so far, 0 to BOOT_STATE_MAX_STARTS; reset to 0 when the trial ends */
    uint8_t flags;          /* 0 or BOOT_STATE_FLAG_FALLBACK (decision 30) */
    uint32_t given_up_check;   /* body check value (image header, common/image_format.h) of the
                                * image most recently given up; 0 = none (decision 30, REQ-019) */
    uint32_t check;         /* CRC-32 of the 12 bytes above (REQ-136) */
} boot_state_t;

/* What the two stored copies hold (decision D, owner, 13 Oct 2026). Every enumerator is a value
 * the bootloader acts on (bootloader/main.c, load_state). */
typedef enum {
    BOOT_STATE_VALID = 0,   /* a copy passed its checks; the newest such copy is used */
    BOOT_STATE_BLANK,       /* both records erased (all 16 bytes 0xFF): never written */
    BOOT_STATE_DAMAGED      /* no valid copy, and at least one copy is not erased */
} boot_state_load_t;

/* Fill *state from the newest valid copy. False if neither copy is valid. */
bool boot_state_read(boot_state_t *state);

/* Read both copies and say which of the three cases above holds; on BOOT_STATE_VALID *state is
 * the newest valid copy, otherwise *state is not written. */
boot_state_load_t boot_state_load(boot_state_t *state);

/* The decision of boot_state_load on two records already read: readable[i] is false when the
 * read of copy i saw a flash ECC error. Has no flash access, so the host tests call it directly. */
boot_state_load_t boot_state_classify(const boot_state_t copies[2], const bool readable[2],
                                      boot_state_t *state);

/* Store confirmed, trial, failed_starts, flags and given_up_check from *state. Sets
 * state->sequence and state->check to the values written. False if the fields are not allowed,
 * the flash reported an error or the read-back did not match. */
bool boot_state_write(boot_state_t *state);

/* The other slot of the pair: A for B, B for A. Any other code is a corrupted value: the fault
 * reaction (common/fault.h). */
uint8_t boot_state_other_slot(uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_STATE_H */
