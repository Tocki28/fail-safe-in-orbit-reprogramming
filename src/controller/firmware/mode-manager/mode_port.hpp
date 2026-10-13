/*
 * mode_port.hpp — everything the mode manager needs from the chip, in one list: the boot state
 * (read and write), the running image in flash (self-test), flash read errors, the restart.
 *
 * On the board: mode_port.cpp, which passes each call to common/boot_state.c, common/flash.c
 * or the core's reset. In the host unit tests: test/controller/unit/fake_mode_port.cpp, which
 * works on the same RAM copy of the flash as the update receiver's fake (fake_port.cpp), counts
 * boot-state writes and restarts, and can make a write fail.
 *
 * C++: a struct of static functions and the build picks the body, as for the update receiver
 * (update-receiver/update_port.hpp says why not virtual functions or a template). The mode
 * manager has its own port, not the update receiver's: each component names what it needs
 * from the chip, and only the mode manager writes the boot state or restarts.
 */
#ifndef MODE_PORT_HPP
#define MODE_PORT_HPP

#include <cstdint>

#include "boot_state.h"

struct ModePort {
    ModePort() = delete;

    /** @brief The newest valid boot state (boot_state_read). False if neither copy is valid. */
    [[nodiscard]] static bool readBootState(boot_state_t &state);

    /** @brief Store confirmed, trial and failed starts (boot_state_write). True if read back equal. */
    [[nodiscard]] static bool writeBootState(boot_state_t &state);

    /** @brief Where the program reads the flash byte at chip address `address`. */
    static const uint8_t *flash(uint32_t address);

    /** @brief Forget earlier flash read errors (flash_ecc_clear). */
    static void eccClear();

    /** @brief True if a flash read since eccClear() hit a damaged word (flash_ecc_error_seen). */
    [[nodiscard]] static bool eccErrorSeen();

    /** @brief Restart the chip. On the board it does not return; the host fake returns. */
    static void restart();
};

#endif /* MODE_PORT_HPP */
