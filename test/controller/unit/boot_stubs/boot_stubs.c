/*
 * boot_stubs.c — host stand-ins for what the bootloader's main.c calls on the chip: the UART, the
 * clock, the watchdog, the hand-over to the firmware, the flash, the TAMP backup registers and
 * the fault reaction (unit/test_boot.c and unit/test_flow.c use them).
 *
 * The UART stand-in collects the text in a buffer so a test can look at the log. The flash is a
 * RAM copy served by flash_at(); erase and program report a failure (no test writes flash through
 * them). The backup registers are a RAM array with the same pair rule as bootloader/backup.c
 * (tested on its own in test_backup.c). The watchdog stand-ins only record that they were called;
 * the real watchdog.c is tested apart (test_watchdog.c). The hand-over, the park loop and the
 * fault reaction leave the code under test by longjmp, so a test sees how a run ended. Every
 * stand-in that matters to the order of the bootloader's steps adds one character to ev_trace.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>

#include "backup.h"
#include "boot_stubs.h"
#include "clock.h"
#include "fault.h"
#include "flash.h"
#include "handover.h"
#include "uart.h"
#include "watchdog.h"

RCC_TypeDef fake_rcc;
char uart_log[UART_LOG_BYTES];
char ev_trace[EV_MAX];
bool stub_cycles_counts = true;
bool stub_watchdog_running = true;
uint32_t stub_watchdog_sr = 0U;
uint32_t stub_rcc_csr = 0x00000002U;   /* LSIRDY, bit 1 [RM0440 Rev 9, Sec. 7.4.28, p. 328] */
jmp_buf run_exit;
bool run_exit_armed;
volatile int run_how;
uint32_t started_vector;
char fault_where[64];
uint32_t fault_calls;
uint32_t stub_backup[BACKUP_REGISTERS];
bool stub_backup_writable = true;
bool stub_backup_open;
uint8_t stub_flash[FLASH_BYTES];
uint32_t stub_ecc_address;

static uint32_t ev_length;
static bool uart_is_off;
static uint32_t reloads_after_off;
static bool ecc_seen;

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
 * @brief Leaves the code under test by longjmp to run_exit, or stops the test program if no test
 * armed it.
 *
 * @param[in]     how   RUN_PARKED, RUN_STARTED or RUN_FAULT
 * @param[in]     what  text for the FAIL line when run_exit is not armed
 * @note Does not return.
 */
static _Noreturn void leave_run(int how, const char *what)
{
    if (run_exit_armed) {
        run_exit_armed = false;
        run_how = how;
        longjmp(run_exit, how);
    }
    printf("FAIL  %s, and no test expected it\n", what);
    exit(1);
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
            leave_run(RUN_PARKED, "the park loop was reached");
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
 * @brief Stand-in for the watchdog check of decision F(b).
 *
 * Hands back the register values a test chose, as the real function hands back what it read.
 *
 * @param[out]    iwdg_sr  set to stub_watchdog_sr
 * @param[out]    rcc_csr  set to stub_rcc_csr
 * @return stub_watchdog_running
 */
bool watchdog_running(uint32_t *iwdg_sr, uint32_t *rcc_csr)
{
    *iwdg_sr = stub_watchdog_sr;
    *rcc_csr = stub_rcc_csr;
    return stub_watchdog_running;
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
        (void)snprintf(&uart_log[used], UART_LOG_BYTES - used, "%s", text);   /* cut text is fine */
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

    (void)snprintf(digits, sizeof digits, "%u", (unsigned)value);   /* 10 digits always fit */
    uart_put(digits);
}

/**
 * @brief Appends a number to the collected UART text as "0x" and eight upper-case hex digits.
 *
 * @param[in]     value  number to write
 */
void uart_put_hex32(uint32_t value)
{
    char digits[16];

    (void)snprintf(digits, sizeof digits, "0x%08X", (unsigned)value);   /* 10 characters fit */
    uart_put(digits);
}

/**
 * @brief Does nothing; the host has no transmitter to wait for.
 */
void uart_wait_sent(void)
{
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
 * @brief Records the start of a firmware and leaves the run: no firmware runs on the host.
 *
 * @param[in]     vector_table  address of the firmware's vector table, kept in started_vector
 * @note Does not return.
 */
void handover_start(uint32_t vector_table)
{
    ev_add('H');
    started_vector = vector_table;
    leave_run(RUN_STARTED, "handover_start was called");
}

/**
 * @brief Stand-in for the fault reaction: records where, and leaves the run.
 *
 * @param[in]     where  the function that found the impossible value, kept in fault_where
 * @note Does not return.
 */
void fault_impossible_value(const char *where)
{
    ev_add('Z');
    fault_calls++;
    (void)snprintf(fault_where, sizeof fault_where, "%s", where);   /* a cut name still shows */
    leave_run(RUN_FAULT, where);
}

/**
 * @brief Clears the record of fault reactions.
 */
void fault_clear(void)
{
    fault_where[0] = '\0';
    fault_calls = 0U;
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
 * @brief Forgets an earlier ECC error.
 */
void flash_ecc_clear(void)
{
    ecc_seen = false;
}

/**
 * @brief Reports whether a read through flash_at() hit stub_ecc_address since flash_ecc_clear().
 *
 * @return true if it did
 */
bool flash_ecc_error_seen(void)
{
    return ecc_seen;
}

/**
 * @brief Serves a flash read from the RAM copy; a read of stub_ecc_address sets the ECC flag.
 *
 * @param[in]     address  chip address, inside the main flash
 * @return pointer into stub_flash
 * @note Stops the test program for an address outside the main flash.
 */
const uint8_t *flash_at(uint32_t address)
{
    if ((address < (uint32_t)FLASH_START) ||
        ((address - (uint32_t)FLASH_START) >= (uint32_t)FLASH_BYTES)) {
        printf("FAIL  flash_at(0x%08x): outside the main flash\n", (unsigned)address);
        exit(1);
    }
    if ((stub_ecc_address != 0U) && (address == stub_ecc_address)) {
        ecc_seen = true;
    }
    return &stub_flash[address - (uint32_t)FLASH_START];
}

/**
 * @brief Erases the RAM flash (every byte 0xFF) and forgets the ECC address.
 */
void stub_flash_reset(void)
{
    memset(stub_flash, 0xFF, sizeof stub_flash);
    stub_ecc_address = 0U;
    ecc_seen = false;
}

/**
 * @brief Records that the backup registers were opened.
 */
void backup_open(void)
{
    stub_backup_open = true;
}

/**
 * @brief Records that the backup registers were closed.
 */
void backup_close(void)
{
    stub_backup_open = false;
}

/**
 * @brief Reads a pair of the RAM backup registers, with the rule of bootloader/backup.c.
 *
 * @param[in]     pair   pair number
 * @param[out]    value  the stored value; 0 if the pair is broken
 * @return true if the two words are complements
 */
bool backup_read(uint32_t pair, uint32_t *value)
{
    return stub_backup_get(pair, value);
}

/**
 * @brief Writes a pair of the RAM backup registers; fails, changing nothing, when
 * stub_backup_writable is false.
 *
 * @param[in]     pair   pair number
 * @param[in]     value  value to store with its complement
 * @return stub_backup_writable
 */
bool backup_write(uint32_t pair, uint32_t value)
{
    if (!stub_backup_writable) {
        return false;
    }
    stub_backup_set(pair, value);
    return true;
}

/**
 * @brief Stores a value and its complement in a pair; stops the test for a pair outside the map.
 *
 * @param[in]     pair   pair number, below BACKUP_PAIRS
 * @param[in]     value  value to store
 */
void stub_backup_set(uint32_t pair, uint32_t value)
{
    if (pair >= BACKUP_PAIRS) {
        printf("FAIL  backup pair %u outside the map\n", (unsigned)pair);
        exit(1);
    }
    stub_backup[2U * pair] = value;
    stub_backup[(2U * pair) + 1U] = ~value;
}

/**
 * @brief Reads a pair with the complement rule; stops the test for a pair outside the map.
 *
 * @param[in]     pair   pair number, below BACKUP_PAIRS
 * @param[out]    value  the stored value; 0 if the pair is broken
 * @return true if the two words are complements
 */
bool stub_backup_get(uint32_t pair, uint32_t *value)
{
    if (pair >= BACKUP_PAIRS) {
        printf("FAIL  backup pair %u outside the map\n", (unsigned)pair);
        exit(1);
    }
    if (stub_backup[2U * pair] != ~stub_backup[(2U * pair) + 1U]) {
        *value = 0U;
        return false;
    }
    *value = stub_backup[2U * pair];
    return true;
}

/**
 * @brief Sets every backup register to 0, as after an RTC domain reset: every pair broken, empty.
 */
void stub_backup_reset(void)
{
    memset(stub_backup, 0, sizeof stub_backup);
    stub_backup_writable = true;
    stub_backup_open = false;
}
