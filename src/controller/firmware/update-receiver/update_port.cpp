/*
 * update_port.c — the update receiver's chip calls on the board (see update_port.h). Each one
 * passes straight on; the checks are in update_receiver.c.
 */
#include "update_port.h"

#include "flash.h"
#include "watchdog.h"

bool update_port_boot_state(boot_state_t *state)
{
    return boot_state_read(state);
}

bool update_port_erase_page(uint32_t page_start)
{
    return flash_erase_page(page_start);
}

bool update_port_program(uint32_t start, const uint32_t *words, uint32_t pairs)
{
    return flash_program(start, words, pairs);
}

/* The flash is read in place at its own address [RM0440 Rev 9, Table 28, p. 189]. */
const uint8_t *update_port_flash(uint32_t address)
{
    return (const uint8_t *)(uintptr_t)address;
}

void update_port_ecc_clear(void)
{
    flash_ecc_clear();
}

bool update_port_ecc_error_seen(void)
{
    return flash_ecc_error_seen();
}

void update_port_watchdog_reload(void)
{
    watchdog_reload();
}
