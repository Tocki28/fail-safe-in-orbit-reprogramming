/*
 * fake_port.h — the update receiver's chip calls (src/controller/firmware/update-receiver/
 * update_port.h) on the host: a RAM copy of the controller's 128 KB flash that follows the
 * chip's erase and program rules, a boot state the test sets, and a record of every erase and
 * write, so a test can show that nothing outside the target slot was touched.
 */
#ifndef FAKE_PORT_H
#define FAKE_PORT_H

#include <stdbool.h>
#include <stdint.h>

#include "boot_state.h"
#include "layout.h"

/* FLASH_START .. FLASH_START + 128 KB: up to the end of the boot state (layout.h). */
#define FAKE_FLASH_BYTES  ((uint32_t)BOOT_STATE_START + 2U * (uint32_t)BOOT_STATE_COPY_BYTES - \
                           (uint32_t)FLASH_START)

typedef struct {
    uint8_t flash[FAKE_FLASH_BYTES];   /* index = chip address - FLASH_START */
    bool state_valid;                  /* what update_port_boot_state() returns */
    boot_state_t state;
    uint8_t allowed_slot;              /* the slot the test expects to be written */
    bool fail_erase;                   /* the next erases report a flash error */
    bool fail_program;                 /* the next writes report a flash error */
    bool ecc_error;                    /* reads report a damaged flash word (ECC) */
    uint32_t erases;                   /* pages erased */
    uint32_t programs;                 /* update_port_program() calls that wrote */
    uint32_t outside;                  /* erases or writes outside allowed_slot: must stay 0 */
    uint32_t bad_reads;                /* update_port_flash() outside the fake flash */
    uint32_t reloads;                  /* watchdog reloads */
} fake_t;

extern fake_t fake;

/* Fill every region with its own byte pattern, so a stray write shows. */
void fake_reset(void);
/* Number of bytes in [start, start + bytes) that differ from `value`. */
uint32_t fake_count_not(uint32_t start, uint32_t bytes, uint8_t value);

#endif /* FAKE_PORT_H */
