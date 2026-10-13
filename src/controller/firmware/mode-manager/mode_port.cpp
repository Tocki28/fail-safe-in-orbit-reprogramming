/*
 * mode_port.cpp — the mode manager's chip calls on the board (see mode_port.hpp). Each one
 * passes straight on; the decisions are in mode_manager.cpp.
 */
#include "mode_port.hpp"

#include "flash.h"
#include "stm32g4xx.h"

/**
 * @brief Reads the newest valid boot state (common/boot_state.c, boot_state_read).
 *
 * @param[out]    state  the boot state; not meaningful when false is returned
 * @return true if a valid copy was read, false if neither copy is valid
 */
bool ModePort::readBootState(boot_state_t &state)
{
    return boot_state_read(&state);
}

/**
 * @brief Writes the boot state to the copy that is not current, then reads it back
 * (common/boot_state.c, boot_state_write).
 *
 * @param[in,out] state  confirmed, trial and failed_starts to store; sequence and check are set
 * @return true if written and read back equal; false if the fields are not valid, or the erase,
 *         the programming or the read-back failed
 */
bool ModePort::writeBootState(boot_state_t &state)
{
    return boot_state_write(&state);
}

/**
 * @brief The address of a flash byte as a pointer: the flash is read in place at its own address
 * [RM0440 Rev 9, Table 28, p. 189].
 *
 * @param[in]     address  chip address of the byte
 * @return pointer to that byte
 */
const uint8_t *ModePort::flash(uint32_t address)
{
    return reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(address));
}

/**
 * @brief Forgets earlier flash read errors (common/flash.c, flash_ecc_clear).
 */
void ModePort::eccClear()
{
    flash_ecc_clear();
}

/**
 * @brief Returns true if a read since eccClear() hit a word with two wrong bits
 * (common/flash.c, flash_ecc_error_seen).
 *
 * @return true if a damaged flash word was read, false otherwise
 */
bool ModePort::eccErrorSeen()
{
    return flash_ecc_error_seen();
}

/**
 * @brief Restarts the chip through the core's system reset request.
 *
 * CMSIS NVIC_SystemReset (core_cm4.h): waits for outstanding memory writes (DSB), sets
 * SYSRESETREQ in SCB->AIRCR, then loops until the reset takes effect. The bootloader logs the
 * cause as "software" (bootloader/main.c, log_reset_cause) and reads the boot state again.
 * Rejected: stopping the watchdog reload and waiting up to 1.88 s for the watchdog; slower,
 * and the bootloader would log the restart as a hang.
 *
 * @note Does not return.
 */
void ModePort::restart()
{
    NVIC_SystemReset();
}
