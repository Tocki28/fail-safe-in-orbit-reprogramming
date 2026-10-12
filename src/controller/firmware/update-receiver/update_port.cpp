/*
 * update_port.cpp — the update receiver's chip calls on the board (see update_port.hpp). Each
 * one passes straight on; the checks are in update_receiver.cpp.
 */
#include "update_port.hpp"

#include "flash.h"
#include "watchdog.hpp"

bool UpdatePort::bootState(boot_state_t &state)
{
    return boot_state_read(&state);
}

bool UpdatePort::erasePage(uint32_t page_start)
{
    return flash_erase_page(page_start);
}

bool UpdatePort::program(uint32_t start, const uint32_t *words, uint32_t pairs)
{
    return flash_program(start, words, pairs);
}

/* The flash is read in place at its own address [RM0440 Rev 9, Table 28, p. 189].
 * C++: reinterpret_cast names the one thing done here, an address number made a pointer; a
 * C cast would also allow any other conversion without saying so. */
const uint8_t *UpdatePort::flash(uint32_t address)
{
    return reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(address));
}

void UpdatePort::eccClear()
{
    flash_ecc_clear();
}

bool UpdatePort::eccErrorSeen()
{
    return flash_ecc_error_seen();
}

void UpdatePort::watchdogReload()
{
    Watchdog::reload();
}
