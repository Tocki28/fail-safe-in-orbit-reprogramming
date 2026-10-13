/*
 * fault.c — the bootloader's fault reaction (common/fault.h): a value in memory that no correct
 * run can produce. Owner's decision, 13 Oct 2026: such a value is a fault, never data.
 *
 * In its own file, not in main.c, so the host tests that include main.c can put a stand-in in its
 * place (test/controller/unit/boot_stubs).
 */
#include "fault.h"

#include "stm32g4xx.h"
#include "uart.h"

/**
 * @brief Logs "BL: corrupted value in <where>; restarting", waits (bounded) for the line to leave
 * the UART, then resets the chip.
 *
 * Why a reset: the restart reads the boot state and every image again from flash, where each
 * value is checked again (common/boot_state.c, common/image_check.c); a value that only went bad
 * in RAM is then gone. Why not count it: decision B counts the restarts of a firmware started as
 * the confirmed slot; the bootloader is not one, and the record of its last start is cleared at
 * every start (bootloader/main.c, count_restart), so a fault here is never charged to a slot.
 * Bounded: uart_wait_sent gives up after its time-out (bootloader/uart.c).
 *
 * The reset: CMSIS NVIC_SystemReset (core_cm4.h) waits for outstanding memory writes (DSB), writes
 * AIRCR with the key 0x5FA and SYSRESETREQ set, and loops until the reset comes. SYSRESETREQ asks
 * for a system reset [Arm DUI 0553A, Sec. 4.3.5, Table 4-17, p. 4-17]; on this chip that is the
 * software reset [RM0440 Rev 9, Sec. 7.1.2, p. 271], reported by SFTRSTF in RCC_CSR
 * [RM0440 Rev 9, Sec. 7.4.28, p. 327].
 *
 * @param[in]     where  the function that found the impossible value
 * @note Does not return.
 */
void fault_impossible_value(const char *where)
{
    uart_put("BL: corrupted value in ");
    uart_put(where);
    uart_put("; restarting\r\n");
    uart_wait_sent();
    NVIC_SystemReset();
}
