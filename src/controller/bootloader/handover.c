/*
 * handover.c — watchdog on, chip back to its reset state, jump.
 */
#include "handover.h"

#include "clock.h"
#include "stm32g4xx.h"
#include "uart.h"

/* IWDG key values [RM0440 Rev 9, Sec. 35.3.2, p. 1557]. */
#define IWDG_KEY_START     0xCCCCU
#define IWDG_KEY_UNLOCK    0x5555U
#define IWDG_KEY_RELOAD    0xAAAAU
/* Timeout = 4 x 2^PR x (RLR + 1) / f_LSI. PR = 3 divides the LSI by 32
 * [RM0440 Rev 9, Sec. 35.4.2, p. 1560], one tick per ms at the typical 32 kHz; 2000 ticks give
 * 2 s, 1.88-2.17 s over the LSI range 29.5-34 kHz [DS12589 Rev 6, Table 44, p. 105].
 * Bench value: REQ-022 asks for the restart but sets no time. */
#define IWDG_PR_DIV32      3U
#define IWDG_RELOAD_TICKS  1999U
/* The LSI clocks the new values in: it starts within 130 us [DS12589 Rev 6, Table 44, p. 105],
 * then each update takes up to five LSI cycles [RM0440 Rev 9, Sec. 35.4.4, p. 1562], 170 us at
 * the slowest 29.5 kHz. 2 ms of cycles at 170 MHz is about six times that (21 ms at 16 MHz). */
#define IWDG_UPDATE_CYCLES (2U * (CORE_CLOCK_HZ / 1000U))

/* Starts the independent watchdog with a time-out of about 2 s.
 * Waits for the register update to finish, at most IWDG_UPDATE_CYCLES, then reloads the counter.
 * Window option unused [RM0440 Rev 9, Sec. 35.3.2, p. 1557, steps 1-6]. Once started, the
 * IWDG cannot be stopped except by a reset (p. 1557), so a program that hangs is restarted
 * (REQ-022) and the bootloader counts the failed start. */
static void watchdog_start(void)
{
    IWDG->KR = IWDG_KEY_START;
    IWDG->KR = IWDG_KEY_UNLOCK;
    IWDG->PR = IWDG_PR_DIV32;
    IWDG->RLR = IWDG_RELOAD_TICKS;
    /* Bounded: if the update never ends the LSI is not running, and the watchdog with it;
     * waiting longer would only stop the start-up. */
    uint32_t start = cycles_now();
    while ((IWDG->SR != 0U) && ((cycles_now() - start) <= IWDG_UPDATE_CYCLES)) {
    }
    IWDG->KR = IWDG_KEY_RELOAD;
}

/* Starts the program whose vector table is at vector_table. Does not return.
 * Starts the watchdog, turns the UART off, puts the clock back to its reset state, sets VTOR,
 * loads the program's stack pointer and jumps to its reset handler. */
void handover_start(uint32_t vector_table)
{
    const volatile uint32_t *vectors = (const volatile uint32_t *)vector_table;
    /* Vector table: word 0 = initial stack pointer, word 1 = reset handler
     * [Arm DUI 0553A, Sec. 2.3.4, Figure 2-2, p. 2-24]. */
    uint32_t stack_top = vectors[0];
    uint32_t reset_handler = vectors[1];

    watchdog_start();
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
    for (;;) {
        /* Not reached. */
    }
}
