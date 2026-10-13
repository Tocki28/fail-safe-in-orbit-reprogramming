/*
 * mode_port.cpp — the mode manager's chip calls on the board (see mode_port.hpp). Each one
 * passes straight on; the decisions are in mode_manager.cpp.
 *
 * The backup-register access below is the firmware's own copy of the few lines the bootloader
 * has in bootloader/backup.c (REQ-119: no code shared between bootloader and firmware). The map
 * and the reasons for these registers are in common/backup_layout.h.
 */
#include "mode_port.hpp"

#include "backup_layout.h"
#include "flash.h"
#include "stm32g4xx.h"

namespace {

/* TAMP_BKPxR sits at offset 0x100 + 0x04 * x from the TAMP base [RM0440 Rev 9, Sec. 38.6.8,
 * p. 1623]. */
constexpr uint32_t kBkpOffset = 0x100U;
constexpr uint32_t kBkpStride = 4U;
/* A pair is two registers: the value, then its complement (common/backup_layout.h). */
constexpr uint32_t kPairWords = 2U;
constexpr uint32_t kByteMask = BACKUP_BYTE_MASK;

static_assert((BACKUP_PAIRS * kPairWords) <= BACKUP_REGISTERS,
              "the backup pairs of common/backup_layout.h fit the 16 TAMP backup registers");

/**
 * @brief Returns backup register `index` (TAMP_BKP<index>R).
 *
 * The address is computed from the TAMP base as the reference manual gives it: TAMP_BASE is
 * 0x4000 2400 [RM0440 Rev 9, Table 3, p. 87]. C++: reinterpret_cast names the one thing done, an
 * address made a reference to a register.
 *
 * @param[in] index  register number, 0 to BACKUP_REGISTERS - 1
 * @return the register
 */
volatile uint32_t &backupRegister(uint32_t index)
{
    return *reinterpret_cast<volatile uint32_t *>(TAMP_BASE + kBkpOffset + (index * kBkpStride));
}


/**
 * @brief Turns on the PWR and RTC APB clocks, which reading and writing the backup registers
 * need.
 *
 * TAMP registers run on the RTC APB clock, RTCAPBEN [RM0440 Rev 9, Table 347, p. 1612;
 * Sec. 7.4.17, p. 310]; DBP lives in PWR_CR1, which needs the PWR clock [RM0440 Rev 9,
 * Sec. 6.1.3, p. 231]. Both stay on: the firmware uses them again, and nothing else is clocked
 * by them.
 */
void clocksOn()
{
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN | RCC_APB1ENR1_RTCAPBEN;
    (void)RCC->APB1ENR1;   /* read back: both clocks run before the next access (uart_link.cpp) */
}

template <uint32_t kPair>
/**
 * @brief Reads a pair; it counts only when the second word is the complement of the first.
 *
 * A broken pair (one bit error, or both words 0 after an RTC domain reset) reads as empty: stored
 * data that failed its check, never a fault (common/backup_layout.h). The pair number is a
 * template argument, checked by the compiler against the map (static_assert), so no wrong number
 * can reach the registers and there is no run-time path for one. C++: the bootloader's C copy
 * checks it at run time instead (bootloader/backup.c).
 *
 * @tparam kPair  BACKUP_PAIR_... (common/backup_layout.h)
 * @param[out] value  the stored value; 0 when the pair is broken
 * @return true if the pair matches, false if it is empty
 */
bool readPair(uint32_t &value)
{
    static_assert(kPair < BACKUP_PAIRS, "a backup pair of common/backup_layout.h");
    constexpr uint32_t index = kPair * kPairWords;

    clocksOn();
    uint32_t word = backupRegister(index);
    uint32_t inverse = backupRegister(index + 1U);
    if (word != ~inverse) {
        value = 0U;
        return false;
    }
    value = word;
    return true;
}

template <uint32_t kPair>
/**
 * @brief Writes a value and its complement into a pair, with the write protection lifted only
 * for these two writes, then reads both back.
 *
 * DBP in PWR_CR1 lifts the backup-domain write protection [RM0440 Rev 9, Sec. 38.3.3, p. 1612;
 * Sec. 6.4.1, p. 253]; it is cleared again at once, so a stray write elsewhere in the firmware
 * cannot change these words. A write the chip ignored shows in the read-back.
 *
 * The pair number is a template argument checked by the compiler, as for readPair.
 *
 * @tparam kPair  BACKUP_PAIR_... (common/backup_layout.h)
 * @param[in] value  value to store
 * @return true if both words read back as written, false otherwise
 */
bool writePair(uint32_t value)
{
    static_assert(kPair < BACKUP_PAIRS, "a backup pair of common/backup_layout.h");
    constexpr uint32_t index = kPair * kPairWords;

    clocksOn();
    PWR->CR1 |= PWR_CR1_DBP;
    (void)PWR->CR1;   /* read back: DBP is set before the first write below */
    backupRegister(index) = value;
    backupRegister(index + 1U) = ~value;
    PWR->CR1 &= ~PWR_CR1_DBP;
    return (backupRegister(index) == value) && (backupRegister(index + 1U) == ~value);
}

}  // namespace

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

/**
 * @brief Leaves the give-up request for the trial in `slot_code` (decision G): the bootloader
 * gives that trial up before it counts its next start (bootloader/main.c, apply_give_up_request).
 *
 * @param[in]     slot_code  BOOT_SLOT_A or BOOT_SLOT_B, the trial slot given up
 * @return true if the request reads back as written, false otherwise
 */
bool ModePort::leaveGiveUpRequest(uint8_t slot_code)
{
    return writePair<BACKUP_PAIR_GIVE_UP>(slot_code);
}

/**
 * @brief Reads the bootloader's fall-back record (FALLBACK pair, common/backup_layout.h), split
 * into one entry per copy and the copy started.
 *
 * @param[out]    record  each copy's check result, and the copy started; every check result and
 *                        the started copy 0 when none is stored
 * @return true if a record is stored (matching pair, value not 0), false otherwise
 */
bool ModePort::readFallbackRecord(FallbackRecord &record)
{
    uint32_t value = 0U;
    bool stored = readPair<BACKUP_PAIR_FALLBACK>(value) && (value != 0U);

    record.copies[0] = {BACKUP_COPY_A,
                        static_cast<uint8_t>((value >> BACKUP_FALLBACK_A_SHIFT) & kByteMask)};
    record.copies[1] = {BACKUP_COPY_B,
                        static_cast<uint8_t>((value >> BACKUP_FALLBACK_B_SHIFT) & kByteMask)};
    record.copies[2] = {BACKUP_COPY_LAST_RESORT,
                        static_cast<uint8_t>((value >> BACKUP_FALLBACK_LAST_RESORT_SHIFT) &
                                             kByteMask)};
    record.started = static_cast<uint8_t>((value >> BACKUP_FALLBACK_STARTED_SHIFT) & kByteMask);
    return stored;
}

/**
 * @brief Clears the fall-back record, so its EVENT goes out once (Interfaces Sec. 5.4).
 *
 * @return true if the cleared record reads back, false otherwise
 */
bool ModePort::clearFallbackRecord()
{
    return writePair<BACKUP_PAIR_FALLBACK>(0U);
}

/**
 * @brief Reads the first fault location the bootloader's fault reaction left (FAULT_LOCATION
 * pair) and the number of faults it found after it (FAULT_LATER pair), common/backup_layout.h;
 * decision 3.
 *
 * The count is read only when a location is stored: the bootloader sets it to 0 when it writes
 * a first location, so a count without a location is a leftover and means nothing.
 *
 * @param[out]    location  the stored location as read; 0 when none is stored
 * @param[out]    later     faults found after the first; 0 when no location is stored or the
 *                          count's pair is broken
 * @return true if a location is stored (matching pair, value not 0), false otherwise
 */
bool ModePort::readBootloaderFault(uint32_t &location, uint32_t &later)
{
    later = 0U;
    if (!readPair<BACKUP_PAIR_FAULT_LOCATION>(location) || (location == 0U)) {
        return false;
    }
    if (!readPair<BACKUP_PAIR_FAULT_LATER>(later)) {
        later = 0U;
    }
    return true;
}

/**
 * @brief Clears the bootloader's fault location and the count after it, so its EVENT 0x0005 goes
 * out once (decision 3).
 *
 * The count is cleared first: if only the location clear succeeds, the next fault starts a new
 * count from 0 anyway (bootloader/backup.c).
 *
 * @return true if both cleared pairs read back, false otherwise
 */
bool ModePort::clearBootloaderFault()
{
    const bool later_cleared = writePair<BACKUP_PAIR_FAULT_LATER>(0U);

    return writePair<BACKUP_PAIR_FAULT_LOCATION>(0U) && later_cleared;
}

/**
 * @brief Clears decision B's restart count if it belongs to `slot_code`.
 *
 * Only this slot's count: a count of the other slot is not this firmware's to clear (a trial
 * running normally says nothing about the confirmed slot's restarts). A broken pair reads as no
 * count and is left as it is.
 *
 * @param[in]     slot_code  the running slot's code, BOOT_SLOT_A or BOOT_SLOT_B
 * @return true if there was nothing to clear or the cleared count reads back; false if the
 *         write failed
 */
bool ModePort::clearRestartCount(uint8_t slot_code)
{
    uint32_t value = 0U;

    if (!readPair<BACKUP_PAIR_RESTARTS>(value) ||
        (((value >> BACKUP_RESTARTS_SLOT_SHIFT) & kByteMask) != slot_code)) {
        return true;
    }
    return writePair<BACKUP_PAIR_RESTARTS>(0U);
}

/**
 * @brief Marks this restart as a fault reaction, then restarts the chip (decision B, decision L,
 * owner's fault decision of 13 Oct 2026).
 *
 * The mark (FAULT pair) makes the bootloader count this software restart like a watchdog restart
 * of the confirmed slot, so a loop of fault restarts ends in the last-resort firmware after
 * three (bootloader/main.c, count_restart). The reset: CMSIS NVIC_SystemReset sets SYSRESETREQ
 * in AIRCR [Arm DUI 0553A, Sec. 4.3.5, Table 4-17, p. 4-17], a software reset of the chip
 * [RM0440 Rev 9, Sec. 7.1.2, p. 271].
 *
 * @param[in]     reason  BACKUP_FAULT_IMPOSSIBLE_VALUE or BACKUP_FAULT_SELF_TEST
 * @note Does not return.
 */
void ModePort::restartCounted(uint8_t reason)
{
    /* The restart follows either way. If the mark could not be stored, the backup domain cannot
     * be written at all; the bootloader then does not count this restart, and nothing here can do
     * better. */
    (void)writePair<BACKUP_PAIR_FAULT>(reason);
    NVIC_SystemReset();
}
