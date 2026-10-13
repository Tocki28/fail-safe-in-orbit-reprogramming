/*
 * handover.h — start a program that passed its checks.
 *
 * What the program finds: the independent watchdog running with a time-out of about 2 s (the
 * bootloader started it with a long time-out and sets this one just before the jump; the
 * program must reload it), VTOR pointing at its vector table, its own stack pointer loaded,
 * and the rest as after a reset: USART2 and GPIOA in reset state, no interrupt enabled in the
 * NVIC. The clock is HSI16 in the normal case. If the switch back to HSI16 did not finish,
 * clock_back_to_reset() returns early and the program starts on the PLL (clock.c); the firmware
 * sets its own clock again and copes with either. One more difference: the RCC_CSR reset-cause
 * flags are already cleared (the bootloader logs them).
 */
#ifndef HANDOVER_H
#define HANDOVER_H

#include <stdint.h>

/* vector_table: address of the program's vector table (region start + IMAGE_CODE_OFFSET). */
void handover_start(uint32_t vector_table) __attribute__((noreturn));

#endif /* HANDOVER_H */
