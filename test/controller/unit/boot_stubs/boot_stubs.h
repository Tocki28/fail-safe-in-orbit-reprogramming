/*
 * boot_stubs.h — what the bootloader unit tests share with their host stand-ins (boot_stubs.c).
 */
#ifndef BOOT_STUBS_H
#define BOOT_STUBS_H

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>

#include "backup_layout.h"
#include "layout.h"
#include "stm32g4xx.h"

/* Room for the UART text of one test. */
#define UART_LOG_BYTES 2048U

/* Everything the bootloader wrote to the UART since uart_log_clear(). */
extern char uart_log[UART_LOG_BYTES];

void uart_log_clear(void);

/* What the bootloader did, in order, one character per event, since ev_clear():
 *   W watchdog_start_long   R watchdog_reload   F watchdog_set_firmware_timeout
 *   C cycles_start          K clock_to_170mhz   U uart_init   O uart_off
 *   H handover_start        Z fault_impossible_value
 * and, added by test_flow.c's own stand-ins, E boot_state_write and X image_check_contents. */
#define EV_MAX 96U
extern char ev_trace[EV_MAX];

void ev_clear(void);
void ev_add(char event);

/* cycles_start() answers this: false stands for a cycle counter that does not count. */
extern bool stub_cycles_counts;

/* watchdog_running() answers this (decision F(b)); true unless a test sets it false. It also hands
 * back stub_watchdog_sr and stub_rcc_csr as the register values it "read" (0 and LSIRDY, the
 * values of a running watchdog, unless a test sets others). */
extern bool stub_watchdog_running;
extern uint32_t stub_watchdog_sr;
extern uint32_t stub_rcc_csr;

/* How a run under test ended. A test sets run_exit with setjmp and arms it (run_exit_armed);
 * the stand-ins leave by longjmp(run_exit, <one of these>):
 *   RUN_PARKED   the third watchdog_reload() after uart_off(): the park loop;
 *   RUN_STARTED  handover_start(): a firmware would start (started_vector says which);
 *   RUN_FAULT    fault_impossible_value() (fault_where says where).
 * Unarmed, a hand-over or a fault prints FAIL and stops the test program. */
#define RUN_PARKED   1
#define RUN_STARTED  2
#define RUN_FAULT    3
extern jmp_buf run_exit;
extern bool run_exit_armed;
extern volatile int run_how;   /* set to RUN_... by the stand-in just before its longjmp */
extern uint32_t started_vector;
extern char fault_where[64];
extern uint32_t fault_calls;

/* Runs `statement` with run_exit armed and sets `ended` to how it ended: 0 if the statement came
 * back, or RUN_PARKED, RUN_STARTED, RUN_FAULT (from run_how). setjmp(...) == 0 is the whole
 * controlling expression of the if, one of the places C11 allows setjmp (ISO/IEC 9899:2011,
 * 7.13.1.1). Host tests only: JPL Power of Ten bans setjmp in flight code (CODE_RULES A9), and this
 * is the one way to leave a function the code under test declares noreturn. A variable the
 * statement changes must not be read after a longjmp unless it is volatile. */
#define RUN_CATCH(ended, statement)        \
    do {                                   \
        run_exit_armed = true;             \
        run_how = 0;                       \
        if (setjmp(run_exit) == 0) {       \
            statement;                     \
            run_exit_armed = false;        \
        }                                  \
        (ended) = run_how;                 \
    } while (0)

/* Clears the fault record (fault_where, fault_calls) before a test. */
void fault_clear(void);

/* The TAMP backup registers, as RAM (common/backup_layout.h): pair n is words 2n and 2n + 1.
 * stub_backup_writable false makes every backup_write fail and change nothing. stub_backup_open
 * is true between backup_open() and backup_close(). */
extern uint32_t stub_backup[BACKUP_REGISTERS];
extern bool stub_backup_writable;
extern bool stub_backup_open;

/* Store `value` with its complement in pair `pair` / clear every pair (both words 0: broken, as
 * after an RTC domain reset). */
void stub_backup_set(uint32_t pair, uint32_t value);
void stub_backup_reset(void);
/* The value of a pair, and whether it matches its complement. */
bool stub_backup_get(uint32_t pair, uint32_t *value);

/* The flash as RAM, served by flash_at(): index = chip address - FLASH_START. Erased (0xFF)
 * after stub_flash_reset(). A read through flash_at() of the address stub_ecc_address (0: none)
 * sets the ECC error flag, as a two-bit error would. */
extern uint8_t stub_flash[FLASH_BYTES];
extern uint32_t stub_ecc_address;
void stub_flash_reset(void);

#endif /* BOOT_STUBS_H */
