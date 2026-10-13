/*
 * boot_slot.hpp — the slot codes of the boot state (common/boot_state.h) as a C++ type.
 *
 * C++: enum class instead of the #define codes BOOT_SLOT_NONE, _A, _B. A Slot does not mix
 * with plain numbers or with the other codes (frame types, answer codes) by mistake: the
 * compiler refuses `slot == 1`. common/ stays C, shared with the bootloader, so the stored
 * boot state still holds uint8_t codes; toSlot() converts where the firmware reads one.
 *
 * Switches over a Slot have no default label: every enumerator has its own case that leaves the
 * function, and the line after the switch, reached only by a corrupted value, calls the fault
 * reaction (firmware/fault.hpp, which states the MISRA C:2012 Rule 16.4 deviation).
 *
 * No hardware here: compiles for the board and for the unit tests (test/controller/unit).
 */
#ifndef BOOT_SLOT_HPP
#define BOOT_SLOT_HPP

#include <cstdint>

#include "boot_state.h"
#include "fault.hpp"

/* Enumerator names are kName, not NAME: the vendor headers own the UPPER_CASE names as macros
 * (FLASH, CRC and RCC are macros in stm32g4xx.h), and a macro would replace an enumerator of
 * the same name. */
enum class Slot : uint8_t {
    kNone = BOOT_SLOT_NONE,
    kA = BOOT_SLOT_A,
    kB = BOOT_SLOT_B,
};

/**
 * @brief Returns a stored slot code as a Slot; any other code is the fault reaction.
 *
 * Every caller passes a code from a boot state that passed its checks (common/boot_state.c,
 * fields_valid: confirmed A or B, trial none or the other slot), and reads it only after the read
 * reported a valid state. So a code that is none of the three is a corrupted value in memory
 * (owner's decision, 13 Oct 2026). Before, it converted any byte, and a damaged code became a
 * Slot value outside the enum that the callers then compared as data.
 *
 * @param[in]     code  slot code as stored in the boot state (common/boot_state.h)
 * @return the Slot with that code
 * @note Does not return for a code that is not BOOT_SLOT_NONE, BOOT_SLOT_A or BOOT_SLOT_B.
 */
inline Slot toSlot(uint8_t code)
{
    switch (code) {
    case BOOT_SLOT_NONE:
        return Slot::kNone;
    case BOOT_SLOT_A:
        return Slot::kA;
    case BOOT_SLOT_B:
        return Slot::kB;
    }
    faultImpossibleValue("toSlot");   /* only a corrupted value reaches here */
}

/**
 * @brief Returns the other slot of the pair: B for A, A for B; Slot::kNone and any other value
 * are the fault reaction.
 *
 * Every caller asks only about a running slot it has found to be A or B: ModeManager::giveUp runs
 * only in TRIAL, which start() sets only for a running slot A or B; ModeManager::runSwitch and
 * UpdateReceiver::chooseTarget check for A or B first. So kNone here is an impossible value as
 * much as a corrupted one is (owner's decision, 13 Oct 2026). Before, both gave Slot::kNone, a
 * normal-looking answer, and before that slot A.
 *
 * @param[in]     slot  Slot::kA or Slot::kB
 * @return the other slot
 * @note Does not return for Slot::kNone or a value outside the enum.
 */
inline Slot otherSlot(Slot slot)
{
    switch (slot) {
    case Slot::kA:
        return Slot::kB;
    case Slot::kB:
        return Slot::kA;
    case Slot::kNone:
        faultImpossibleValue("otherSlot: no slot");   /* no caller asks this: see above */
    }
    faultImpossibleValue("otherSlot");   /* only a corrupted value reaches here */
}

#endif /* BOOT_SLOT_HPP */
