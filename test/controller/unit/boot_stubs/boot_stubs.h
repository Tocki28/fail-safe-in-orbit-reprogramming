/*
 * boot_stubs.h — what the bootloader unit test shares with its host stand-ins (boot_stubs.c).
 */
#ifndef BOOT_STUBS_H
#define BOOT_STUBS_H

#include "stm32g4xx.h"

/* Room for the UART text of one test. */
#define UART_LOG_BYTES 512U

/* Everything the bootloader wrote to the UART since uart_log_clear(). */
extern char uart_log[UART_LOG_BYTES];

void uart_log_clear(void);

#endif /* BOOT_STUBS_H */
