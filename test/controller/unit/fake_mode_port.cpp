/*
 * fake_mode_port.cpp — the mode manager's chip calls (src/controller/firmware/mode-manager/
 * mode_port.hpp) on the host, on the same RAM flash and boot state as fake_port.cpp.
 *
 * writeBootState refuses what the real boot_state_write refuses (common/boot_state.c,
 * fields_valid: confirmed A or B; trial none or the slot not confirmed), and fails on request.
 * restart() records when it was called and returns, so a test can look at what happened before
 * it; on the board it does not return, so a test does not poll again after a restart. The backup
 * registers are fake.backup, with the pair rule of mode_port.cpp: a pair counts only when its
 * second word is the complement of the first.
 */
#include <cstdio>
#include <cstdlib>

#include "fake_port.hpp"
#include "fake_uart_link.hpp"
#include "mode_port.hpp"

namespace {

/**
 * @brief Returns true if the record names allowed slots (common/boot_state.c, fields_valid).
 *
 * @param[in]     state  the record
 * @return true if confirmed is A or B and trial is none or the other slot
 */
bool fieldsValid(const boot_state_t &state)
{
    bool confirmed_ok = (state.confirmed == BOOT_SLOT_A) || (state.confirmed == BOOT_SLOT_B);
    uint8_t other = (state.confirmed == BOOT_SLOT_A) ? uint8_t{BOOT_SLOT_B} : uint8_t{BOOT_SLOT_A};
    return confirmed_ok && ((state.trial == BOOT_SLOT_NONE) || (state.trial == other));
}

}  // namespace

/**
 * @brief Host stand-in for ModePort::readBootState: the boot state the test set.
 *
 * @param[out]    state  fake.state
 * @return fake.state_valid
 */
bool ModePort::readBootState(boot_state_t &state)
{
    state = fake.state;
    return fake.state_valid;
}

/**
 * @brief Host stand-in for ModePort::writeBootState: stores the record in fake.state.
 *
 * @param[in,out] state  the record; its sequence is set one higher than the stored one
 * @return false if fake.fail_state_write is set or the fields are not valid; true otherwise
 */
bool ModePort::writeBootState(boot_state_t &state)
{
    fake.state_writes++;
    fake.sent_at_state_write = fake_link.sent_bytes;
    if (fake.fail_state_write || !fieldsValid(state)) {
        return false;
    }
    state.sequence = fake.state.sequence + 1U;
    fake.state = state;
    fake.state_valid = true;
    return true;
}

/**
 * @brief Host stand-in for ModePort::flash: the byte's place in the fake flash.
 *
 * @param[in]     address  chip address
 * @return pointer into fake.flash; its first byte (and a count in fake.bad_reads) if the address
 *         is outside the flash
 */
const uint8_t *ModePort::flash(uint32_t address)
{
    if ((address < kFlashStart) || ((address - kFlashStart) >= kFakeFlashBytes)) {
        fake.bad_reads++;
        std::printf("fake_mode_port: read of 0x%08x, outside the flash\n", address);
        return fake.flash.data();
    }
    return &fake.flash[address - kFlashStart];
}

/**
 * @brief Host stand-in for ModePort::eccClear: nothing to clear.
 */
void ModePort::eccClear()
{
}

/**
 * @brief Host stand-in for ModePort::eccErrorSeen.
 *
 * @return fake.ecc_error
 */
bool ModePort::eccErrorSeen()
{
    return fake.ecc_error;
}

/**
 * @brief Host stand-in for ModePort::restart: records the call, the drains and the bytes sent
 * before it, and returns.
 */
void ModePort::restart()
{
    fake.restarts++;
    fake.drains_at_restart = fake_link.drains;
    fake.sent_at_restart = fake_link.sent_bytes;
}

/* fakeBackupSet and fakeBackupGet are in fake_port.cpp since decision 6. */

namespace {

/**
 * @brief Writes a backup pair unless the test made the backup registers unwritable.
 *
 * @param[in]     pair   pair number
 * @param[in]     value  value to store
 * @return fake.backup_writable
 */
bool writePair(uint32_t pair, uint32_t value)
{
    if (!fake.backup_writable) {
        return false;
    }
    fakeBackupSet(pair, value);
    return true;
}

}  // namespace

/**
 * @brief Host stand-in for ModePort::leaveGiveUpRequest: the GIVE_UP pair.
 *
 * @param[in]     slot_code  the trial slot given up
 * @return fake.backup_writable
 */
bool ModePort::leaveGiveUpRequest(uint8_t slot_code)
{
    return writePair(BACKUP_PAIR_GIVE_UP, slot_code);
}

/**
 * @brief Host stand-in for ModePort::readFallbackRecord: the FALLBACK pair, split into one entry
 * per copy and the copy started, as mode_port.cpp splits it (common/backup_layout.h).
 *
 * @param[out]    record  each copy's check result, and the copy started
 * @return true if the pair matches and is not 0
 */
bool ModePort::readFallbackRecord(FallbackRecord &record)
{
    uint32_t value = 0U;
    bool stored = fakeBackupGet(BACKUP_PAIR_FALLBACK, value) && (value != 0U);

    record.copies[0] = {BACKUP_COPY_A, static_cast<uint8_t>((value >> BACKUP_FALLBACK_A_SHIFT) &
                                                            BACKUP_BYTE_MASK)};
    record.copies[1] = {BACKUP_COPY_B, static_cast<uint8_t>((value >> BACKUP_FALLBACK_B_SHIFT) &
                                                            BACKUP_BYTE_MASK)};
    record.copies[2] = {BACKUP_COPY_LAST_RESORT,
                        static_cast<uint8_t>((value >> BACKUP_FALLBACK_LAST_RESORT_SHIFT) &
                                             BACKUP_BYTE_MASK)};
    record.started = static_cast<uint8_t>((value >> BACKUP_FALLBACK_STARTED_SHIFT) &
                                          BACKUP_BYTE_MASK);
    return stored;
}

/**
 * @brief Host stand-in for ModePort::clearFallbackRecord.
 *
 * @return fake.backup_writable
 */
bool ModePort::clearFallbackRecord()
{
    return writePair(BACKUP_PAIR_FALLBACK, 0U);
}

/**
 * @brief Host stand-in for ModePort::readBootloaderFault: the FAULT_LOCATION pair (decision 3).
 *
 * @param[out]    location  the stored value; 0 when none is stored
 * @return true if the pair matches and is not 0
 */
bool ModePort::readBootloaderFault(uint32_t &location)
{
    return fakeBackupGet(BACKUP_PAIR_FAULT_LOCATION, location) && (location != 0U);
}

/**
 * @brief Host stand-in for ModePort::clearBootloaderFault.
 *
 * @return fake.backup_writable
 */
bool ModePort::clearBootloaderFault()
{
    return writePair(BACKUP_PAIR_FAULT_LOCATION, 0U);
}

/**
 * @brief Host stand-in for ModePort::clearRestartCount: clears the RESTARTS pair if it belongs to
 * slot_code, as mode_port.cpp does.
 *
 * @param[in]     slot_code  the running slot's code
 * @return true if nothing to clear or cleared; false if the write failed
 */
bool ModePort::clearRestartCount(uint8_t slot_code)
{
    uint32_t value = 0U;

    if (!fakeBackupGet(BACKUP_PAIR_RESTARTS, value) ||
        (((value >> BACKUP_RESTARTS_SLOT_SHIFT) & BACKUP_BYTE_MASK) != slot_code)) {
        return true;
    }
    return writePair(BACKUP_PAIR_RESTARTS, 0U);
}

/**
 * @brief Host stand-in for ModePort::restartCounted: stores the fault mark, records the restart,
 * the drains and the bytes sent before it, and returns.
 *
 * @param[in]     reason  BACKUP_FAULT_...
 */
void ModePort::restartCounted(uint8_t reason)
{
    /* As on the board: the restart follows whether or not the mark was stored. */
    (void)writePair(BACKUP_PAIR_FAULT, reason);
    fake.counted_restarts++;
    fake.fault_reason = reason;
    fake.drains_at_restart = fake_link.drains;
    fake.sent_at_restart = fake_link.sent_bytes;
}
