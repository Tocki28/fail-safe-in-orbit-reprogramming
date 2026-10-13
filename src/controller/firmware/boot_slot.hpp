/*
 * boot_slot.hpp — the slot codes of the boot state (common/boot_state.h) as a C++ type.
 *
 * C++: enum class instead of the #define codes BOOT_SLOT_NONE, _A, _B. A Slot does not mix
 * with plain numbers or with the other codes (frame types, answer codes) by mistake: the
 * compiler refuses `slot == 1`. common/ stays C, shared with the bootloader, so the stored
 * boot state still holds uint8_t codes; toSlot() converts where the firmware reads one.
 *
 * No hardware here: compiles for the board and for the unit tests (test/controller/unit).
 */
#ifndef BOOT_SLOT_HPP
#define BOOT_SLOT_HPP

#include <cstdint>

#include "boot_state.h"

/* Enumerator names are kName, not NAME: the vendor headers own the UPPER_CASE names as macros
 * (FLASH, CRC and RCC are macros in stm32g4xx.h), and a macro would replace an enumerator of
 * the same name. */
enum class Slot : uint8_t {
    kNone = BOOT_SLOT_NONE,
    kA = BOOT_SLOT_A,
    kB = BOOT_SLOT_B,
};

/**
 * @brief Returns a stored slot code as a Slot.
 *
 * The underlying type is fixed (uint8_t), so every byte converts: a damaged code stays a value
 * that equals none of the three enumerators, as the byte did in the C version. Switches over a
 * Slot meet such a value in their default case (otherSlot below).
 *
 * @param[in]     code  slot code as stored in the boot state (common/boot_state.h)
 * @return the Slot with that code; a value outside the three enumerators for any other byte
 */
constexpr Slot toSlot(uint8_t code)
{
    return static_cast<Slot>(code);
}

/**
 * @brief Returns the other slot of the pair: B for A, A for B, Slot::kNone for any other value.
 *
 * Before (mode-manager/mode_manager.cpp, a private helper), every value that was not A answered
 * A, so kNone or a damaged code named a real slot. Moved here so the unit tests can reach it.
 * Every Slot enumerator has its own case (-Wswitch-enum, common/build.mk); the default is only
 * for a value outside the enum (a corrupted variable; MISRA C:2012 Rule 16.4). Callers
 * (ModeManager::giveUp, runSwitch) store stateCode() of the result in the boot state, where
 * kNone as the confirmed slot is refused by the boot-state write (common/boot_state.c,
 * fields_valid).
 *
 * @param[in]     slot  Slot::kA or Slot::kB
 * @return the other slot; Slot::kNone if slot is not A or B
 */
constexpr Slot otherSlot(Slot slot)
{
    switch (slot) {
    case Slot::kA:
        return Slot::kB;
    case Slot::kB:
        return Slot::kA;
    case Slot::kNone:
        return Slot::kNone;
    default:
        return Slot::kNone;   /* outside the enum only: a corrupted value has no other slot */
    }
}

#endif /* BOOT_SLOT_HPP */
