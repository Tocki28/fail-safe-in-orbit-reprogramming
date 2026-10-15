/*
 * test_separation_flash.c — host differential test of the bootloader's flash module (bootloader/
 * bl_flash.c) against the firmware's (common/flash.c), REQ-119, owner decision 35, 15 Oct 2026.
 * The other modules are compared in test_separation.c; this one is apart because both flash
 * modules write the same FLASH registers, which a RAM structure stands in for here
 * (unit/flash_stubs, as in test_flash.c), and a RAM flash behind flash_at() cannot be linked into
 * the same program.
 *
 * Both modules get the same arguments on the same register stand-in; what they do to the
 * registers and what they answer must be the same. Compared: the answer; whether a register was
 * touched; the page number, PER and PG at the moment STRT is written; the lock bit at the end;
 * the ECC flags after a clear and after the NMI; the pointer flash_at gives.
 *
 * Only calls that cannot reach the chip's flash are made with pairs above zero: a program that
 * is allowed would write to the chip's own addresses through a pointer, which a host cannot do
 * (test_flash.c has the same limit). A bad range is therefore only tried where the common module
 * refuses it. The two cases that differ only in the unaligned bit are tried with zero pairs and
 * in refused ranges: common/flash.c does not check the alignment of an allowed range
 * (flash.h says "8-byte aligned" and range_allowed() does not test it), so an unaligned allowed
 * start with pairs above zero cannot be tried on the host, and whether the bootloader's module
 * should refuse it is a human call.
 *
 * If the bootloader's module accepts a range that the common one refuses, it writes through a
 * pointer to the chip's address, which a host cannot map: the run ends with a signal. A handler
 * (on_signal) prints which call it was, as a FAIL line, and stops with status 1.
 *
 * flash.c is compiled with its NMI_Handler renamed (-DNMI_Handler=common_flash_nmi_handler, test/
 * controller/Makefile) because bl_flash.c defines the NMI_Handler that the bootloader's image
 * needs and two definitions cannot be linked together.
 *
 * Built with the Mac's cc and the undefined-behaviour sanitizer. Run: make -C test/controller
 * unit. Prints PASS or FAIL per test; exit status 1 on a failure.
 */
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bl_flash.h"
#include "flash.h"
#include "layout.h"
#include "stm32g4xx.h"

/* The handlers: the common one renamed at compile time, the bootloader's under its real name. */
void common_flash_nmi_handler(void);
void NMI_Handler(void);

FLASH_TypeDef fake_flash;

/* Bits of FLASH_CR the stand-in records at the moment STRT is written (flash_stubs/stm32g4xx.h
 * has the values of ST's header). */
#define WATCHED_CR_BITS (FLASH_CR_PER | FLASH_CR_PG | FLASH_CR_PNB_Msk)
#define PAIR_WORDS      2U
#define ADDRESS_NONE    0xFFFFFFFFU

#define SIGNAL_NOTE_BYTES 120U
static char signal_note[SIGNAL_NOTE_BYTES];
static uint32_t strt_count;
static uint32_t strt_cr;
static uint32_t failures;
static uint32_t tests;
static char detail[160];

/**
 * @brief Records FLASH_CR at the moment STRT is written, and clears the error flags.
 *
 * Same as test_flash.c: a RAM register keeps the 1s that the chip would have cleared.
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
 * @brief Counts one test result and prints a PASS or FAIL line, with the first difference.
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
    if (!ok && (detail[0] != '\0')) {
        printf("      first difference: %s\n", detail);
    }
    detail[0] = '\0';
}

/**
 * @brief Signal handler: prints the call that was running when the program wrote to an address
 * the host cannot map, and stops.
 *
 * @param[in]     signal_number  the signal (not used)
 * @note Does not return.
 */
static void on_signal(int signal_number)
{
    (void)signal_number;
    (void)write(STDOUT_FILENO, signal_note, strlen(signal_note));
    _exit(1);
}

/**
 * @brief Remembers which call is about to run, for on_signal.
 *
 * @param[in]     who      "common" or "bootloader"
 * @param[in]     address  address given
 * @param[in]     pairs    pairs given
 */
static void announce(const char *who, uint32_t address, uint32_t pairs)
{
    (void)snprintf(signal_note, sizeof signal_note,
                   "FAIL  flash: %s program(0x%08x, %u pairs) reached the chip's flash "
                   "(signal)\n", who, (unsigned)address, (unsigned)pairs);
}

/**
 * @brief Resets the stand-in registers to a locked flash, idle and without errors.
 */
static void reset_rig(void)
{
    memset(&fake_flash, 0, sizeof fake_flash);
    fake_flash.CR = FLASH_CR_LOCK;
    strt_count = 0U;
    strt_cr = 0U;
}

/* What one call did, to compare the two modules. */
typedef struct {
    bool result;          /* the answer */
    uint32_t strt_count;  /* times STRT was written */
    uint32_t strt_bits;   /* PER, PG and PNB at the moment of STRT */
    bool locked_after;    /* FLASH_CR lock bit at the end */
    bool untouched;       /* no key written, no operation started, lock bit still set */
} effect_t;

/**
 * @brief Reads what a call did to the stand-in registers.
 *
 * @param[in]     result  the call's answer
 * @return the effect
 */
static effect_t effect_of(bool result)
{
    effect_t e;

    e.result = result;
    e.strt_count = strt_count;
    e.strt_bits = strt_cr & WATCHED_CR_BITS;
    e.locked_after = (fake_flash.CR & FLASH_CR_LOCK) != 0U;
    e.untouched = e.locked_after && (fake_flash.KEYR == 0U) && (strt_count == 0U) &&
                  (fake_flash.ACR == 0U) && (fake_flash.SR == 0U);
    return e;
}

/**
 * @brief Returns true if two effects are the same.
 *
 * @param[in]     a  effect of the common module
 * @param[in]     b  effect of the bootloader's module
 * @return true if the answer, the STRT record and the lock bit are equal, and, for a refused
 *         call, both left every register untouched. An accepted call is not required to touch the
 *         same registers: common/flash.c unlocks and locks the flash even for zero pairs, and the
 *         bootloader's module may do nothing; the answer, no operation started and a locked
 *         flash at the end are what the callers see. This reading is an assumption awaiting
 *         the owner's acceptance (rule: no assumptions).
 */
static bool same_effect(const effect_t *a, const effect_t *b)
{
    return (a->result == b->result) && (a->strt_count == b->strt_count) &&
           (a->strt_bits == b->strt_bits) && (a->locked_after == b->locked_after) &&
           (a->result || (a->untouched == b->untouched));
}

/**
 * @brief Remembers the first input on which the two modules differed.
 *
 * @param[in]     what     what was compared
 * @param[in]     address  address given
 * @param[in]     pairs    pairs given (0 for an erase)
 * @param[in]     a        effect of the common module
 * @param[in]     b        effect of the bootloader's module
 */
static void note_difference(const char *what, uint32_t address, uint32_t pairs, const effect_t *a,
                            const effect_t *b)
{
    if (detail[0] == '\0') {
        (void)snprintf(detail, sizeof detail,
                       "%s at 0x%08x, pairs %u: common (answer %d, STRT %u, untouched %d), "
                       "bootloader (answer %d, STRT %u, untouched %d)",
                       what, (unsigned)address, (unsigned)pairs, (int)a->result,
                       (unsigned)a->strt_count, (int)a->untouched, (int)b->result,
                       (unsigned)b->strt_count, (int)b->untouched);
    }
}

/**
 * @brief Erases one address with both modules and compares what they did.
 *
 * @param[in]     address     address given to erase
 * @param[out]    ref_result  the common module's answer
 * @return true if the effects are the same
 */
static bool erase_same(uint32_t address, bool *ref_result)
{
    reset_rig();
    effect_t common = effect_of(flash_erase_page(address));
    reset_rig();
    effect_t boot = effect_of(bl_flash_erase_page(address));

    *ref_result = common.result;
    if (!same_effect(&common, &boot)) {
        note_difference("erase", address, 0U, &common, &boot);
        return false;
    }
    return true;
}

/**
 * @brief Programs with both modules and compares what they did.
 *
 * @param[in]     address  start address
 * @param[in]     pairs    64-bit words
 * @return true if the effects are the same
 */
static bool program_same(uint32_t address, uint32_t pairs)
{
    const uint32_t words[PAIR_WORDS] = {0U, 0U};

    reset_rig();
    announce("common", address, pairs);
    effect_t common = effect_of(flash_program(address, words, pairs));
    reset_rig();
    announce("bootloader", address, pairs);
    effect_t boot = effect_of(bl_flash_program(address, words, pairs));

    if (!same_effect(&common, &boot)) {
        note_difference("program", address, pairs, &common, &boot);
        return false;
    }
    return true;
}

/**
 * @brief Erase of many addresses by both modules: refused pages, refused addresses, and the
 * pages that run, with the same page number in FLASH_CR.
 *
 * The addresses are those of test_flash.c (first, middle and last page of the bootloader and the
 * last-resort region; just inside a protected page; at and past the end of the flash; below it;
 * RAM; 0), plus the pages next to the protected ones, an address inside an allowed page, and the
 * 6-bit page field's limit (page 63).
 *
 * Catches: the bootloader's erase has a barrier one page off (erases the last page of the
 * bootloader or refuses the first page of slot A), forgets the last-resort region, lets an
 * address past the end wrap into the flash (0xFFFFF800), erases the page that holds an
 * unaligned address differently, writes the wrong page number or no PER, or does not lock the
 * flash again.
 */
static void test_erase_same(void)
{
    const uint32_t addresses[] = {
        0x08000000U, 0x08002800U, 0x08005800U, 0x08005FFFU, 0x08006000U, 0x08006001U,
        0x08006400U, 0x08019800U, 0x08019FFFU, 0x0801A000U, 0x0801B000U, 0x0801D800U,
        0x0801D801U, 0x0801E000U, 0x0801F000U, 0x0801F800U, 0x0801FFFFU, 0x08020000U,
        0x0802F800U, 0xFFFFF800U, 0xFFFFFFFFU, 0x07FFF800U, 0x07FFFFFFU, 0x20000000U, 0U,
    };
    bool all = true;
    uint32_t ran = 0U;
    bool ref = false;

    for (uint32_t i = 0U; i < (uint32_t)(sizeof addresses / sizeof addresses[0]); i++) {
        all = erase_same(addresses[i], &ref) && all;
        ran += ref ? 1U : 0U;
    }
    report(all && (ran > 0U), "flash: erase of 25 addresses, bootloader's module equals common "
                              "(same answer, same page number, same lock)");
}

/**
 * @brief Program of ranges the common module refuses, and of zero pairs, by both modules.
 *
 * Refused ranges: start inside the bootloader region, a range that runs into it or into the
 * last-resort region, past the end of the flash, in RAM, below the flash, a pair count that would
 * wrap the length, an unaligned start in a refused range. Zero pairs: aligned and unaligned
 * starts, inside and outside the flash, in the protected regions.
 *
 * Catches: the bootloader's program lets a range through that crosses into the protected region
 * (here the first byte of the last-resort region, or the last word of the bootloader), multiplies
 * pairs by 8 without a check (a wrapped length passes the barrier), answers differently for zero
 * pairs (common answers true inside the flash and touches only the lock), or touches a register
 * before it refuses.
 */
static void test_program_same(void)
{
    const uint32_t refused[][2] = {
        {0x08000000U, 1U}, {0x08005FF8U, 1U}, {0x08005FF8U, 2U}, {0x08019FF8U, 2U},
        {0x0801A000U, 1U}, {0x08019000U, 0x400U}, {0x0801FFF8U, 2U}, {0x08020000U, 1U},
        {0x20000000U, 1U}, {0x07FFFFF8U, 1U}, {0x08006000U, 0x20000000U},
        {0x0801E000U, 0xFFFFFFFFU}, {0x08005FF9U, 1U}, {0x08000003U, 1U}, {0x0801FFFDU, 1U},
        {0x07FFFFF9U, 1U}, {0xFFFFFFF8U, 1U}, {0xFFFFFFFFU, 1U},
    };
    const uint32_t zero_starts[] = {
        0x08000000U, 0x08005FF8U, 0x08006000U, 0x08006001U, 0x08006004U, 0x08006007U,
        0x0801A000U, 0x0801E000U, 0x0801FFF8U, 0x08020000U, 0x20000000U, 0x07FFFFF8U, 0U,
        0xFFFFFFFFU,
    };
    bool all = true;

    for (uint32_t i = 0U; i < (uint32_t)(sizeof refused / sizeof refused[0]); i++) {
        all = program_same(refused[i][0], refused[i][1]) && all;
    }
    for (uint32_t i = 0U; i < (uint32_t)(sizeof zero_starts / sizeof zero_starts[0]); i++) {
        all = program_same(zero_starts[i], 0U) && all;
    }
    report(all, "flash: program of 18 refused ranges and 14 starts with zero pairs, bootloader's "
                "module equals common");
}

/**
 * @brief The ECC bookkeeping: clear, error seen, the NMI that records an error, clear again.
 *
 * The NMI is called with ECCD set, as the chip raises it for a two-bit error
 * [RM0440 Rev 9, Sec. 5.3.2, p. 190]. Both modules must clear the flags by writing 1 to ECCD and
 * ECCC and keep ECCIE, record the error in their own flag, and forget it on the next clear. The
 * run is repeated with ECCIE set and clear.
 *
 * Catches: the bootloader's NMI handler records nothing (the error flag stays false, so a damaged
 * boot state is read as good), does not clear ECCD (the NMI fires again at once), clears ECCIE
 * (a later error raises no NMI), or does not reset its flag in ecc_clear.
 */
static void test_ecc_same(void)
{
    const uint32_t eccie_values[2] = {0U, FLASH_ECCR_ECCIE};
    bool all = true;

    for (uint32_t v = 0U; v < 2U; v++) {
        reset_rig();
        fake_flash.ECCR = eccie_values[v];
        flash_ecc_clear();
        uint32_t common_after_clear = fake_flash.ECCR;
        bool common_clear_seen = flash_ecc_error_seen();
        fake_flash.ECCR = eccie_values[v] | FLASH_ECCR_ECCD;
        common_flash_nmi_handler();
        uint32_t common_after_nmi = fake_flash.ECCR;
        bool common_nmi_seen = flash_ecc_error_seen();
        flash_ecc_clear();
        bool common_forgot = !flash_ecc_error_seen();

        reset_rig();
        fake_flash.ECCR = eccie_values[v];
        bl_flash_ecc_clear();
        uint32_t boot_after_clear = fake_flash.ECCR;
        bool boot_clear_seen = bl_flash_ecc_error_seen();
        fake_flash.ECCR = eccie_values[v] | FLASH_ECCR_ECCD;
        NMI_Handler();
        uint32_t boot_after_nmi = fake_flash.ECCR;
        bool boot_nmi_seen = bl_flash_ecc_error_seen();
        bl_flash_ecc_clear();
        bool boot_forgot = !bl_flash_ecc_error_seen();

        bool same = (common_after_clear == boot_after_clear) && (common_clear_seen == false) &&
                    (boot_clear_seen == false) && (common_after_nmi == boot_after_nmi) &&
                    common_nmi_seen && boot_nmi_seen && common_forgot && boot_forgot;
        if (!same && (detail[0] == '\0')) {
            (void)snprintf(detail, sizeof detail,
                           "ECCIE %u: ECCR after clear 0x%08x/0x%08x, after NMI 0x%08x/0x%08x, "
                           "seen after NMI %d/%d, forgot %d/%d",
                           (unsigned)v, (unsigned)common_after_clear, (unsigned)boot_after_clear,
                           (unsigned)common_after_nmi, (unsigned)boot_after_nmi,
                           (int)common_nmi_seen, (int)boot_nmi_seen, (int)common_forgot,
                           (int)boot_forgot);
        }
        all = all && same;
    }
    report(all, "flash: ECC clear, NMI record and second clear equal in both modules, ECCIE kept");
}

/**
 * @brief flash_at gives the same pointer as bl_flash_at: the address itself.
 *
 * The flash is read in place at its own address [RM0440 Rev 9, Table 28, p. 189]; only the
 * pointer is compared, nothing is read.
 *
 * Catches: the bootloader's flash_at adds an offset or masks the address (every flash read, the
 * boot state included, would read the wrong bytes), or truncates it to 16 bits.
 */
static void test_flash_at_same(void)
{
    const uint32_t addresses[] = {0x08000000U, 0x08006000U, 0x0801E000U, 0x0801FFFFU, 0U};
    bool all = true;

    for (uint32_t i = 0U; i < (uint32_t)(sizeof addresses / sizeof addresses[0]); i++) {
        if (flash_at(addresses[i]) != bl_flash_at(addresses[i])) {
            all = false;
            if (detail[0] == '\0') {
                (void)snprintf(detail, sizeof detail, "flash_at(0x%08x) differs",
                               (unsigned)addresses[i]);
            }
        }
    }
    report(all, "flash: flash_at gives the same pointer in both modules");
}

/**
 * @brief Runs the tests and prints the totals.
 *
 * @return 0 if every test passed, 1 otherwise
 */
int main(void)
{
    (void)signal(SIGSEGV, on_signal);
    (void)signal(SIGBUS, on_signal);
    test_erase_same();
    test_program_same();
    test_ecc_same();
    test_flash_at_same();
    if (failures == 0U) {
        printf("separation flash tests: all %u passed\n", tests);
        return 0;
    }
    printf("separation flash tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
