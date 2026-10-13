/*
 * test_flash.c — host unit tests of the flash address barrier (src/controller/common/flash.c:
 * flash_erase_page, flash_program), with the FLASH registers replaced by a RAM structure
 * (unit/flash_stubs). The chip's flash is not touched: a program that is allowed is tested only
 * with zero pairs, because a real program would write to the chip's addresses.
 *
 * The rules under test (audit finding #15): erase and program refuse any address outside the
 * main flash (0x0800 0000 to 0x0801 FFFF) and any page of the bootloader region and of the
 * last-resort region, return false, and touch no register. An allowed erase starts with the
 * right page number in FLASH_CR PNB. Before the barrier, every one of the refused calls reached
 * the chip. The test also links flash.c alone: it needs no watchdog function, which shows that
 * nothing reloads a watchdog inside the flash wait (audit findings #3 and #4).
 *
 * Built with the Mac's cc and the undefined-behaviour sanitizer. Run: make -C test/controller
 * unit. Prints PASS or FAIL per test; exit status 1 on a failure.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "flash.h"
#include "layout.h"
#include "stm32g4xx.h"

FLASH_TypeDef fake_flash;

static uint32_t strt_count;
static uint32_t strt_cr;
static uint32_t failures;
static uint32_t tests;

/**
 * @brief Records FLASH_CR at the moment STRT is written, and clears the error flags.
 *
 * flash.c clears the error flags by writing 1 to them before it starts an operation
 * [RM0440 Rev 9, Sec. 5.7.5, p. 217-218, "Cleared by writing 1"]. A RAM register keeps the 1s instead, so the stand-in
 * clears FLASH_SR here, where the chip would have cleared it, and no operation error is left.
 *
 * @return the STRT bit (bit 16)
 */
uint32_t flash_strt_hook(void)
{
    strt_count++;
    strt_cr = fake_flash.CR;
    fake_flash.SR = 0U;
    return 1UL << 16;
}

/**
 * @brief Counts one test result and prints a PASS or FAIL line for it.
 *
 * @param[in]     ok    true if the check passed
 * @param[in]     name  text printed after PASS or FAIL
 */
static void report(bool ok, const char *name)
{
    tests++;
    if (!ok) {
        failures++;
    }
    printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
}

/**
 * @brief Resets the fake registers to a locked flash, idle and without errors.
 */
static void reset_rig(void)
{
    memset(&fake_flash, 0, sizeof fake_flash);
    fake_flash.CR = FLASH_CR_LOCK;
    strt_count = 0U;
    strt_cr = 0U;
}

/**
 * @brief Returns true if no register was touched since reset_rig().
 *
 * Untouched means: still locked, no key written, no erase started, no error flag cleared.
 *
 * @return true if the fake registers are as reset_rig() left them
 */
static bool untouched(void)
{
    return (fake_flash.CR == FLASH_CR_LOCK) && (fake_flash.KEYR == 0U) && (strt_count == 0U) &&
           (fake_flash.ACR == 0U);
}

/**
 * @brief Erase of an address the barrier refuses: false, nothing touched.
 *
 * @param[in]     address  address given to flash_erase_page
 * @return true if the call returned false and touched no register
 */
static bool erase_refused(uint32_t address)
{
    reset_rig();
    bool result = flash_erase_page(address);
    return !result && untouched();
}

/**
 * @brief Erase of an allowed page: true, started once, with the given page number in PNB.
 *
 * @param[in]     address  address of the page
 * @param[in]     page     page number expected in PNB
 * @return true if the erase ran with PER and that page number
 */
static bool erase_runs(uint32_t address, uint32_t page)
{
    reset_rig();
    bool result = flash_erase_page(address);
    return result && (strt_count == 1U) && ((strt_cr & FLASH_CR_PER) != 0U) &&
           (((strt_cr & FLASH_CR_PNB_Msk) >> FLASH_CR_PNB_Pos) == page) &&
           ((fake_flash.CR & FLASH_CR_LOCK) != 0U);
}

/**
 * @brief Program of a range the barrier refuses: false, nothing touched.
 *
 * @param[in]     address  start address
 * @param[in]     pairs    64-bit words
 * @return true if the call returned false and touched no register
 */
static bool program_refused(uint32_t address, uint32_t pairs)
{
    const uint32_t words[2] = {0U, 0U};

    reset_rig();
    bool result = flash_program(address, words, pairs);
    return !result && untouched();
}

/**
 * @brief The erase barrier: refused pages and addresses, allowed pages with their PNB.
 */
static void test_erase(void)
{
    report(erase_refused(0x08000000U) && erase_refused(0x08002800U) && erase_refused(0x08005800U),
           "erase: first, middle and last page of the bootloader region refused");
    report(erase_refused(0x0801A000U) && erase_refused(0x0801B000U) && erase_refused(0x0801D800U),
           "erase: first, middle and last page of the last-resort region refused");
    report(erase_refused(0x08005FFFU) && erase_refused(0x0801D801U),
           "erase: an address inside a protected page, not at its start, refused (page it erases)");
    report(erase_refused(0x08020000U) && erase_refused(0x0802F800U) && erase_refused(0xFFFFF800U),
           "erase: at and past the end of the main flash refused (no wrap)");
    report(erase_refused(0x07FFF800U) && erase_refused(0x20000000U) && erase_refused(0U),
           "erase: below the main flash, in RAM, at 0 refused");
    report(erase_runs(0x08006000U, 12U) && erase_runs(0x08019800U, 51U),
           "erase: first page of slot A (page 12) and last page of slot B (page 51) run, PNB right");
    report(erase_runs(0x0801E000U, 60U) && erase_runs(0x0801F800U, 63U),
           "erase: boot-state pages 60 and 63 run, PNB right (63 fills the 6-bit field)");
}

/**
 * @brief The program barrier: refused ranges; an allowed start with zero pairs is accepted.
 */
static void test_program(void)
{
    report(program_refused(0x08000000U, 1U) && program_refused(0x08005FF8U, 1U),
           "program: start inside the bootloader region refused");
    report(program_refused(0x08005FF8U, 2U),
           "program: a range that starts in the bootloader region's last word and runs on refused");
    report(program_refused(0x08019FF8U, 2U) && program_refused(0x0801A000U, 1U) &&
               program_refused(0x08019000U, 0x400U),
           "program: ranges that enter the last-resort region refused");
    report(program_refused(0x0801FFF8U, 2U) && program_refused(0x08020000U, 1U) &&
               program_refused(0x20000000U, 1U) && program_refused(0x07FFFFF8U, 1U),
           "program: past the end of the flash, in RAM or below the flash refused");
    report(program_refused(0x08006000U, 0x20000000U) && program_refused(0x0801E000U, 0xFFFFFFFFU),
           "program: a pair count that would wrap the length refused");

    const uint32_t words[2] = {0U, 0U};
    reset_rig();
    bool zero_ok = flash_program(0x08006000U, words, 0U);
    report(zero_ok, "program: zero pairs at the start of slot A accepted (nothing to write)");
}

/**
 * @brief Runs the tests and prints the totals.
 *
 * @return 0 if every test passed, 1 otherwise
 */
int main(void)
{
    test_erase();
    test_program();
    if (failures == 0U) {
        printf("flash unit tests: all %u passed\n", tests);
        return 0;
    }
    printf("flash unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
