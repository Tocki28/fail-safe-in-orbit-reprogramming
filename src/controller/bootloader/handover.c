/*
 * handover.c — firmware time-out on the watchdog, chip back to its reset state, jump.
 *
 * The watchdog itself is already running: main() starts it as its first action
 * (watchdog.c). This file only changes its time-out before the jump.
 */
#include "handover.h"

#include "clock.h"
#include "stm32g4xx.h"
#include "uart.h"
#include "watchdog.h"

/**
 * @brief Starts the program whose vector table is at vector_table.
 *
 * Sets the firmware's time-out of about 2 s on the already running watchdog and reloads it
 * (watchdog_set_firmware_timeout), turns the UART off, puts the clock back to its reset state,
 * sets VTOR, loads the program's stack pointer and jumps to its reset handler. The watchdog
 * cannot be stopped once running [RM0440 Rev 9, Sec. 35.3.1, p. 1557], so a program that hangs
 * is restarted (REQ-022) and the bootloader counts the failed start.
 *
 * @param[in]     vector_table  address of the program's vector table
 * @note Does not return.
 */
void handover_start(uint32_t vector_table)
{
    const volatile uint32_t *vectors = (const volatile uint32_t *)vector_table;
    /* Vector table: word 0 = initial stack pointer, word 1 = reset handler
     * [Arm DUI 0553A, Sec. 2.3.4, Figure 2-2, p. 2-24]. */
    uint32_t stack_top = vectors[0];
    uint32_t reset_handler = vectors[1];

    watchdog_set_firmware_timeout();
    uart_off();
    clock_back_to_reset();

    /* The program's exceptions use its own table from now on. VTOR takes the table address;
     * it must be aligned as layout.h explains [Arm DUI 0553A, Sec. 4.3.4, p. 4-16]. */
    SCB->VTOR = vector_table;
    __DSB();
    __ISB();
    /* Load the program's stack pointer and branch to its reset handler. In assembly, because
     * C code could still use the old stack between those two steps. */
    __asm volatile("msr msp, %0\n\tbx %1" : : "r"(stack_top), "r"(reset_handler) : "memory");
    for (;;) {   /* endless: not reached, the branch above does not come back; it marks noreturn */
    }
}
