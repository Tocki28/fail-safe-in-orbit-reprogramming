/*
 * update_port.cpp — the update receiver's chip calls on the board (see update_port.hpp). Each
 * one passes straight on; the checks are in update_receiver.cpp.
 */
#include "update_port.hpp"

#include "flash.h"
#include "watchdog.hpp"

/**
 * @brief Reads the newest valid boot state through boot_state_read.
 *
 * @param[out] state  the boot state; filled by boot_state_read
 * @return true if a valid boot state was read, false if neither copy is valid
 */
bool UpdatePort::bootState(boot_state_t &state)
{
    return boot_state_read(&state);
}

/**
 * @brief Erases the 2 KB flash page at page_start through flash_erase_page.
 *
 * @param[in] page_start  chip address of the first byte of the page
 * @return true if the chip reported no error, false otherwise
 */
bool UpdatePort::erasePage(uint32_t page_start)
{
    return flash_erase_page(page_start);
}

/**
 * @brief Programs 64-bit flash words through flash_program.
 *
 * @param[in] start  chip address of the first word, 8-byte aligned, erased
 * @param[in] words  data as 32-bit words; two words make one 64-bit flash word
 * @param[in] pairs  number of 64-bit flash words to program
 * @return true if the chip reported no error, false otherwise
 */
bool UpdatePort::program(uint32_t start, const uint32_t *words, uint32_t pairs)
{
    return flash_program(start, words, pairs);
}

/**
 * @brief Returns a pointer to the flash byte at a chip address.
 *
 * The flash is read in place at its own address [RM0440 Rev 9, Table 28, p. 189].
 * C++: reinterpret_cast names the one thing done here, an address number made a pointer; a
 * C cast would also allow any other conversion without saying so.
 *
 * @param[in] address  chip address of the byte
 * @return read-only pointer to that byte
 */
const uint8_t *UpdatePort::flash(uint32_t address)
{
    return reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(address));
}

/**
 * @brief Forgets earlier flash read errors through flash_ecc_clear.
 */
void UpdatePort::eccClear()
{
    flash_ecc_clear();
}

/**
 * @brief Tells whether a flash read since the last eccClear hit a damaged word.
 *
 * @return true if a read error (ECC) was seen, false otherwise
 */
bool UpdatePort::eccErrorSeen()
{
    return flash_ecc_error_seen();
}

/**
 * @brief Reloads the independent watchdog through Watchdog::reload.
 */
void UpdatePort::watchdogReload()
{
    Watchdog::reload();
}
