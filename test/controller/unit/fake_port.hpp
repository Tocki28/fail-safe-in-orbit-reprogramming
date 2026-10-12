/*
 * fake_port.hpp — the update receiver's chip calls (src/controller/firmware/update-receiver/
 * update_port.hpp) on the host: a RAM copy of the controller's 128 KB flash that follows the
 * chip's erase and program rules, a boot state the test sets, and a record of every erase and
 * write, so a test can show that nothing outside the target slot was touched.
 *
 * C++: fake_port.cpp defines UpdatePort's static functions; the unit-test Makefile links it in
 * place of the board's update_port.cpp.
 */
#ifndef FAKE_PORT_HPP
#define FAKE_PORT_HPP

#include <array>
#include <cstdint>

#include "boot_slot.hpp"
#include "boot_state.h"
#include "layout.h"

/* layout.h's values as uint32_t, for the tests (layout.h has no U suffixes; see there). */
constexpr uint32_t kFlashStart = FLASH_START;
constexpr uint32_t kFlashPageBytes = FLASH_PAGE_BYTES;
constexpr uint32_t kSlotAStart = SLOT_A_START;
constexpr uint32_t kSlotBStart = SLOT_B_START;
constexpr uint32_t kSlotBytes = SLOT_BYTES;

/* FLASH_START .. FLASH_START + 128 KB: up to the end of the boot state (layout.h). */
constexpr uint32_t kFakeFlashBytes = BOOT_STATE_START + (2 * BOOT_STATE_COPY_BYTES) - FLASH_START;

struct Fake {
    std::array<uint8_t, kFakeFlashBytes> flash;   /* index = chip address - FLASH_START */
    bool state_valid;                             /* what UpdatePort::bootState() returns */
    boot_state_t state;
    Slot allowed_slot;                            /* the slot the test expects to be written */
    bool fail_erase;                              /* the next erases report a flash error */
    bool fail_program;                            /* the next writes report a flash error */
    bool ecc_error;                               /* reads report a damaged flash word (ECC) */
    uint32_t erases;                              /* pages erased */
    uint32_t programs;                            /* UpdatePort::program() calls that wrote */
    uint32_t outside;                             /* erases or writes outside allowed_slot: stays 0 */
    uint32_t bad_reads;                           /* UpdatePort::flash() outside the fake flash */
    uint32_t reloads;                             /* watchdog reloads */
};

extern Fake fake;

/* Fill every region with its own byte pattern, so a stray write shows. */
void fakeReset();
/* Number of bytes in [start, start + bytes) that differ from `value`. */
uint32_t fakeCountNot(uint32_t start, uint32_t bytes, uint8_t value);

#endif /* FAKE_PORT_HPP */
