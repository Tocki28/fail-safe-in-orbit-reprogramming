/*
 * fault.h — the one fault reaction of every controller program: a value in memory that no
 * correct run can produce.
 *
 * Owner's decision, 13 Oct 2026: a value that cannot exist is a fault, never data. A switch over
 * an enum has no default label: every enumerator has its own case that leaves the function, and
 * the line after the switch, which only a corrupted value reaches, calls this function. An
 * if-chain whose last branch only a corrupted value reaches calls it too.
 *
 * Declared here, defined once per program:
 *   bootloader      bootloader/fault.c: log line, bounded wait for the UART, software reset;
 *   main firmware   firmware/main.cpp: EVENT 0x0005, bounded wait, restart counted by the
 *                   bootloader (decision B);
 *   test firmware   test/controller/test-firmware/main.c (it links common/boot_state.c);
 *   host tests      test/controller/unit: records the call and leaves by longjmp;
 *   host tool       common/image_check.c holds a host-only fallback (see there).
 * The shared code in this directory calls it for its own impossible values (image_check.c,
 * boot_state.c), so each program gets the reaction it defines.
 *
 * What is not a fault: bytes received from a link are input, refused or dropped by their own
 * checks; values read from flash (boot state, image headers) are judged by their own checks
 * and by decision D; a TAMP backup-register pair that does not match its complement reads as
 * empty (common/backup_layout.h). None of these reach this function.
 */
#ifndef FAULT_H
#define FAULT_H

/* C linkage for the C++ firmware (boot_state.h says why). [[noreturn]] in C++, _Noreturn in C11:
 * the same promise to the compiler, that a call never comes back. */
#ifdef __cplusplus
extern "C" {
#define FAULT_NORETURN [[noreturn]]
#else
#define FAULT_NORETURN _Noreturn
#endif

/* The fault reaction. `where` names the function that found the impossible value: the
 * bootloader logs it; the firmware keeps it for a debugger (no detail byte is defined for EVENT
 * 0x0005 in Interfaces Sec. 5.4). Never returns. */
FAULT_NORETURN void fault_impossible_value(const char *where);

#ifdef __cplusplus
}
#endif

#endif /* FAULT_H */
