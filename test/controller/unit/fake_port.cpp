/*
 * fake_port.cpp — see fake_port.hpp. Chip rules copied from RM0440 Rev 9:
 *   erase: a 2 KB page at a page boundary, to 0xFF (Sec. 5.3.6, p. 195);
 *   program: 8 bytes at an 8-byte aligned address, only over erased bytes (Sec. 5.3.7, p. 196).
 */
#include "fake_port.hpp"

#include <cstdio>
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

void fill(uint32_t start, uint32_t bytes, uint8_t value)
{
    std::memset(&fake.flash[start - kFlashStart], value, bytes);
}

/* True if [address, address + bytes) is inside the slot the test allows. */
bool inAllowedSlot(uint32_t address, uint32_t bytes)
{
    uint32_t start = (fake.allowed_slot == Slot::kA) ? kSlotAStart : kSlotBStart;

    return (address >= start) && (bytes <= kSlotBytes) && ((address - start) <= (kSlotBytes - bytes));
}

bool inFlash(uint32_t address, uint32_t bytes)
{
    return (address >= kFlashStart) && (bytes <= kFakeFlashBytes) &&
           ((address - kFlashStart) <= (kFakeFlashBytes - bytes));
}

}  // namespace

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
}

uint32_t fakeCountNot(uint32_t start, uint32_t bytes, uint8_t value)
{
    uint32_t count = 0U;

    for (uint32_t i = 0U; i < bytes; i++) {
        count += (fake.flash[start - kFlashStart + i] != value) ? 1U : 0U;
    }
    return count;
}

bool UpdatePort::bootState(boot_state_t &state)
{
    state = fake.state;
    return fake.state_valid;
}

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

const uint8_t *UpdatePort::flash(uint32_t address)
{
    if (!inFlash(address, 1U)) {
        fake.bad_reads++;
        std::printf("fake_port: read of 0x%08x, outside the flash\n", address);
        return fake.flash.data();
    }
    return &fake.flash[address - kFlashStart];
}

void UpdatePort::eccClear()
{
}

bool UpdatePort::eccErrorSeen()
{
    return fake.ecc_error;
}

void UpdatePort::watchdogReload()
{
    fake.reloads++;
}
