/*
 * fz_ramflash.h — the controller's 128 KB flash as RAM, for the C fuzz targets: one array and
 * two sets of entry points, flash_* for common/boot_state.c and bl_flash_* for
 * bootloader/bl_boot_state.c and the bootloader's main.c.
 *
 * It follows the chip's rules as test/controller/unit/test_separation.c does (an erase gives
 * 0xFF; a program needs an 8-byte aligned address and erased bytes), and can raise a flash ECC
 * error on a read inside a chosen range, as a double-bit error does [RM0440 Rev 9, Sec. 5.3.2,
 * p. 190]. Written here and not taken from test_separation.c, which keeps its model private.
 *
 * Host tests only (CODE_RULES A10b).
 */
#ifndef FZ_RAMFLASH_H
#define FZ_RAMFLASH_H

#include <stdbool.h>
#include <stdint.h>

#include "layout.h"

/* Ranges of the flash whose reads raise an ECC error. */
#define RAMFLASH_ECC_RANGES 2U

/* Every byte 0xFF (erased), no ECC range, no ECC error seen. */
void ramflash_reset(void);
/* Pointer into the array for a chip address; NULL if the address is outside the flash. */
uint8_t *ramflash_ptr(uint32_t address);
/* Reads at addresses in [start, start + bytes) raise the ECC error; slot 0 or 1. */
void ramflash_set_ecc(uint32_t slot, uint32_t start, uint32_t bytes);
/* Clear both ranges. */
void ramflash_clear_ecc(void);
/* Number of erases and programs since reset. */
uint32_t ramflash_erases(void);
uint32_t ramflash_programs(void);
/* The next erase, or the next program, fails (reports a flash error and changes nothing). */
void ramflash_fail_next(bool erase, bool program);

#endif /* FZ_RAMFLASH_H */
