/*
 * fault.c — the bootloader's fault reaction (common/fault.h): a value in memory that no correct
 * run can produce. Owner's decision, 13 Oct 2026: such a value is a fault, never data.
 *
 * In its own file, not in main.c, so the host tests that include main.c can put a stand-in in its
 * place (test/controller/unit/boot_stubs).
 *
 * Owner's decision 7, 13 Oct 2026, accepted as is: a fault loop inside the bootloader is not
 * counted. Every fault here ends in a software reset, and decision B counts only restarts of a
 * firmware started as the confirmed slot (bootloader/main.c, count_restart), which the bootloader
 * is not. If the impossible value comes back at every start, the chip restarts in a loop; the
 * onboard computer's restore over SWD ends it (Architecture Sec. 5.1).
 */
#include "fault.h"

#include "backup.h"
#include "stm32g4xx.h"
#include "uart.h"

/**
 * @brief Leaves the fault-location code in the backup registers, logs "BL: corrupted value in
 * <where> (fault location <code>); restarting", waits (bounded) for the line to leave the UART,
 * then resets the chip.
 *
 * Why the code in the backup registers (decision 3, owner, 13 Oct 2026): the bootloader has no
 * link to the ground, so the firmware that runs next reads the code and sends it once as the
 * detail of EVENT 0x0005 (firmware/mode-manager/mode_manager.cpp). It is left first, before the
 * log line, so a UART that never empties cannot keep it from being stored. Before, only the log
 * line said where.
 * Why a reset: the restart reads the boot state and every image again from flash, where each
 * value is checked again (common/boot_state.c, common/image_check.c); a value that only went bad
 * in RAM is then gone. Why not count it: decision B counts the restarts of a firmware started as
 * the confirmed slot; the bootloader is not one, and the record of its last start is cleared at
 * every start (bootloader/main.c, count_restart), so a fault here is never charged to a slot
 * (decision 7, see the file comment).
 * Bounded: uart_wait_sent gives up after its time-out (bootloader/uart.c).
 *
 * The reset: CMSIS NVIC_SystemReset (core_cm4.h) waits for outstanding memory writes (DSB), writes
 * AIRCR with the key 0x5FA and SYSRESETREQ set, and loops until the reset comes. SYSRESETREQ asks
 * for a system reset [Arm DUI 0553A, Sec. 4.3.5, Table 4-17, p. 4-17]; on this chip that is the
 * software reset [RM0440 Rev 9, Sec. 7.1.2, p. 271], reported by SFTRSTF in RCC_CSR
 * [RM0440 Rev 9, Sec. 7.4.28, p. 327]. The backup registers keep their values through it
 * [RM0440 Rev 9, Sec. 38.6.8, p. 1623].
 *
 * @param[in]     location  the fault-location code of the function (common/fault_codes.h)
 * @param[in]     where     the function that found the impossible value
 * @note Does not return.
 */
void fault_impossible_value(uint8_t location, const char *where)
{
    backup_leave_fault_location(location);
    uart_put("BL: corrupted value in ");
    uart_put(where);
    uart_put(" (fault location ");
    uart_put_hex32(location);
    uart_put("); restarting\r\n");
    uart_wait_sent();
    NVIC_SystemReset();
}
