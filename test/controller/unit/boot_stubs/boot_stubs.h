/*
 * boot_stubs.h — what the bootloader unit test shares with its host stand-ins (boot_stubs.c).
 */
#ifndef BOOT_STUBS_H
#define BOOT_STUBS_H

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>

#include "stm32g4xx.h"

/* Room for the UART text of one test. */
#define UART_LOG_BYTES 1024U

/* Everything the bootloader wrote to the UART since uart_log_clear(). */
extern char uart_log[UART_LOG_BYTES];

void uart_log_clear(void);

/* What the bootloader did, in order, one character per event, since ev_clear():
 *   W watchdog_start_long   R watchdog_reload   F watchdog_set_firmware_timeout
 *   C cycles_start          K clock_to_170mhz   U uart_init   O uart_off
 * and, added by test_flow.c's own stand-ins, E boot_state_write and X image_check_contents. */
#define EV_MAX 96U
extern char ev_trace[EV_MAX];

void ev_clear(void);
void ev_add(char event);

/* cycles_start() answers this: false stands for a cycle counter that does not count. */
extern bool stub_cycles_counts;

/* After uart_off(), the third watchdog_reload() jumps to park_exit, so a test can leave the
 * bootloader's endless park loop. The test sets park_exit with setjmp before the call. */
extern jmp_buf park_exit;

#endif /* BOOT_STUBS_H */
