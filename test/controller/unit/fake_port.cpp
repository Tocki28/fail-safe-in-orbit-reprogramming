/*
 * fake_port.cpp — see fake_port.hpp. Chip rules copied from RM0440 Rev 9:
 *   erase: a 2 KB page at a page boundary, to 0xFF (Sec. 5.3.6, p. 195);
 *   program: 8 bytes at an 8-byte aligned address, only over erased bytes (Sec. 5.3.7, p. 196).
 */
#include "fake_port.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "update_port.hpp"

Fake fake;

namespace {

constexpr uint8_t kErased = 0xFFU;
constexpr uint32_t kWordBytes = 8U;
/* Region patterns, so a stray write is visible. */
constexpr uint8_t kFillBootloader = 0xB0U;
constexpr uint8_t kFillSlotA = 0xAAU;
constexpr uint8_t kFillSlotB = 0xBBU;
constexpr uint8_t kFillLastResort = 0x1EU;
constexpr uint8_t kFillBootState = 0x5EU;

/**
 * @brief Sets a range of the fake flash to one byte value.
 *
 * @param[in] start   chip address of the first byte, inside the fake flash
 * @param[in] bytes   number of bytes
 * @param[in] value   byte value written to each
 */
void fill(uint32_t start, uint32_t bytes, uint8_t value)
{
    std::memset(&fake.flash[start - kFlashStart], value, bytes);
}

/**
 * @brief Tells whether a range lies inside the slot the test allows.
 *
 * True if [address, address + bytes) is inside the slot the test allows.
 *
 * @param[in] address  chip address of the first byte
 * @param[in] bytes    length of the range in bytes
 * @return true if the range is inside the allowed slot, false otherwise
 */
bool inAllowedSlot(uint32_t address, uint32_t bytes)
{
    uint32_t start = (fake.allowed_slot == Slot::kA) ? kSlotAStart : kSlotBStart;

    return (address >= start) && (bytes <= kSlotBytes) && ((address - start) <= (kSlotBytes - bytes));
}

/**
 * @brief Tells whether a range lies inside the fake flash.
 *
 * @param[in] address  chip address of the first byte
 * @param[in] bytes    length of the range in bytes
 * @return true if the range is inside the fake flash, false otherwise
 */
bool inFlash(uint32_t address, uint32_t bytes)
{
    return (address >= kFlashStart) && (bytes <= kFakeFlashBytes) &&
           ((address - kFlashStart) <= (kFakeFlashBytes - bytes));
}

}  // namespace

/**
 * @brief Resets the fake chip to its start state.
 *
 * Clears every counter and flag, fills each flash region with its own pattern, makes the boot
 * state valid with slot A confirmed and no trial, and allows writes to slot B only. The backup
 * registers are all 0 (every pair empty) and writable.
 */
void fakeReset()
{
    fake = Fake{};   /* every field 0 (C: memset) */
    fill(BOOTLOADER_START, BOOTLOADER_BYTES, kFillBootloader);
    fill(SLOT_A_START, SLOT_BYTES, kFillSlotA);
    fill(SLOT_B_START, SLOT_BYTES, kFillSlotB);
    fill(LAST_RESORT_START, LAST_RESORT_BYTES, kFillLastResort);
    fill(BOOT_STATE_START, 2U * BOOT_STATE_COPY_BYTES, kFillBootState);
    fake.state_valid = true;
    fake.state.confirmed = BOOT_SLOT_A;
    fake.state.trial = BOOT_SLOT_NONE;
    fake.allowed_slot = Slot::kB;
    fake.backup_writable = true;
}

/**
 * @brief Counts the bytes of a flash range that differ from a value.
 *
 * @param[in] start  chip address of the first byte
 * @param[in] bytes  length of the range in bytes
 * @param[in] value  byte value to compare with
 * @return number of bytes in the range not equal to value
 */
uint32_t fakeCountNot(uint32_t start, uint32_t bytes, uint8_t value)
{
    uint32_t count = 0U;

    for (uint32_t i = 0U; i < bytes; i++) {
        count += (fake.flash[start - kFlashStart + i] != value) ? 1U : 0U;
    }
    return count;
}

/**
 * @brief Gives the boot state the test has set.
 *
 * @param[out] state  copy of the fake boot state
 * @return the fake's state_valid flag
 */
bool UpdatePort::bootState(boot_state_t &state)
{
    state = fake.state;
    return fake.state_valid;
}

/**
 * @brief Erases a page of the fake flash to 0xFF.
 *
 * A page outside the allowed slot is counted in fake.outside and refused. A forced failure or
 * a start that is not on a page boundary fails without erasing. Success counts in fake.erases.
 *
 * @param[in] page_start  chip address of the page's first byte
 * @return true if the page was erased, false otherwise
 */
bool UpdatePort::erasePage(uint32_t page_start)
{
    if (!inAllowedSlot(page_start, kFlashPageBytes)) {
        fake.outside++;
        return false;
    }
    if (fake.fail_erase || (((page_start - kFlashStart) % kFlashPageBytes) != 0U)) {
        return false;
    }
    fill(page_start, kFlashPageBytes, kErased);
    fake.erases++;
    return true;
}

/**
 * @brief Programs 64-bit words into the fake flash, with the chip's rules.
 *
 * A range outside the allowed slot is counted in fake.outside and refused. A forced failure, a
 * start not 8-byte aligned or bytes not erased fail without writing. Success counts in
 * fake.programs.
 *
 * @param[in] start  chip address of the first word
 * @param[in] words  data as 32-bit words
 * @param[in] pairs  number of 64-bit flash words
 * @return true if the words were written, false otherwise
 */
bool UpdatePort::program(uint32_t start, const uint32_t *words, uint32_t pairs)
{
    uint32_t bytes = pairs * kWordBytes;

    if (!inAllowedSlot(start, bytes)) {
        fake.outside++;
        return false;
    }
    if (fake.fail_program || ((start % kWordBytes) != 0U) ||
        (fakeCountNot(start, bytes, kErased) != 0U)) {
        return false;   /* PGAERR, or PROGERR: written over bytes that were not erased */
    }
    std::memcpy(&fake.flash[start - kFlashStart], words, bytes);
    fake.programs++;
    return true;
}

/**
 * @brief Gives a pointer into the fake flash for a chip address.
 *
 * An address outside the fake flash is counted in fake.bad_reads, reported on stdout, and
 * answered with the start of the fake flash.
 *
 * @param[in] address  chip address of the byte
 * @return read-only pointer to that byte in the fake flash
 */
const uint8_t *UpdatePort::flash(uint32_t address)
{
    if (!inFlash(address, 1U)) {
        fake.bad_reads++;
        std::printf("fake_port: read of 0x%08x, outside the flash\n", address);
        return fake.flash.data();
    }
    return &fake.flash[address - kFlashStart];
}

/**
 * @brief Does nothing; the fake's ECC error flag is set by the test.
 */
void UpdatePort::eccClear()
{
}

/**
 * @brief Gives the ECC error flag the test has set.
 *
 * @return the fake's ecc_error flag
 */
bool UpdatePort::eccErrorSeen()
{
    return fake.ecc_error;
}

/**
 * @brief Counts a watchdog reload in fake.reloads.
 */
void UpdatePort::watchdogReload()
{
    fake.reloads++;
}

/**
 * @brief Stores a value and its complement in a backup pair; stops the test for a pair outside
 * the map.
 *
 * Here, not in fake_mode_port.cpp, since decision 6: the update receiver's fake reaches the
 * backup registers too (UpdatePort::clearRestartCount), and test_update links this file only.
 *
 * @param[in]     pair   pair number, below BACKUP_PAIRS
 * @param[in]     value  value to store
 */
void fakeBackupSet(uint32_t pair, uint32_t value)
{
    if (pair >= BACKUP_PAIRS) {
        std::printf("FAIL  backup pair %u outside the map\n", pair);
        std::exit(1);
    }
    fake.backup[2U * pair] = value;
    fake.backup[(2U * pair) + 1U] = ~value;
}

/**
 * @brief Reads a backup pair with the complement rule; stops the test for a pair outside the map.
 *
 * @param[in]     pair   pair number, below BACKUP_PAIRS
 * @param[out]    value  the stored value; 0 if the pair is broken
 * @return true if the two words are complements
 */
bool fakeBackupGet(uint32_t pair, uint32_t &value)
{
    if (pair >= BACKUP_PAIRS) {
        std::printf("FAIL  backup pair %u outside the map\n", pair);
        std::exit(1);
    }
    if (fake.backup[2U * pair] != ~fake.backup[(2U * pair) + 1U]) {
        value = 0U;
        return false;
    }
    value = fake.backup[2U * pair];
    return true;
}

/**
 * @brief Host stand-in for UpdatePort::clearRestartCount: clears the RESTARTS pair if it belongs
 * to slot_code, with the rule of ModePort::clearRestartCount, to which the board's
 * update_port.cpp forwards (decision 6).
 *
 * @param[in]     slot_code  the slot just written
 * @return true if nothing to clear or cleared; false if fake.backup_writable is false
 */
bool UpdatePort::clearRestartCount(uint8_t slot_code)
{
    uint32_t value = 0U;

    if (!fakeBackupGet(BACKUP_PAIR_RESTARTS, value) ||
        (((value >> BACKUP_RESTARTS_SLOT_SHIFT) & BACKUP_BYTE_MASK) != slot_code)) {
        return true;
    }
    if (!fake.backup_writable) {
        return false;
    }
    fakeBackupSet(BACKUP_PAIR_RESTARTS, 0U);
    return true;
}
