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

/* A stored code as a Slot. The underlying type is fixed (uint8_t), so every byte converts: a
 * damaged code stays a value that equals none of the three, as the byte did in the C version. */
constexpr Slot toSlot(uint8_t code)
{
    return static_cast<Slot>(code);
}

#endif /* BOOT_SLOT_HPP */
