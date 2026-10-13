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
 *   bootloader      bootloader/fault.c: the fault location left in the backup registers, a log
 *                   line, bounded wait for the UART, software reset;
 *   main firmware   firmware/main.cpp: EVENT 0x0005 with the fault location, bounded wait,
 *                   restart counted by the bootloader (decision B);
 *   test firmware   test/controller/test-firmware/main.c (it links common/boot_state.c);
 *   host tests      test/controller/unit: records the call and leaves by longjmp;
 *   host tool       tools/host_fault.c (outside src/): abort(), for tools/check_images.py.
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

#include <stdint.h>

/* Every caller passes one of these codes (FAULT_AT_...). */
#include "fault_codes.h"

/* C linkage for the C++ firmware (boot_state.h says why). [[noreturn]] in C++, _Noreturn in C11:
 * the same promise to the compiler, that a call never comes back. */
#ifdef __cplusplus
extern "C" {
#define FAULT_NORETURN [[noreturn]]
#else
#define FAULT_NORETURN _Noreturn
#endif

/* The fault reaction. `location` is the fault-location code of the function that found the
 * impossible value (common/fault_codes.h, decision 3, owner, 13 Oct 2026): the firmware sends it
 * as the 1-byte detail of EVENT 0x0005 (Interfaces Sec. 5.4); the bootloader logs it and leaves it
 * in the backup registers for the firmware that runs next. `where` names the same function: the
 * bootloader logs it; the firmware keeps it for a debugger. Before, only `where` was passed and
 * EVENT 0x0005 carried no detail. Never returns. */
FAULT_NORETURN void fault_impossible_value(uint8_t location, const char *where);

#ifdef __cplusplus
}
#endif

#endif /* FAULT_H */
