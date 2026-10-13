/*
 * fake_mode_port.cpp — the mode manager's chip calls (src/controller/firmware/mode-manager/
 * mode_port.hpp) on the host, on the same RAM flash and boot state as fake_port.cpp.
 *
 * writeBootState refuses what the real boot_state_write refuses (common/boot_state.c,
 * fields_valid: confirmed A or B; trial none or the slot not confirmed), and fails on request.
 * restart() records when it was called and returns, so a test can look at what happened before
 * it; on the board it does not return, so a test does not poll again after a restart.
 */
#include <cstdio>

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
