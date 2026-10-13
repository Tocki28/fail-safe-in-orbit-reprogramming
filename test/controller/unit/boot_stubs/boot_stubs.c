/*
 * boot_stubs.c — host stand-ins for what the bootloader's main.c calls on the chip: the UART,
 * the clock, the hand-over to the firmware and the flash (unit/test_boot.c uses them).
 *
 * The UART stand-in collects the text in a buffer so a test can look at the log. The flash
 * stand-ins report no error and do nothing: the tests call no function that reads or writes
 * flash. The watchdog stand-ins only record that they were called (ev_trace); the real
 * watchdog.c is tested apart (test_watchdog.c). Every stand-in that matters to the order of the
 * bootloader's steps adds one character to ev_trace.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>

#include "boot_stubs.h"
#include "clock.h"
#include "flash.h"
#include "handover.h"
#include "uart.h"
#include "watchdog.h"

RCC_TypeDef fake_rcc;
char uart_log[UART_LOG_BYTES];
char ev_trace[EV_MAX];
bool stub_cycles_counts = true;
jmp_buf park_exit;

static uint32_t ev_length;
static bool uart_is_off;
static uint32_t reloads_after_off;

/**
 * @brief Empties the event trace and the park-loop counter.
 */
void ev_clear(void)
{
    ev_trace[0] = '\0';
    ev_length = 0U;
    uart_is_off = false;
    reloads_after_off = 0U;
}

/**
 * @brief Appends one event character to the trace; an event that does not fit is dropped.
 *
 * @param[in]     event  character that names the event
 */
void ev_add(char event)
{
    if (ev_length < (EV_MAX - 1U)) {
        ev_trace[ev_length] = event;
        ev_length++;
        ev_trace[ev_length] = '\0';
    }
}

/**
 * @brief Records that the long watchdog time-out was started.
 */
void watchdog_start_long(void)
{
    ev_add('W');
}

/**
 * @brief Records a watchdog reload; the third one after uart_off() leaves the park loop.
 */
void watchdog_reload(void)
{
    ev_add('R');
    if (uart_is_off) {
        reloads_after_off++;
        if (reloads_after_off >= 3U) {
            longjmp(park_exit, 1);
        }
    }
}

/**
 * @brief Records that the firmware's watchdog time-out was set.
 */
void watchdog_set_firmware_timeout(void)
{
    ev_add('F');
}

/**
 * @brief Empties the collected UART text.
 */
void uart_log_clear(void)
{
    uart_log[0] = '\0';
}

/**
 * @brief Does nothing; the host has no UART to start.
 */
void uart_init(void)
{
    ev_add('U');
}

/**
 * @brief Appends text to the collected UART text; text that does not fit is cut.
 *
 * @param[in]     text  zero-terminated text
 */
void uart_put(const char *text)
{
    size_t used = strlen(uart_log);

    if (used < (UART_LOG_BYTES - 1U)) {
        (void)snprintf(&uart_log[used], UART_LOG_BYTES - used, "%s", text);
    }
}

/**
 * @brief Appends a number to the collected UART text.
 *
 * @param[in]     value  number to write in decimal
 */
void uart_put_decimal(uint32_t value)
{
    char digits[16];

    (void)snprintf(digits, sizeof digits, "%u", (unsigned)value);
    uart_put(digits);
}

/**
 * @brief Records that the UART was switched off; the host has no UART to switch off.
 */
void uart_off(void)
{
    ev_add('O');
    uart_is_off = true;
}

/**
 * @brief Records the start of the cycle counter; the host has none.
 *
 * @return stub_cycles_counts
 */
bool cycles_start(void)
{
    ev_add('C');
    return stub_cycles_counts;
}

/**
 * @brief Returns 0; the tests do not time anything.
 *
 * @return 0
 */
uint32_t cycles_now(void)
{
    return 0U;
}

/**
 * @brief Reports that the clock is at 170 MHz; the host has no clock to set.
 *
 * @return true
 */
bool clock_to_170mhz(void)
{
    ev_add('K');
    return true;
}

/**
 * @brief Returns the 170 MHz clock the bootloader expects after clock_to_170mhz.
 *
 * @return CORE_CLOCK_HZ
 */
uint32_t clock_hz(void)
{
    return CORE_CLOCK_HZ;
}

/**
 * @brief Does nothing; the host has no clock to put back.
 */
void clock_back_to_reset(void)
{
}

/**
 * @brief Stops the test: no test may start a firmware.
 *
 * @param[in]     vector_table  address of the firmware's vector table (not used)
 * @note Does not return.
 */
void handover_start(uint32_t vector_table)
{
    (void)vector_table;
    (void)fputs("FAIL  handover_start was called by a test\n", stdout);
    exit(1);
}

/**
 * @brief Reports a failed erase; no test erases flash.
 *
 * @param[in]     page_start  address of the page (not used)
 * @return false
 */
bool flash_erase_page(uint32_t page_start)
{
    (void)page_start;
    return false;
}

/**
 * @brief Reports a failed programming; no test writes flash.
 *
 * @param[in]     start  address of the first word (not used)
 * @param[in]     words  words to program (not used)
 * @param[in]     pairs  number of 64-bit words (not used)
 * @return false
 */
bool flash_program(uint32_t start, const uint32_t *words, uint32_t pairs)
{
    (void)start;
    (void)words;
    (void)pairs;
    return false;
}

/**
 * @brief Does nothing; the host flash has no error flag.
 */
void flash_ecc_clear(void)
{
}

/**
 * @brief Reports that no flash read error was seen.
 *
 * @return false
 */
bool flash_ecc_error_seen(void)
{
    return false;
}
