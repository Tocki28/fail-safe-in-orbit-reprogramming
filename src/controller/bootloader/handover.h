/*
 * handover.h — start a program that passed its checks.
 *
 * What the program finds: the independent watchdog running (~2 s, it must reload it), VTOR
 * pointing at its vector table, its own stack pointer loaded, and the rest as after a reset:
 * HSI16 clock, USART2 and GPIOA in reset state, no interrupt enabled in the NVIC. One
 * difference: the RCC_CSR reset-cause flags are already cleared (the bootloader logs them).
 */
#ifndef HANDOVER_H
#define HANDOVER_H

#include <stdint.h>

/* vector_table: address of the program's vector table (region start + IMAGE_CODE_OFFSET). */
void handover_start(uint32_t vector_table) __attribute__((noreturn));

#endif /* HANDOVER_H */
