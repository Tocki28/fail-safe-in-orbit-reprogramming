/*
 * update_port.h — everything the update receiver needs from the chip, in one list.
 *
 * On the board: update_port.c, which passes each call to common/flash.c, common/boot_state.c
 * or the watchdog. In the host unit tests: test/controller/unit/fake_port.c, which keeps a RAM
 * copy of the 128 KB flash and records every erase and write. So update_receiver.c holds no
 * register access and compiles on both.
 */
#ifndef UPDATE_PORT_H
#define UPDATE_PORT_H

#include <stdbool.h>
#include <stdint.h>

#include "boot_state.h"

/* The newest valid boot state (boot_state_read). False if neither copy is valid. */
bool update_port_boot_state(boot_state_t *state);

/* Erase the 2 KB page at page_start (flash_erase_page). True if the chip reported no error. */
bool update_port_erase_page(uint32_t page_start);

/* Program pairs of 32-bit words, one 64-bit flash word per pair, at an 8-byte aligned erased
 * address (flash_program). True if the chip reported no error. */
bool update_port_program(uint32_t start, const uint32_t *words, uint32_t pairs);

/* Where the program reads the flash byte at chip address `address`. */
const uint8_t *update_port_flash(uint32_t address);

/* Forget earlier flash read errors / true if a read since then hit a damaged word (ECC). */
void update_port_ecc_clear(void);
bool update_port_ecc_error_seen(void);

/* Reload the independent watchdog. */
void update_port_watchdog_reload(void);

#endif /* UPDATE_PORT_H */
