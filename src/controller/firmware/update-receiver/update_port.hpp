/*
 * update_port.hpp — everything the update receiver needs from the chip, in one list.
 *
 * On the board: update_port.cpp, which passes each call to common/flash.c, common/boot_state.c
 * or the watchdog. In the host unit tests: test/controller/unit/fake_port.cpp, which keeps a
 * RAM copy of the 128 KB flash and records every erase and write. So update_receiver.cpp holds
 * no register access and compiles on both.
 *
 * C++: a struct of static functions, and the build picks the body, as in the C version: the
 * firmware Makefile links update_port.cpp, the unit-test Makefile links fake_port.cpp.
 *   - Not an abstract class with virtual functions: there is one flash per chip and one port
 *     per program, so nothing is chosen while the program runs. Virtual functions would add a
 *     table of function pointers and an indirect call to every flash operation, for nothing.
 *   - Not a template parameter (UpdateReceiver<Port>): that would move the whole receiver
 *     into its header. This way update_receiver.cpp stays the one file the C version had.
 * A struct and not a namespace, so the receiver names one type for "the chip"; it is never
 * an object (constructor deleted).
 */
#ifndef UPDATE_PORT_HPP
#define UPDATE_PORT_HPP

#include <cstdint>

#include "boot_state.h"

struct UpdatePort {
    UpdatePort() = delete;

    /* The newest valid boot state (boot_state_read). False if neither copy is valid. */
    [[nodiscard]] static bool bootState(boot_state_t &state);

    /* Erase the 2 KB page at page_start (flash_erase_page). True if the chip reported no
     * error. */
    [[nodiscard]] static bool erasePage(uint32_t page_start);

    /* Program pairs of 32-bit words, one 64-bit flash word per pair, at an 8-byte aligned
     * erased address (flash_program). True if the chip reported no error. */
    [[nodiscard]] static bool program(uint32_t start, const uint32_t *words, uint32_t pairs);

    /* Where the program reads the flash byte at chip address `address`. */
    static const uint8_t *flash(uint32_t address);

    /* Forget earlier flash read errors / true if a read since then hit a damaged word (ECC). */
    static void eccClear();
    [[nodiscard]] static bool eccErrorSeen();

    /* Reload the independent watchdog. */
    static void watchdogReload();
};

#endif /* UPDATE_PORT_HPP */
