/*
 * fault.hpp — the main firmware's fault reaction: a value in memory that no correct run can
 * produce (owner's decision, 13 Oct 2026: such a value is a fault, never data; REQ-041: detect a
 * change in its own memory and return to a known safe state).
 *
 * On the board: main.cpp defines faultImpossibleValue (it owns the frame sender the EVENT goes
 * out through) and the C function fault_impossible_value of common/fault.h, which the shared C
 * code calls and which forwards here. In the host unit tests: test/controller/unit/fake_fault.cpp
 * records the call and leaves by longjmp.
 *
 * The reaction on the board: EVENT 0x0005 "change in memory detected" (Interfaces Sec. 5.4) with
 * no detail bytes, because the Interfaces document defines none for 0x0005; a bounded wait for the
 * UART; then a software restart marked in the backup registers, which the bootloader counts like
 * a watchdog restart of the confirmed slot (decision B), so a loop of such restarts ends in the
 * last-resort firmware.
 *
 * Switches over an enum in the firmware have no default label. No default: the compiler checks
 * every case (-Wswitch, -Werror); only a corrupted value reaches the line after the switch, which
 * calls faultImpossibleValue. This deviates on purpose from MISRA C:2012 Rule 16.4; its purpose,
 * handling a value outside the cases, is met by the fault reaction. Each file with such a switch
 * says so once.
 */
#ifndef FIRMWARE_FAULT_HPP
#define FIRMWARE_FAULT_HPP

#include "fault.h"

/**
 * @brief The fault reaction: EVENT 0x0005, bounded wait, counted restart.
 *
 * @param[in]     where  the function that found the impossible value; kept for a debugger, not
 *                       sent (no detail is defined for EVENT 0x0005)
 * @note Does not return.
 */
[[noreturn]] void faultImpossibleValue(const char *where);

#endif /* FIRMWARE_FAULT_HPP */
