/*
 * boot_stubs.c — host stand-ins for what the bootloader's main.c calls on the chip: the UART,
 * the clock, the hand-over to the firmware and the flash (unit/test_boot.c uses them).
 *
 * The UART stand-in collects the text in a buffer so a test can look at the log. The flash
 * stand-ins report no error and do nothing: the tests call no function that reads or writes
 * flash (boot_state_read and boot_state_write are not called).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot_stubs.h"
#include "clock.h"
#include "flash.h"
#include "handover.h"
#include "uart.h"

RCC_TypeDef fake_rcc;
char uart_log[UART_LOG_BYTES];

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
 * @brief Does nothing; the host has no UART to switch off.
 */
void uart_off(void)
{
}

/**
 * @brief Does nothing; the host has no cycle counter to start.
 */
void cycles_start(void)
{
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
