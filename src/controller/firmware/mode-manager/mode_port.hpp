/*
 * mode_port.hpp — everything the mode manager needs from the chip, in one list: the boot state
 * (read and write), the running image in flash (self-test), flash read errors, the TAMP backup
 * registers it shares with the bootloader (common/backup_layout.h), and the restarts.
 *
 * On the board: mode_port.cpp, which passes each call to common/boot_state.c, common/flash.c,
 * the backup registers or the core's reset. In the host unit tests:
 * test/controller/unit/fake_mode_port.cpp, which works on the same RAM copy of the flash as the
 * update receiver's fake (fake_port.cpp), keeps the backup registers as RAM, counts boot-state
 * writes and restarts, and can make a write fail.
 *
 * C++: a struct of static functions and the build picks the body, as for the update receiver
 * (update-receiver/update_port.hpp says why not virtual functions or a template). The mode
 * manager has its own port, not the update receiver's: each component names what it needs
 * from the chip, and only the mode manager writes the boot state or restarts.
 */
#ifndef MODE_PORT_HPP
#define MODE_PORT_HPP

#include <array>
#include <cstdint>

#include "boot_state.h"

/* The copies the bootloader can refuse or skip on one start: slot A, slot B, the last-resort
 * firmware (common/backup_layout.h, FALLBACK). */
constexpr uint32_t kFallbackCopies = 3U;

/* One copy's entry in the bootloader's fall-back record. */
struct FallbackEntry {
    uint8_t copy;    /* the copy: 0x01 slot A, 0x02 slot B, 0x03 last-resort */
    uint8_t check;   /* its check result, Interfaces Sec. 5.4 "Check result" table; 0x00 when it
                      * was not refused or skipped */
};

/* The bootloader's fall-back record, as read from the FALLBACK pair (common/backup_layout.h):
 * every copy refused or skipped on one start (decision 4). Before, one refused copy. */
struct FallbackRecord {
    std::array<FallbackEntry, kFallbackCopies> copies;   /* slot A, slot B, last-resort */
    uint8_t started;                                     /* copy started, same codes as copy */
};

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

    /** @brief Leave the give-up request for `slot_code` (decision G). True if read back. */
    [[nodiscard]] static bool leaveGiveUpRequest(uint8_t slot_code);

    /** @brief Read the bootloader's fall-back record. False if none is stored. */
    [[nodiscard]] static bool readFallbackRecord(FallbackRecord &record);

    /** @brief Clear the fall-back record. True if read back. */
    [[nodiscard]] static bool clearFallbackRecord();

    /** @brief Read the first fault location the bootloader left and the number of faults after
     *  it (decision 3). False if no location is stored; `later` is 0 then, and also when its
     *  pair is broken. */
    [[nodiscard]] static bool readBootloaderFault(uint32_t &location, uint32_t &later);

    /** @brief Clear the bootloader's fault location and its later-faults count. True if both
     *  read back. */
    [[nodiscard]] static bool clearBootloaderFault();

    /** @brief Clear decision B's restart count if it belongs to `slot_code`. True unless the
     *  write failed. */
    [[nodiscard]] static bool clearRestartCount(uint8_t slot_code);

    /** @brief Mark the restart as a fault reaction (`reason`, BACKUP_FAULT_...), then restart. On
     *  the board it does not return; the host fake returns. */
    static void restartCounted(uint8_t reason);
};

#endif /* MODE_PORT_HPP */
