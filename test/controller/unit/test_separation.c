/*
 * test_separation.c — host differential test of the bootloader's own code against the shared
 * code of common/ (REQ-119: the bootloader shares no code with the firmware; owner decision 35,
 * 15 Oct 2026).
 *
 * The bootloader has its own, independently written versions of four modules (bootloader/
 * bl_crc32.c, bl_boot_state.c, bl_image_check.c, bl_flash.c). The firmware keeps common/crc32.c,
 * boot_state.c, image_check.c and flash.c. Two copies of the same job can drift apart, and the
 * flash holds data that both must read the same way: the boot state the firmware's SWITCH and
 * CONFIRM write and the bootloader reads, the images the update receiver checks with common/
 * code and the bootloader checks again with its own. This test links BOTH sets in one program
 * and gives them the same input, so any difference is a failure here, on the host, before it is a
 * boot failure on the board.
 *
 * Which copy is the reference: common/ is the code that runs today and passed the existing
 * tests; the bl_ copy is judged against it. Where the two differ, this test says "common" and
 * "bootloader" and prints the first input that differs. It does not say which one is right: that
 * is a human call, and the answer is a new test or a changed module, not a looser comparison.
 *
 * What is compared (the spec of the bl_ modules is "same signature and meaning"):
 *   - CRC-32: pinned zlib values, every length 0 to 300 at four alignments, large random buffers
 *     up to one slot, every 1-byte and 2-byte input;
 *   - boot state: boot_state_classify on every pair from a catalogue of 20 record shapes with
 *     every readable/unreadable combination; boot_state_load and boot_state_read on the same
 *     pairs placed in a RAM flash, with and without a flash ECC error; boot_state_write from
 *     every one of those starting states (same copy chosen, same sequence number, same bytes in
 *     flash); a record written by one module read by the other in every order; refused records;
 *     a power cut at every step of a write; one flipped byte in each stored record; the fault
 *     reaction for every slot code 0 to 255;
 *   - image check: every signed image the build makes (argv[1], argv[2]) in every region with
 *     every image kind, plus hundreds of damaged copies of the images that pass; result text for
 *     every result value and for values outside the enum; little-endian reads.
 * The flash module (bl_flash.c against flash.c) is compared in test_separation_flash.c, because
 * both talk to the same FLASH registers and a RAM flash cannot replace them in the same program.
 *
 * The RAM flash below stands in for the chip's flash. It has one RAM array and two sets of
 * entry points: flash_* for common/boot_state.c and bl_flash_* for bl_boot_state.c. It refuses
 * what the chip refuses (an unaligned or non-erased program, a page outside the boot state), so
 * a module that forgets the erase fails here as it would on the chip.
 *
 * Built with the Mac's cc and the undefined-behaviour sanitizer. Run: make -C test/controller
 * unit. Usage: test_separation <firmware image directory> <test image directory>. Prints PASS
 * or FAIL per test; exit status 1 on a failure.
 */
#include <dirent.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bl_boot_state.h"
#include "bl_crc32.h"
#include "bl_flash.h"
#include "bl_image_check.h"
#include "boot_state.h"
#include "crc32.h"
#include "fault.h"
#include "flash.h"
#include "image_check.h"
#include "image_format.h"
#include "layout.h"

/* ---- Constants ------------------------------------------------------------------------- */

/* Seed of the random bytes: fixed, so a failure can be repeated (as tools/check_crc32.py). */
#define RNG_SEED          0x20261015U
/* Marsaglia xorshift32 shifts [G. Marsaglia, "Xorshift RNGs", J. Stat. Softw. 8(14), 2003,
 * p. 3, the 13-17-5 triple]. Any generator would do: the bytes only have to be uneven. */
#define RNG_SHIFT_A       13U
#define RNG_SHIFT_B       17U
#define RNG_SHIFT_C       5U
#define BITS_PER_BYTE     8U
#define BYTE_MASK         0xFFU
#define TOP_BYTE_SHIFT    24U
/* Longest input of the every-length sweep (bytes). No technical reason for 300: it is past the
 * 256 distinct byte values and past several word and double-word multiples. */
#define SWEEP_MAX_LENGTH  300U
/* Start offsets of the CRC input within a buffer, so a word-at-a-time reader is caught. */
#define ALIGNMENTS        4U
/* Largest CRC input: one slot, the longest body the bootloader checks (layout.h). */
#define SLOT_BUFFER_BYTES ((uint32_t)SLOT_BYTES)
#define CRC_BUFFER_BYTES  (SLOT_BUFFER_BYTES + ALIGNMENTS)
/* The flash ECC and fault-reaction detail line. */
#define DETAIL_BYTES      300U
/* Bytes of a boot-state record (boot_state.h: two 64-bit flash words). */
#define RECORD_BYTES      ((uint32_t)sizeof(boot_state_t))
#define RECORD_HALF       (RECORD_BYTES / 2U)
#define PAIR_BYTES        8U
#define WORD_BYTES        4U
/* The two stored copies and the bytes the boot state occupies in flash (layout.h). */
#define COPIES            2U
#define STATE_AREA_BYTES  (COPIES * (uint32_t)BOOT_STATE_COPY_BYTES)
#define RAM_FLASH_BYTES   ((uint32_t)FLASH_BYTES)
/* Byte that fills a result the code under test must leave alone. */
#define SENTINEL          0xA5U
#define ERASED            0xFFU
/* Largest value of the 8-bit slot code, and the first one past the slot codes A and B. */
#define SLOT_CODES        256U
/* The ECC cases: none, copy 0, copy 1, both copies read with an ECC error. */
#define ECC_MASKS         4U
/* Write sequence of the format test: six writes with allowed fields. */
#define WRITE_STEPS       6U
#define WRITE_PATTERNS    (1U << WRITE_STEPS)

/* ---- Counting, printing -------------------------------------------------------------- */

static uint32_t failures;
static uint32_t tests;
static char detail[DETAIL_BYTES];

/**
 * @brief Remembers the first input on which the two modules differed, for the FAIL line.
 *
 * Only the first difference of a test is kept: it is the one to look at; the rest are the same
 * defect repeated.
 *
 * @param[in]     what     what was compared
 * @param[in]     case_no  number of the case in the test's sweep
 * @param[in]     common   value from the common/ module
 * @param[in]     boot     value from the bootloader's module
 */
static void note_difference(const char *what, uint32_t case_no, uint32_t common, uint32_t boot)
{
    if (detail[0] == '\0') {
        (void)snprintf(detail, sizeof detail, "%s, case %u: common 0x%08x, bootloader 0x%08x",
                       what, (unsigned)case_no, (unsigned)common, (unsigned)boot);
    }
}

/**
 * @brief Counts one test result and prints a PASS or FAIL line, with the first difference found.
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

/* ---- Random bytes -------------------------------------------------------------------- */

static uint32_t rng_state = RNG_SEED;

/**
 * @brief Returns the next 32-bit pseudo-random value (xorshift32).
 *
 * @return the value; never 0 for a non-zero seed
 */
static uint32_t rng_next(void)
{
    uint32_t x = rng_state;

    x ^= x << RNG_SHIFT_A;
    x ^= x >> RNG_SHIFT_B;
    x ^= x << RNG_SHIFT_C;
    rng_state = x;
    return x;
}

/**
 * @brief Fills a buffer with pseudo-random bytes.
 *
 * @param[out]    buffer  bytes to fill
 * @param[in]     size    number of bytes
 */
static void rng_fill(uint8_t *buffer, uint32_t size)
{
    for (uint32_t i = 0U; i < size; i++) {
        buffer[i] = (uint8_t)(rng_next() >> TOP_BYTE_SHIFT);
    }
}

/* ---- The fault reaction -------------------------------------------------------------- */

static jmp_buf fault_jump;
static bool fault_armed;

/**
 * @brief Stand-in for the fault reaction (fault.h): leaves the guarded call by longjmp.
 *
 * Both modules call the fault reaction for a value that cannot exist (owner's decision, 13 Oct
 * 2026). The location codes may differ between the two modules (each fault site has its own code,
 * tools check_fault_codes.py), so only the fact of the fault is compared, not the code.
 *
 * @param[in]     location  fault-location code (not compared)
 * @param[in]     where     name of the function (not compared)
 * @note Does not return. Stops the test program if no guarded call is running.
 */
FAULT_NORETURN void fault_impossible_value(uint8_t location, const char *where)
{
    (void)location;
    if (!fault_armed) {
        printf("FAIL  fault reaction outside a guarded call, in %s\n", where);
        exit(1);
    }
    fault_armed = false;
    longjmp(fault_jump, 1);
}

/* ---- The RAM flash ------------------------------------------------------------------- */

static uint8_t ram[RAM_FLASH_BYTES];
/* Which stored copies raise a flash ECC error when read (bit 0: copy 0, bit 1: copy 1). */
static uint32_t ecc_mask;
static bool ecc_seen;
/* Power-cut model of a write: the erase does nothing, or the program stops after this many
 * 32-bit words (RAM_CUT_NEVER: no cut). */
#define RAM_CUT_NEVER 0xFFFFFFFFU
static bool cut_erase;
static uint32_t cut_after_words;
static uint32_t words_programmed;

/**
 * @brief Returns the start address of a stored boot-state copy.
 *
 * @param[in]     copy  0 or 1
 * @return chip address of the copy
 */
static uint32_t copy_address(uint32_t copy)
{
    return (copy == 0U) ? (uint32_t)BOOT_STATE_COPY_0 : (uint32_t)BOOT_STATE_COPY_1;
}

/**
 * @brief Returns the index into ram[] of a chip address, or stops the test for one outside.
 *
 * @param[in]     address  chip address inside the main flash
 * @return index into ram[]
 */
static uint32_t ram_index(uint32_t address)
{
    if ((address < (uint32_t)FLASH_START) ||
        ((address - (uint32_t)FLASH_START) >= RAM_FLASH_BYTES)) {
        printf("FAIL  flash read at 0x%08x: outside the main flash\n", (unsigned)address);
        exit(1);
    }
    return address - (uint32_t)FLASH_START;
}

/**
 * @brief Serves a flash read from RAM; a read inside a copy with an ECC error sets the flag.
 *
 * Any address inside the 16 bytes of the copy counts, so a module that reads the record in two
 * pieces still meets the error, as the chip would.
 *
 * @param[in]     address  chip address
 * @return pointer into ram[]
 */
static const uint8_t *ram_at(uint32_t address)
{
    for (uint32_t c = 0U; c < COPIES; c++) {
        if (((ecc_mask >> c) & 1U) != 0U) {
            uint32_t first = copy_address(c);
            if ((address >= first) && (address < (first + RECORD_BYTES))) {
                ecc_seen = true;
            }
        }
    }
    return &ram[ram_index(address)];
}

/**
 * @brief Erases one page of the boot-state area, unless the power-cut model says the erase was
 * cut.
 *
 * @param[in]     page_start  chip address of the page, 2 KB aligned, inside the boot state
 * @return true if the page was erased
 */
static bool ram_erase(uint32_t page_start)
{
    if ((page_start % (uint32_t)FLASH_PAGE_BYTES) != 0U) {
        return false;
    }
    if ((page_start < (uint32_t)BOOT_STATE_START) ||
        (page_start >= ((uint32_t)BOOT_STATE_START + STATE_AREA_BYTES))) {
        return false;
    }
    if (cut_erase) {
        return false;
    }
    memset(&ram[ram_index(page_start)], (int)ERASED, (size_t)FLASH_PAGE_BYTES);
    return true;
}

/**
 * @brief Programs 64-bit words into erased bytes of the boot-state area, as the chip does.
 *
 * The chip refuses an unaligned start, and a double word whose target is not erased
 * [RM0440 Rev 9, Sec. 5.3.7, p. 196-197]. A power cut (cut_after_words) stops after a number of
 * 32-bit words and reports a failure.
 *
 * @param[in]     start  chip address of the first double word, 8-byte aligned
 * @param[in]     words  32-bit words to program
 * @param[in]     pairs  number of double words
 * @return true if all words were programmed
 */
static bool ram_program(uint32_t start, const uint32_t *words, uint32_t pairs)
{
    uint32_t bytes = pairs * PAIR_BYTES;

    if (((start % PAIR_BYTES) != 0U) || (start < (uint32_t)BOOT_STATE_START) ||
        (bytes > STATE_AREA_BYTES) ||
        (start > (((uint32_t)BOOT_STATE_START + STATE_AREA_BYTES) - bytes))) {
        return false;
    }
    uint32_t first = ram_index(start);
    for (uint32_t i = 0U; i < bytes; i++) {
        if (ram[first + i] != ERASED) {
            return false;
        }
    }
    for (uint32_t w = 0U; w < (pairs * 2U); w++) {
        if (words_programmed >= cut_after_words) {
            return false;
        }
        memcpy(&ram[first + (w * WORD_BYTES)], &words[w], WORD_BYTES);
        words_programmed++;
    }
    return true;
}

/**
 * @brief Erases the whole RAM flash and removes every ECC error and power cut.
 */
static void ram_reset(void)
{
    memset(ram, (int)ERASED, sizeof ram);
    ecc_mask = 0U;
    ecc_seen = false;
    cut_erase = false;
    cut_after_words = RAM_CUT_NEVER;
    words_programmed = 0U;
}

/**
 * @brief Puts the bytes of a record into a stored copy, as raw flash content (no erase check).
 *
 * @param[in]     copy    0 or 1
 * @param[in]     record  record bytes
 */
static void ram_put(uint32_t copy, const boot_state_t *record)
{
    memcpy(&ram[ram_index(copy_address(copy))], record, sizeof(*record));
}

/**
 * @brief Copies the boot-state area (both copies) out of the RAM flash.
 *
 * @param[out]    snapshot  STATE_AREA_BYTES bytes
 */
static void ram_snapshot(uint8_t *snapshot)
{
    memcpy(snapshot, &ram[ram_index((uint32_t)BOOT_STATE_START)], STATE_AREA_BYTES);
}

/* The two sets of entry points over the one RAM flash (flash.h for common/boot_state.c,
 * bl_flash.h for bootloader/bl_boot_state.c). */

/**
 * @brief flash.h flash_erase_page over the RAM flash.
 *
 * @param[in]     page_start  page address
 * @return true if erased
 */
bool flash_erase_page(uint32_t page_start)
{
    return ram_erase(page_start);
}

/**
 * @brief flash.h flash_program over the RAM flash.
 *
 * @param[in]     start  first double word
 * @param[in]     words  32-bit words
 * @param[in]     pairs  number of double words
 * @return true if programmed
 */
bool flash_program(uint32_t start, const uint32_t *words, uint32_t pairs)
{
    return ram_program(start, words, pairs);
}

/**
 * @brief flash.h flash_ecc_clear over the RAM flash.
 */
void flash_ecc_clear(void)
{
    ecc_seen = false;
}

/**
 * @brief flash.h flash_ecc_error_seen over the RAM flash.
 *
 * @return true if a read since the clear met an ECC error
 */
bool flash_ecc_error_seen(void)
{
    return ecc_seen;
}

/**
 * @brief flash.h flash_at over the RAM flash.
 *
 * @param[in]     address  chip address
 * @return pointer to the byte
 */
const uint8_t *flash_at(uint32_t address)
{
    return ram_at(address);
}

/**
 * @brief bl_flash.h bl_flash_erase_page over the RAM flash.
 *
 * @param[in]     page_start  page address
 * @return true if erased
 */
bool bl_flash_erase_page(uint32_t page_start)
{
    return ram_erase(page_start);
}

/**
 * @brief bl_flash.h bl_flash_program over the RAM flash.
 *
 * @param[in]     start  first double word
 * @param[in]     words  32-bit words
 * @param[in]     pairs  number of double words
 * @return true if programmed
 */
bool bl_flash_program(uint32_t start, const uint32_t *words, uint32_t pairs)
{
    return ram_program(start, words, pairs);
}

/**
 * @brief bl_flash.h bl_flash_ecc_clear over the RAM flash.
 */
void bl_flash_ecc_clear(void)
{
    ecc_seen = false;
}

/**
 * @brief bl_flash.h bl_flash_ecc_error_seen over the RAM flash.
 *
 * @return true if a read since the clear met an ECC error
 */
bool bl_flash_ecc_error_seen(void)
{
    return ecc_seen;
}

/**
 * @brief bl_flash.h bl_flash_at over the RAM flash.
 *
 * @param[in]     address  chip address
 * @return pointer to the byte
 */
const uint8_t *bl_flash_at(uint32_t address)
{
    return ram_at(address);
}

/* ---- CRC-32 ------------------------------------------------------------------------- */

/* Reference values of zlib.crc32, the CRC-32 that tools/check_crc32.py compares crc32.c with
 * (Interfaces Sec. 4.0: polynomial 0x4C11DB7, start 0xFFFFFFFF, reflected, inverted). Computed
 * with Python 3 zlib.crc32 on 15 Oct 2026 for the inputs named in the table. */
typedef struct {
    const char *name;
    const uint8_t *data;
    uint32_t size;
    uint32_t zlib;
} crc_vector_t;

static const uint8_t VEC_A[1] = {0x61U};                        /* "a" */
static const uint8_t VEC_ZERO[1] = {0x00U};
static const uint8_t VEC_FF[1] = {0xFFU};
static const uint8_t VEC_DIGITS[9] = {0x31U, 0x32U, 0x33U, 0x34U, 0x35U,
                                      0x36U, 0x37U, 0x38U, 0x39U};   /* "123456789" */
#define ERASED_RUN_BYTES  64U    /* tools/check_crc32.py: "64 x 0xFF (erased flash)" */
#define PATTERN_STEP      167U   /* byte i of the pinned pattern: (i * 167 + 13) mod 256 */
#define PATTERN_OFFSET    13U

/**
 * @brief Compares bl_crc32_compute with crc32_compute and with pinned zlib values.
 *
 * Catches: the bootloader's CRC uses another polynomial, start value or final inversion (fails
 * at the first non-empty vector, "a"), reads bits most significant first (fails at "a"), or
 * returns the value of the wrong length (fails at the 40 KB pattern). It also fails if
 * common/crc32.c itself drifts from zlib.
 */
static void test_crc_pinned_vectors(void)
{
    static uint8_t counting[SLOT_CODES];
    static uint8_t erased_run[ERASED_RUN_BYTES];
    static uint8_t pattern[SLOT_BUFFER_BYTES];

    for (uint32_t i = 0U; i < SLOT_CODES; i++) {
        counting[i] = (uint8_t)i;
    }
    memset(erased_run, (int)ERASED, sizeof erased_run);
    for (uint32_t i = 0U; i < SLOT_BUFFER_BYTES; i++) {
        pattern[i] = (uint8_t)((i * PATTERN_STEP) + PATTERN_OFFSET);
    }
    const crc_vector_t vectors[] = {
        {"empty", VEC_A, 0U, 0x00000000U},
        {"'a'", VEC_A, 1U, 0xE8B7BE43U},
        {"one byte 0x00", VEC_ZERO, 1U, 0xD202EF8DU},
        {"one byte 0xFF", VEC_FF, 1U, 0xFF000000U},
        {"'123456789'", VEC_DIGITS, 9U, 0xCBF43926U},
        {"bytes 0..255", counting, SLOT_CODES, 0x29058C73U},
        {"64 x 0xFF", erased_run, ERASED_RUN_BYTES, 0x0F6187BAU},
        {"40 KB pattern", pattern, SLOT_BUFFER_BYTES, 0xBFE891C5U},
    };
    bool all = true;

    for (uint32_t v = 0U; v < (uint32_t)(sizeof vectors / sizeof vectors[0]); v++) {
        uint32_t common = crc32_compute(vectors[v].data, vectors[v].size);
        uint32_t boot = bl_crc32_compute(vectors[v].data, vectors[v].size);
        if ((common != vectors[v].zlib) || (boot != vectors[v].zlib)) {
            all = false;
            note_difference(vectors[v].name, v, common, boot);
        }
    }
    report(all, "crc32: empty, 1 byte, '123456789', 0..255, 64 x 0xFF and a 40 KB pattern equal "
                "zlib in both modules");
}

/**
 * @brief Compares the two CRC-32 modules on every length 0 to 300 at four buffer alignments.
 *
 * Catches: a length-dependent defect in the bootloader's CRC: an off-by-one in the loop (fails at
 * length 1 or 2), a word-at-a-time fast path with a tail bug (fails at a length that is not a
 * multiple of 4), an alignment assumption (fails at an odd start), a table with one wrong entry
 * (fails at the first length that uses it).
 */
static void test_crc_every_length(void)
{
    static uint8_t buffer[SWEEP_MAX_LENGTH + ALIGNMENTS];
    bool all = true;

    rng_fill(buffer, (uint32_t)sizeof buffer);
    for (uint32_t offset = 0U; offset < ALIGNMENTS; offset++) {
        for (uint32_t length = 0U; length <= SWEEP_MAX_LENGTH; length++) {
            uint32_t common = crc32_compute(&buffer[offset], length);
            uint32_t boot = bl_crc32_compute(&buffer[offset], length);
            if (common != boot) {
                all = false;
                note_difference("length/alignment", (offset * 1000U) + length, common, boot);
            }
        }
    }
    report(all, "crc32: every length 0..300 at 4 alignments, bootloader equals common");
}

/**
 * @brief Compares the two CRC-32 modules on large random buffers up to one slot.
 *
 * Catches: a defect that appears only in long input: a 16-bit length or counter in the
 * bootloader's CRC (fails at 65536 and above, here at 40 KB when it is 15-bit), a loop bound on a
 * constant instead of the size, a sum that overflows. The buffers are one slot long (40 KB, the
 * longest body the bootloader checks) and shorter by 1 to 3 bytes.
 */
static void test_crc_large_random(void)
{
    static uint8_t buffer[CRC_BUFFER_BYTES];
    const uint32_t sizes[] = {SLOT_BUFFER_BYTES, SLOT_BUFFER_BYTES - 1U, SLOT_BUFFER_BYTES - 3U,
                              4096U, 1025U, 1024U, 1023U, 257U};
    bool all = true;

    for (uint32_t round = 0U; round < 2U; round++) {
        rng_fill(buffer, (uint32_t)sizeof buffer);
        for (uint32_t s = 0U; s < (uint32_t)(sizeof sizes / sizeof sizes[0]); s++) {
            for (uint32_t offset = 0U; offset < 2U; offset++) {
                uint32_t common = crc32_compute(&buffer[offset], sizes[s]);
                uint32_t boot = bl_crc32_compute(&buffer[offset], sizes[s]);
                if (common != boot) {
                    all = false;
                    note_difference("large buffer", sizes[s], common, boot);
                }
            }
        }
    }
    report(all, "crc32: random buffers from 257 bytes to one 40 KB slot, bootloader equals common");
}

/**
 * @brief Compares the two CRC-32 modules on every 1-byte and every 2-byte input.
 *
 * Catches: a wrong table entry or a wrong bit step that only some byte values reach, and state
 * kept between calls (a static running value not reset): the 2-byte sweep calls the function
 * 65536 times in a row.
 */
static void test_crc_all_short_inputs(void)
{
    uint8_t two[2];
    bool all = true;

    for (uint32_t v = 0U; v < 65536U; v++) {
        two[0] = (uint8_t)(v & BYTE_MASK);
        two[1] = (uint8_t)(v >> BITS_PER_BYTE);
        uint32_t common = crc32_compute(two, 2U);
        uint32_t boot = bl_crc32_compute(two, 2U);
        bool one_ok = crc32_compute(two, 1U) == bl_crc32_compute(two, 1U);
        if ((common != boot) || !one_ok) {
            all = false;
            note_difference("two-byte input", v, common, boot);
        }
    }
    report(all, "crc32: all 256 one-byte and all 65536 two-byte inputs, bootloader equals common");
}

/* ---- Boot state: outcomes ----------------------------------------------------------------- */

/* What one call of a boot-state function did, to compare the two modules. */
typedef struct {
    bool faulted;            /* the fault reaction ran; nothing else below is compared */
    bool flag;               /* bool result of read and write */
    boot_state_load_t load;  /* result of load and classify */
    boot_state_t state;      /* *state after the call */
    uint8_t slot;            /* result of other_slot */
} outcome_t;

/**
 * @brief Starts a guarded call: clears the outcome and arms the fault stand-in.
 *
 * @param[out]    out  outcome to clear; its state is filled with the sentinel byte, so a
 *                     function that must leave *state alone is seen to do so
 */
static void outcome_begin(outcome_t *out)
{
    memset(out, 0, sizeof(*out));
    memset(&out->state, (int)SENTINEL, sizeof(out->state));
}

/**
 * @brief Returns true if two outcomes are the same: both faulted, or equal in every field.
 *
 * @param[in]     a  outcome of the common module
 * @param[in]     b  outcome of the bootloader's module
 * @return true if equal
 */
static bool outcome_same(const outcome_t *a, const outcome_t *b)
{
    if (a->faulted || b->faulted) {
        return a->faulted == b->faulted;
    }
    return (a->flag == b->flag) && (a->load == b->load) && (a->slot == b->slot) &&
           (memcmp(&a->state, &b->state, sizeof(a->state)) == 0);
}

/**
 * @brief Calls boot_state_classify or bl_boot_state_classify inside the fault guard.
 *
 * @param[in]     use_bl    true for the bootloader's module
 * @param[in]     copies    the two records as read
 * @param[in]     readable  false for a copy whose read saw an ECC error
 * @param[out]    out       what the call did
 */
static void run_classify(bool use_bl, const boot_state_t copies[2], const bool readable[2],
                         outcome_t *out)
{
    outcome_begin(out);
    fault_armed = true;
    if (setjmp(fault_jump) == 0) {
        out->load = use_bl ? bl_boot_state_classify(copies, readable, &out->state)
                           : boot_state_classify(copies, readable, &out->state);
        fault_armed = false;
    } else {
        out->faulted = true;
    }
}

/**
 * @brief Calls boot_state_load or bl_boot_state_load inside the fault guard.
 *
 * @param[in]     use_bl  true for the bootloader's module
 * @param[out]    out     what the call did
 */
static void run_load(bool use_bl, outcome_t *out)
{
    outcome_begin(out);
    fault_armed = true;
    if (setjmp(fault_jump) == 0) {
        out->load = use_bl ? bl_boot_state_load(&out->state) : boot_state_load(&out->state);
        fault_armed = false;
    } else {
        out->faulted = true;
    }
}

/**
 * @brief Calls boot_state_read or bl_boot_state_read inside the fault guard.
 *
 * @param[in]     use_bl  true for the bootloader's module
 * @param[out]    out     what the call did
 */
static void run_read(bool use_bl, outcome_t *out)
{
    outcome_begin(out);
    fault_armed = true;
    if (setjmp(fault_jump) == 0) {
        out->flag = use_bl ? bl_boot_state_read(&out->state) : boot_state_read(&out->state);
        fault_armed = false;
    } else {
        out->faulted = true;
    }
}

/**
 * @brief Calls boot_state_write or bl_boot_state_write inside the fault guard.
 *
 * @param[in]     use_bl  true for the bootloader's module
 * @param[in]     input   the record to write
 * @param[out]    out     what the call did; out->state is the record after the call
 */
static void run_write(bool use_bl, const boot_state_t *input, outcome_t *out)
{
    outcome_begin(out);
    out->state = *input;
    fault_armed = true;
    if (setjmp(fault_jump) == 0) {
        out->flag = use_bl ? bl_boot_state_write(&out->state) : boot_state_write(&out->state);
        fault_armed = false;
    } else {
        out->faulted = true;
    }
}

/**
 * @brief Calls boot_state_other_slot or bl_boot_state_other_slot inside the fault guard.
 *
 * @param[in]     use_bl  true for the bootloader's module
 * @param[in]     slot    slot code, any value 0 to 255
 * @param[out]    out     what the call did
 */
static void run_other_slot(bool use_bl, uint8_t slot, outcome_t *out)
{
    outcome_begin(out);
    fault_armed = true;
    if (setjmp(fault_jump) == 0) {
        out->slot = use_bl ? bl_boot_state_other_slot(slot) : boot_state_other_slot(slot);
        fault_armed = false;
    } else {
        out->faulted = true;
    }
}

/**
 * @brief Compares the two modules' outcomes and notes the first difference.
 *
 * @param[in]     what   what was compared
 * @param[in]     case_no  case number
 * @param[in]     a      outcome of the common module
 * @param[in]     b      outcome of the bootloader's module
 * @return true if the outcomes are the same
 */
static bool compare_outcomes(const char *what, uint32_t case_no, const outcome_t *a,
                             const outcome_t *b)
{
    if (outcome_same(a, b)) {
        return true;
    }
    if (detail[0] == '\0') {
        (void)snprintf(detail, sizeof detail,
                       "%s, case %u: common (fault %d, flag %d, load %d, slot %u, seq %u, "
                       "conf %u), bootloader (fault %d, flag %d, load %d, slot %u, seq %u, "
                       "conf %u)",
                       what, (unsigned)case_no, (int)a->faulted, (int)a->flag, (int)a->load,
                       (unsigned)a->slot, (unsigned)a->state.sequence, (unsigned)a->state.confirmed,
                       (int)b->faulted, (int)b->flag, (int)b->load, (unsigned)b->slot,
                       (unsigned)b->state.sequence, (unsigned)b->state.confirmed);
    }
    return false;
}

/* ---- Boot state: the catalogue of records -------------------------------------------- */

/* Record shapes the existing tests use (test_boot.c record_of, erased_record) and the cases of
 * decision D (blank, damaged), plus the half-written records a power cut leaves (boot_state.c:
 * two 64-bit flash words, programmed one after the other). */
#define CATALOGUE_SIZE 20U
static boot_state_t catalogue[CATALOGUE_SIZE];

/**
 * @brief Makes a record with a correct check value, computed by common/crc32.c.
 *
 * @param[in]     sequence   sequence number
 * @param[in]     confirmed  confirmed slot code
 * @param[in]     trial      trial slot code
 * @param[in]     starts     trial starts so far
 * @return the record; unused and reserved are 0
 */
static boot_state_t make_record(uint32_t sequence, uint8_t confirmed, uint8_t trial,
                                uint8_t starts)
{
    boot_state_t r;

    memset(&r, 0, sizeof r);
    r.sequence = sequence;
    r.confirmed = confirmed;
    r.trial = trial;
    r.failed_starts = starts;
    r.check = crc32_compute((const uint8_t *)&r, (uint32_t)offsetof(boot_state_t, check));
    return r;
}

/**
 * @brief Fills the catalogue of record shapes.
 *
 * Indexes: 0 valid seq 1; 1 valid seq 2; 2 valid on trial, 3 starts; 3 valid, sequence
 * 0xFFFFFFFF; 4 valid, sequence 0; 5 and 6 valid, same sequence 5, different content; 7 bad
 * check value; 8 all 0xFF; 9 all 0x00; 10-12 confirmed 0, 3, 0xFF with good check; 13 trial equal
 * to confirmed; 14 trial 3; 15-16 starts 4 and 255; 17 unused and reserved not 0 with a check
 * value that covers them; 18 first double word written, second erased; 19 first erased, second
 * written.
 */
static void build_catalogue(void)
{
    catalogue[0] = make_record(1U, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
    catalogue[1] = make_record(2U, BOOT_SLOT_B, BOOT_SLOT_NONE, 1U);
    catalogue[2] = make_record(2U, BOOT_SLOT_A, BOOT_SLOT_B, BOOT_STATE_MAX_STARTS);
    catalogue[3] = make_record(0xFFFFFFFFU, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
    catalogue[4] = make_record(0U, BOOT_SLOT_B, BOOT_SLOT_A, 2U);
    catalogue[5] = make_record(5U, BOOT_SLOT_B, BOOT_SLOT_A, 2U);
    catalogue[6] = make_record(5U, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
    catalogue[7] = make_record(6U, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
    catalogue[7].check ^= 1U;
    memset(&catalogue[8], (int)ERASED, sizeof(catalogue[8]));
    memset(&catalogue[9], 0, sizeof(catalogue[9]));
    catalogue[10] = make_record(7U, 0U, BOOT_SLOT_NONE, 0U);
    catalogue[11] = make_record(7U, 3U, BOOT_SLOT_NONE, 0U);
    catalogue[12] = make_record(7U, 0xFFU, BOOT_SLOT_NONE, 0U);
    catalogue[13] = make_record(7U, BOOT_SLOT_A, BOOT_SLOT_A, 0U);
    catalogue[14] = make_record(7U, BOOT_SLOT_A, 3U, 0U);
    catalogue[15] = make_record(7U, BOOT_SLOT_A, BOOT_SLOT_NONE, BOOT_STATE_MAX_STARTS + 1U);
    catalogue[16] = make_record(7U, BOOT_SLOT_A, BOOT_SLOT_NONE, 255U);
    catalogue[17] = make_record(8U, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
    catalogue[17].unused = 0x5AU;
    catalogue[17].reserved = 0x12345678U;
    catalogue[17].check =
        crc32_compute((const uint8_t *)&catalogue[17], (uint32_t)offsetof(boot_state_t, check));
    catalogue[18] = catalogue[1];
    memset(&((uint8_t *)&catalogue[18])[RECORD_HALF], (int)ERASED, RECORD_HALF);
    catalogue[19] = catalogue[1];
    memset(&catalogue[19], (int)ERASED, RECORD_HALF);
}

/* ---- Boot state: other_slot, classify, load, read ----------------------------------- */

/**
 * @brief Compares boot_state_other_slot on every byte value 0 to 255.
 *
 * Catches: the bootloader's other_slot answers a normal-looking value for a code that cannot
 * exist (the old "slot == A ? B : A" form, rule A6, or BOOT_SLOT_NONE) instead of the fault
 * reaction (fails at code 0); swaps A and B wrongly (fails at 1 or 2); faults on a valid code.
 */
static void test_other_slot_all_bytes(void)
{
    outcome_t common;
    outcome_t boot;
    bool all = true;
    uint32_t faults = 0U;

    for (uint32_t code = 0U; code < SLOT_CODES; code++) {
        run_other_slot(false, (uint8_t)code, &common);
        run_other_slot(true, (uint8_t)code, &boot);
        all = compare_outcomes("slot code", code, &common, &boot) && all;
        faults += common.faulted ? 1U : 0U;
    }
    report(all && (faults == (SLOT_CODES - 2U)),
           "boot state: other_slot for all codes 0..255: A<->B, every other code the fault "
           "reaction in both");
}

/**
 * @brief Compares boot_state_classify on every pair of catalogue records and every readable flag.
 *
 * Catches: a difference in the decision of decision D or in the pick of the newest copy: ties
 * (seq 5 twice, copy 0 must win), a sequence of 0xFFFFFFFF or 0 (a signed compare), a bad check
 * value accepted, a confirmed slot 0, 3 or 0xFF accepted or answered with the fault reaction
 * instead of DAMAGED (a value read from flash is damage, never a fault), a trial slot equal to
 * the confirmed one, a start count above the maximum, a blank pair that is DAMAGED, an unreadable
 * copy used, *state written for a BLANK or DAMAGED answer.
 */
static void test_classify_catalogue(void)
{
    outcome_t common;
    outcome_t boot;
    bool all = true;
    uint32_t case_no = 0U;

    for (uint32_t a = 0U; a < CATALOGUE_SIZE; a++) {
        for (uint32_t b = 0U; b < CATALOGUE_SIZE; b++) {
            for (uint32_t r = 0U; r < ECC_MASKS; r++) {
                const boot_state_t copies[2] = {catalogue[a], catalogue[b]};
                const bool readable[2] = {(r & 1U) == 0U, (r & 2U) == 0U};

                run_classify(false, copies, readable, &common);
                run_classify(true, copies, readable, &boot);
                all = compare_outcomes("classify", case_no, &common, &boot) && all;
                case_no++;
            }
        }
    }
    report(all, "boot state: classify equal on 20 x 20 record pairs x 4 readable combinations "
                "(valid, tie, bad CRC, blank, impossible fields)");
}

/**
 * @brief Places a catalogue pair in the RAM flash and compares load and read, with ECC errors.
 *
 * Catches: the bootloader's load reads another address (fails with any valid pair), reads only
 * one copy, ignores the ECC flag of one copy, does not clear the flag before reading (a stale
 * flag from a previous read makes a good copy unreadable), or its read() accepts a copy that
 * load() refuses.
 */
static void test_load_and_read_catalogue(void)
{
    outcome_t common;
    outcome_t boot;
    bool all = true;
    uint32_t case_no = 0U;

    for (uint32_t a = 0U; a < CATALOGUE_SIZE; a++) {
        for (uint32_t b = 0U; b < CATALOGUE_SIZE; b++) {
            for (uint32_t mask = 0U; mask < ECC_MASKS; mask++) {
                ram_reset();
                ram_put(0U, &catalogue[a]);
                ram_put(1U, &catalogue[b]);
                ecc_mask = mask;
                ecc_seen = true;   /* a stale flag: the read must clear it first */
                run_load(false, &common);
                run_load(true, &boot);
                all = compare_outcomes("load", case_no, &common, &boot) && all;
                run_read(false, &common);
                run_read(true, &boot);
                all = compare_outcomes("read", case_no, &common, &boot) && all;
                case_no++;
            }
        }
    }
    report(all, "boot state: load and read equal on the same 400 pairs in flash, with and without "
                "an ECC error in either copy");
}

/* ---- Boot state: write ------------------------------------------------------------------- */

/**
 * @brief Returns a record to write, with the fields the module sets filled with junk.
 *
 * The module must overwrite sequence, unused, reserved and check, so junk there shows whether
 * it does.
 *
 * @param[in]     confirmed  confirmed slot code
 * @param[in]     trial      trial slot code
 * @param[in]     starts     trial starts so far
 * @return the record to pass to a write
 */
static boot_state_t to_write(uint8_t confirmed, uint8_t trial, uint8_t starts)
{
    boot_state_t r;

    memset(&r, (int)SENTINEL, sizeof r);
    r.confirmed = confirmed;
    r.trial = trial;
    r.failed_starts = starts;
    return r;
}

/**
 * @brief A write from every catalogue pair as the starting flash, by each module, compared.
 *
 * The same starting flash is written by boot_state_write and by bl_boot_state_write, and the
 * result, the record handed back and the 8 KB of boot-state flash after the write must be the
 * same: the same copy chosen, the same sequence number, the same bytes.
 *
 * Catches: the bootloader writes the current copy instead of the other one (the newest copy is
 * lost on a power cut, REQ-017), numbers the sequence differently (a first write is 1, then one
 * above the newest valid copy), writes bytes in a different layout (the firmware would read a
 * different state than the bootloader wrote), skips the erase (the stand-in refuses to program
 * over written bytes), leaves junk in unused or reserved, returns true when the read-back failed.
 */
static void test_write_from_every_state(void)
{
    static uint8_t after_common[STATE_AREA_BYTES];
    static uint8_t after_boot[STATE_AREA_BYTES];
    outcome_t common;
    outcome_t boot;
    bool all = true;
    uint32_t case_no = 0U;
    const boot_state_t input = to_write(BOOT_SLOT_A, BOOT_SLOT_B, 1U);

    for (uint32_t a = 0U; a < CATALOGUE_SIZE; a++) {
        for (uint32_t b = 0U; b < CATALOGUE_SIZE; b++) {
            for (uint32_t mask = 0U; mask < ECC_MASKS; mask++) {
                ram_reset();
                ram_put(0U, &catalogue[a]);
                ram_put(1U, &catalogue[b]);
                ecc_mask = mask;
                run_write(false, &input, &common);
                ram_snapshot(after_common);

                ram_reset();
                ram_put(0U, &catalogue[a]);
                ram_put(1U, &catalogue[b]);
                ecc_mask = mask;
                run_write(true, &input, &boot);
                ram_snapshot(after_boot);

                all = compare_outcomes("write", case_no, &common, &boot) && all;
                if (memcmp(after_common, after_boot, STATE_AREA_BYTES) != 0) {
                    all = false;
                    note_difference("flash bytes after write", case_no, 0U, 1U);
                }
                case_no++;
            }
        }
    }
    report(all, "boot state: a write from each of 1600 starting flashes leaves the same bytes, "
                "the same record and the same answer in both modules");
}

/**
 * @brief Writes six states in a row, each by common or by the bootloader, in all 64 orders.
 *
 * Every order is compared against the all-common run: the 8 KB of flash after each step must be
 * identical, and both modules must read the state back as written, with sequence numbers 1 to 6.
 * A record written by one module is therefore read the same way by the other, whichever wrote
 * the one before it.
 *
 * Catches: a format difference that shows only when the other module wrote the previous record
 * (a sequence number taken from a copy the other module wrote, a CRC over other bytes, a
 * different field order or byte order): the 64 orders include every change of writer.
 */
static void test_write_orders_cross_read(void)
{
    static uint8_t reference[WRITE_STEPS][STATE_AREA_BYTES];
    static uint8_t now[STATE_AREA_BYTES];
    const boot_state_t steps[WRITE_STEPS] = {
        {0U, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U, 0U, 0U, 0U},
        {0U, BOOT_SLOT_A, BOOT_SLOT_B, 0U, 0U, 0U, 0U},
        {0U, BOOT_SLOT_A, BOOT_SLOT_B, 1U, 0U, 0U, 0U},
        {0U, BOOT_SLOT_A, BOOT_SLOT_B, 2U, 0U, 0U, 0U},
        {0U, BOOT_SLOT_B, BOOT_SLOT_NONE, 0U, 0U, 0U, 0U},
        {0U, BOOT_SLOT_B, BOOT_SLOT_A, BOOT_STATE_MAX_STARTS, 0U, 0U, 0U},
    };
    outcome_t out;
    bool all = true;

    ram_reset();
    for (uint32_t k = 0U; k < WRITE_STEPS; k++) {
        run_write(false, &steps[k], &out);
        ram_snapshot(reference[k]);
        all = all && !out.faulted && out.flag && (out.state.sequence == (k + 1U));
    }
    for (uint32_t pattern = 0U; pattern < WRITE_PATTERNS; pattern++) {
        ram_reset();
        for (uint32_t k = 0U; k < WRITE_STEPS; k++) {
            outcome_t read_common;
            outcome_t read_boot;
            bool by_boot = ((pattern >> k) & 1U) != 0U;

            run_write(by_boot, &steps[k], &out);
            ram_snapshot(now);
            run_read(false, &read_common);
            run_read(true, &read_boot);
            bool same = (memcmp(now, reference[k], STATE_AREA_BYTES) == 0) && out.flag &&
                        read_common.flag && read_boot.flag &&
                        (memcmp(&read_common.state, &out.state, sizeof out.state) == 0) &&
                        (memcmp(&read_boot.state, &out.state, sizeof out.state) == 0) &&
                        (out.state.sequence == (k + 1U));
            if (!same) {
                all = false;
                note_difference("writers pattern / step", (pattern * 10U) + k, pattern, k);
            }
        }
    }
    report(all, "boot state: 6 writes by any mix of the two modules (64 orders): same flash bytes "
                "as the all-common run, each record read back the same by both readers");
}

/**
 * @brief Both modules refuse a record with impossible fields, and touch no flash.
 *
 * Catches: the bootloader's write stores a record the other side would read as damaged (confirmed
 * 0, 3 or 0xFF; a trial equal to the confirmed slot or 3; a start count of 4 or 255) or asks
 * other_slot about a bad code and takes the fault reaction (a record handed to write is a value
 * from the caller, checked before any flash is read; rule B1 is for values in memory, and the
 * common module answers false); reads or erases flash before it refuses.
 */
static void test_write_refused_fields(void)
{
    static uint8_t before[STATE_AREA_BYTES];
    static uint8_t after[STATE_AREA_BYTES];
    const boot_state_t bad[] = {
        {0U, 0U, BOOT_SLOT_NONE, 0U, 0U, 0U, 0U},
        {0U, 3U, BOOT_SLOT_NONE, 0U, 0U, 0U, 0U},
        {0U, 0xFFU, BOOT_SLOT_NONE, 0U, 0U, 0U, 0U},
        {0U, BOOT_SLOT_A, BOOT_SLOT_A, 0U, 0U, 0U, 0U},
        {0U, BOOT_SLOT_B, BOOT_SLOT_B, 0U, 0U, 0U, 0U},
        {0U, BOOT_SLOT_A, 3U, 0U, 0U, 0U, 0U},
        {0U, BOOT_SLOT_A, BOOT_SLOT_NONE, BOOT_STATE_MAX_STARTS + 1U, 0U, 0U, 0U},
        {0U, BOOT_SLOT_B, BOOT_SLOT_A, 255U, 0U, 0U, 0U},
    };
    outcome_t common;
    outcome_t boot;
    bool all = true;

    for (uint32_t i = 0U; i < (uint32_t)(sizeof bad / sizeof bad[0]); i++) {
        ram_reset();
        ram_put(0U, &catalogue[0]);
        ram_put(1U, &catalogue[1]);
        ram_snapshot(before);
        run_write(false, &bad[i], &common);
        run_write(true, &bad[i], &boot);
        ram_snapshot(after);
        all = compare_outcomes("refused record", i, &common, &boot) && all;
        all = all && !common.faulted && !common.flag && !boot.faulted && !boot.flag &&
              (memcmp(before, after, STATE_AREA_BYTES) == 0);
    }
    report(all, "boot state: records with impossible fields refused (false, no fault, flash "
                "untouched) by both modules");
}

/**
 * @brief A power cut at every step of a write, by each module, then read by both.
 *
 * The cut points are: the erase does nothing; the program stops after 0, 1, 2 and 3 of the 4
 * 32-bit words. The starting flash is blank, or holds one valid record (sequence 3). After the cut
 * both readers must give the same answer. With a valid record before the cut, that record must
 * still be the answer (REQ-017: the previous state stays readable), and the write returns false.
 *
 * Catches: the bootloader writes into the current copy (the old state is lost at the first cut
 * point that follows the erase), programs the check value before the data (a cut leaves a record
 * that looks complete: here a different answer between the readers or a lost old state), returns
 * true after a failed program, or reads a half-written record differently from common.
 */
static void test_write_power_cut(void)
{
    const uint32_t cuts = 6U;   /* erase cut, then program cut after 0, 1, 2, 3 words, then none */
    const boot_state_t old_record = make_record(3U, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
    const boot_state_t input = to_write(BOOT_SLOT_A, BOOT_SLOT_B, 0U);
    outcome_t out;
    outcome_t read_common;
    outcome_t read_boot;
    bool all = true;
    uint32_t case_no = 0U;

    for (uint32_t writer = 0U; writer < 2U; writer++) {
        for (uint32_t had_old = 0U; had_old < 2U; had_old++) {
            for (uint32_t cut = 0U; cut < cuts; cut++) {
                ram_reset();
                if (had_old != 0U) {
                    ram_put(0U, &old_record);
                }
                cut_erase = (cut == 0U);
                cut_after_words = (cut >= 1U) && (cut < (cuts - 1U)) ? (cut - 1U) : RAM_CUT_NEVER;
                run_write(writer != 0U, &input, &out);
                run_load(false, &read_common);
                run_load(true, &read_boot);
                bool cut_happened = (cut < (cuts - 1U));
                all = compare_outcomes("after a cut", case_no, &read_common, &read_boot) && all;
                all = all && !out.faulted && (out.flag == !cut_happened);
                if ((had_old != 0U) && cut_happened) {
                    all = all && (read_common.load == BOOT_STATE_VALID) &&
                          (memcmp(&read_common.state, &old_record, sizeof old_record) == 0);
                }
                case_no++;
            }
        }
    }
    report(all, "boot state: power cut at the erase and after 0..3 programmed words: both readers "
                "agree, the old record stays readable, write answers false");
}

/**
 * @brief One flipped bit in each byte of each stored record, read by both modules.
 *
 * Two records are written by each module (sequence 1 in copy 0, sequence 2 in copy 1). One bit
 * of each of the 16 bytes of each copy is flipped (bit 0 and bit 7), and load and read of both
 * modules must agree.
 *
 * Catches: a bootloader check value that does not cover some byte of the record (a flip in that
 * byte is accepted), a CRC over the wrong length (12 bytes are covered: the last 4 are the check
 * itself), a fallback to the older copy that the common module does and the bootloader does not.
 */
static void test_flipped_byte_in_stored_record(void)
{
    const boot_state_t first = to_write(BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
    const boot_state_t second = to_write(BOOT_SLOT_A, BOOT_SLOT_B, 1U);
    const uint8_t masks[2] = {0x01U, 0x80U};
    outcome_t out;
    outcome_t common;
    outcome_t boot;
    bool all = true;
    uint32_t case_no = 0U;

    for (uint32_t writer = 0U; writer < 2U; writer++) {
        for (uint32_t copy = 0U; copy < COPIES; copy++) {
            for (uint32_t byte = 0U; byte < RECORD_BYTES; byte++) {
                for (uint32_t m = 0U; m < 2U; m++) {
                    ram_reset();
                    run_write(writer != 0U, &first, &out);
                    run_write(writer != 0U, &second, &out);
                    uint32_t at = ram_index(copy_address(copy)) + byte;
                    ram[at] = (uint8_t)(ram[at] ^ masks[m]);
                    run_load(false, &common);
                    run_load(true, &boot);
                    all = compare_outcomes("flipped load", case_no, &common, &boot) && all;
                    run_read(false, &common);
                    run_read(true, &boot);
                    all = compare_outcomes("flipped read", case_no, &common, &boot) && all;
                    case_no++;
                }
            }
        }
    }
    report(all, "boot state: one flipped bit in each byte of each copy, written by either module: "
                "load and read equal");
}

/* ---- Image check -------------------------------------------------------------------------- */

typedef struct {
    uint32_t start;   /* chip address of the region */
    uint32_t bytes;   /* size of the region */
} region_t;

#define REGION_COUNT 3U
static const region_t REGIONS[REGION_COUNT] = {
    {(uint32_t)SLOT_A_START, (uint32_t)SLOT_BYTES},
    {(uint32_t)SLOT_B_START, (uint32_t)SLOT_BYTES},
    {(uint32_t)LAST_RESORT_START, (uint32_t)LAST_RESORT_BYTES},
};
/* Image kinds tried in every region: all three of image_format.h, and one that exists nowhere. */
#define KIND_COUNT   4U
#define UNKNOWN_KIND 0xEEU
static const uint8_t KINDS[KIND_COUNT] = {IMAGE_KIND_BOOTLOADER, IMAGE_KIND_MAIN,
                                          IMAGE_KIND_LAST_RESORT, UNKNOWN_KIND};
/* Highest image_result_t value today (image_check.h). Values above it must fault in both. */
#define RESULT_LAST      IMAGE_BAD_SIGNATURE_B
#define RESULT_COUNT     ((uint32_t)RESULT_LAST + 1U)
#define RESULT_OUTSIDE   5U      /* how many values past the last are tried */
#define RESULT_FAR       0x7FFFFFFF
/* Longest name and the most files the image sweep handles (the build makes about 15). */
#define PATH_BYTES       512U
#define MAX_IMAGES       32U
#define IMAGE_BUFFER     ((uint32_t)SLOT_BYTES)
/* Header bytes whose flip is tried one by one: the whole header (layout.h). */
#define HEADER_BYTES     ((uint32_t)IMAGE_HEADER_BYTES)
#define SIGNATURE_BYTES  ((uint32_t)IMAGE_SIGNATURE_BYTES)
#define FRAME_BYTES      ((uint32_t)IMAGE_HEADER_BYTES + (uint32_t)IMAGE_SIGNATURES_BYTES)
/* Smallest body that holds the zero fill and two vector-table words (image_check.c). */
#define MIN_BODY_BYTES   ((uint32_t)IMAGE_CODE_OFFSET - HEADER_BYTES + (2U * WORD_BYTES))
#define SIGNED_UNSET     0xDEADBEEFU

static uint32_t result_seen[RESULT_COUNT];
static uint32_t contents_mismatches;
static uint32_t signature_mismatches;
static uint32_t cases_compared;
static uint32_t signatures_compared;

/**
 * @brief Runs both image_check_contents (and, when it passes, both image_check_signatures) on one
 * image in one region, and counts any difference.
 *
 * @param[in]     image  the region's bytes (region_bytes long)
 * @param[in]     where  region the image sits in
 * @param[in]     kind   the kind that region may run
 * @return the result of common/image_check_contents
 */
static image_result_t compare_image(const uint8_t *image, const region_t *where, uint8_t kind)
{
    uint32_t signed_common = SIGNED_UNSET;
    uint32_t signed_boot = SIGNED_UNSET;
    image_result_t common =
        image_check_contents(image, where->start, where->bytes, kind, &signed_common);
    image_result_t boot =
        bl_image_check_contents(image, where->start, where->bytes, kind, &signed_boot);

    cases_compared++;
    result_seen[common]++;
    if ((common != boot) || (signed_common != signed_boot)) {
        contents_mismatches++;
        note_difference("image contents", cases_compared, (uint32_t)common, (uint32_t)boot);
    }
    if (common == IMAGE_OK) {
        image_result_t sig_common = image_check_signatures(image, signed_common);
        image_result_t sig_boot = bl_image_check_signatures(image, signed_common);

        signatures_compared++;
        result_seen[sig_common]++;
        if (sig_common != sig_boot) {
            signature_mismatches++;
            note_difference("image signatures", signatures_compared, (uint32_t)sig_common,
                            (uint32_t)sig_boot);
        }
    }
    return common;
}

/**
 * @brief Changes one byte of a region copy, compares the two checks, and puts the byte back.
 *
 * @param[in,out] image   the region's bytes
 * @param[in]     where   region the image sits in
 * @param[in]     kind    the kind that region may run
 * @param[in]     offset  byte to change
 * @param[in]     value   new value
 */
static void try_byte(uint8_t *image, const region_t *where, uint8_t kind, uint32_t offset,
                     uint8_t value)
{
    uint8_t old = image[offset];

    image[offset] = value;
    (void)compare_image(image, where, kind);
    image[offset] = old;
}

/**
 * @brief Changes a little-endian 32-bit field of a region copy, compares, and puts it back.
 *
 * @param[in,out] image   the region's bytes
 * @param[in]     where   region the image sits in
 * @param[in]     kind    the kind that region may run
 * @param[in]     offset  first byte of the field
 * @param[in]     value   new value of the field
 */
static void try_field(uint8_t *image, const region_t *where, uint8_t kind, uint32_t offset,
                      uint32_t value)
{
    uint8_t old[WORD_BYTES];

    memcpy(old, &image[offset], WORD_BYTES);
    for (uint32_t i = 0U; i < WORD_BYTES; i++) {
        image[offset + i] = (uint8_t)((value >> (i * BITS_PER_BYTE)) & BYTE_MASK);
    }
    (void)compare_image(image, where, kind);
    memcpy(&image[offset], old, WORD_BYTES);
}

/**
 * @brief Compares the two checks on damaged copies of an image that passes the contents check.
 *
 * Damage tried: every header byte flipped (64), the length field at its limits, the reset
 * handler at both edges of the region's code, a body byte at the start, middle and end, the first
 * and last byte of each signature, and an erased first word.
 *
 * @param[in,out] image  the region's bytes, an image that passes; restored before return
 * @param[in]     where  region the image sits in
 * @param[in]     kind   the kind that region may run
 */
static void mutate_image(uint8_t *image, const region_t *where, uint8_t kind)
{
    uint32_t body = image_read_u32(&image[IMAGE_OFFSET_LENGTH]);
    uint32_t code_start = where->start + (uint32_t)IMAGE_CODE_OFFSET;
    uint32_t code_end = where->start + HEADER_BYTES + body;
    uint32_t signed_bytes = HEADER_BYTES + body;
    const uint32_t lengths[] = {0U, MIN_BODY_BYTES - 1U, MIN_BODY_BYTES, body - 1U, body + 1U,
                                where->bytes - FRAME_BYTES, where->bytes - FRAME_BYTES + 1U,
                                0x7FFFFFFFU, 0xFFFFFFFEU, 0xFFFFFFFFU};
    const uint32_t handlers[] = {code_start - 1U, code_start, code_start + 1U, code_end - 1U,
                                 code_end, 0U, 0xFFFFFFFFU};
    const uint32_t body_offsets[] = {HEADER_BYTES, HEADER_BYTES + (body / 2U),
                                     HEADER_BYTES + body - 1U};
    const uint32_t signature_offsets[] = {signed_bytes, signed_bytes + SIGNATURE_BYTES - 1U,
                                          signed_bytes + SIGNATURE_BYTES,
                                          signed_bytes + (2U * SIGNATURE_BYTES) - 1U};

    for (uint32_t off = 0U; off < HEADER_BYTES; off++) {
        try_byte(image, where, kind, off, (uint8_t)(image[off] ^ BYTE_MASK));
    }
    for (uint32_t i = 0U; i < (uint32_t)(sizeof lengths / sizeof lengths[0]); i++) {
        try_field(image, where, kind, IMAGE_OFFSET_LENGTH, lengths[i]);
    }
    for (uint32_t i = 0U; i < (uint32_t)(sizeof handlers / sizeof handlers[0]); i++) {
        try_field(image, where, kind, (uint32_t)IMAGE_CODE_OFFSET + WORD_BYTES, handlers[i]);
    }
    for (uint32_t i = 0U; i < (uint32_t)(sizeof body_offsets / sizeof body_offsets[0]); i++) {
        try_byte(image, where, kind, body_offsets[i], (uint8_t)(image[body_offsets[i]] ^ 1U));
    }
    for (uint32_t i = 0U; i < (uint32_t)(sizeof signature_offsets / sizeof signature_offsets[0]);
         i++) {
        try_byte(image, where, kind, signature_offsets[i],
                 (uint8_t)(image[signature_offsets[i]] ^ 1U));
    }
    try_field(image, where, kind, 0U, 0xFFFFFFFFU);
}

/**
 * @brief Reads a file into a buffer.
 *
 * @param[in]     path      file to read
 * @param[out]    buffer    bytes read
 * @param[in]     capacity  size of the buffer
 * @return number of bytes read, 0 if the file cannot be opened
 */
static uint32_t read_file(const char *path, uint8_t *buffer, uint32_t capacity)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL) {
        return 0U;
    }
    size_t got = fread(buffer, 1U, capacity, f);
    (void)fclose(f);
    return (uint32_t)got;
}

/**
 * @brief Collects the paths of the .img files of a directory.
 *
 * @param[in]     directory  directory to list
 * @param[in,out] paths      table of paths, appended to
 * @param[in]     used       paths already in the table
 * @return number of paths in the table afterwards
 */
static uint32_t collect_images(const char *directory, char paths[][PATH_BYTES], uint32_t used)
{
    DIR *dir = opendir(directory);
    uint32_t count = used;

    if (dir == NULL) {
        return count;
    }
    for (struct dirent *entry = readdir(dir); entry != NULL; entry = readdir(dir)) {
        size_t n = strlen(entry->d_name);
        if ((n > 4U) && (strcmp(&entry->d_name[n - 4U], ".img") == 0) && (count < MAX_IMAGES)) {
            (void)snprintf(paths[count], PATH_BYTES, "%s/%s", directory, entry->d_name);
            count++;
        }
    }
    (void)closedir(dir);
    return count;
}

/**
 * @brief Both image checks on every signed image the build makes, in every region, with every
 * kind, and on damaged copies of the ones that pass.
 *
 * Reports three results: the contents checks agree, the signature checks agree, and the sweep
 * reached every one of the 11 results (a sweep that never reaches a result proves nothing about
 * it, and an empty image directory would pass vacuously otherwise).
 *
 * Catches (contents): the bootloader accepts a wrong proprietary string, format, kind or target;
 * computes the length bound differently (an off-by-one at the region size, or at the minimum body);
 * accepts a reset handler on the edge of the region; checks the body check value over another
 * range (fails on any image, then on the body flips); returns the checks in another order (a
 * header flip that breaks two checks gives another result); leaves *signed_bytes different.
 * Catches (signatures): the bootloader checks the wrong key, signs over another length, checks
 * signature B before A, or accepts a signature that common refuses (the wrong-key images).
 *
 * @param[in]     dir_firmware  directory of the firmware's signed images
 * @param[in]     dir_test      directory of the bench's signed test images
 */
static void test_image_sweep(const char *dir_firmware, const char *dir_test)
{
    static char paths[MAX_IMAGES][PATH_BYTES];
    static uint8_t file[IMAGE_BUFFER];
    static uint8_t region_copy[IMAGE_BUFFER];
    uint32_t count = collect_images(dir_firmware, paths, 0U);

    count = collect_images(dir_test, paths, count);
    for (uint32_t i = 0U; i < count; i++) {
        uint32_t size = read_file(paths[i], file, IMAGE_BUFFER);

        for (uint32_t r = 0U; r < REGION_COUNT; r++) {
            for (uint32_t k = 0U; k < KIND_COUNT; k++) {
                memset(region_copy, (int)ERASED, REGIONS[r].bytes);
                memcpy(region_copy, file, (size < REGIONS[r].bytes) ? size : REGIONS[r].bytes);
                if (compare_image(region_copy, &REGIONS[r], KINDS[k]) == IMAGE_OK) {
                    mutate_image(region_copy, &REGIONS[r], KINDS[k]);
                }
            }
        }
    }
    memset(region_copy, (int)ERASED, IMAGE_BUFFER);
    (void)compare_image(region_copy, &REGIONS[0], IMAGE_KIND_MAIN);
    memset(region_copy, 0, IMAGE_BUFFER);
    (void)compare_image(region_copy, &REGIONS[0], IMAGE_KIND_MAIN);

    bool all_reached = true;
    for (uint32_t v = 0U; v < RESULT_COUNT; v++) {
        if (result_seen[v] == 0U) {
            all_reached = false;
            note_difference("result never reached by the sweep", v, v, v);
        }
    }
    printf("      (%u image files, %u contents cases, %u signature cases)\n", (unsigned)count,
           (unsigned)cases_compared, (unsigned)signatures_compared);
    report(contents_mismatches == 0U, "image check: contents result and signed length equal on "
                                      "every build image x 3 regions x 4 kinds, and damaged "
                                      "copies of them");
    report(signature_mismatches == 0U, "image check: signature result equal on every image that "
                                       "passes the contents check, wrong-key images included");
    report((count > 0U) && all_reached,
           "image check: the sweep reached all 11 results (OK, EMPTY, MAGIC, FORMAT, KIND, TARGET, "
           "LENGTH, START, BODY, SIG A, SIG B)");
}

/**
 * @brief Runs image_result_text of one module inside the fault guard.
 *
 * @param[in]     use_bl  true for the bootloader's module
 * @param[in]     value   a result value, possibly outside the enum
 * @param[out]    faulted true if the fault reaction ran
 * @return the text, or NULL if the fault reaction ran
 */
static const char *run_result_text(bool use_bl, int value, bool *faulted)
{
    const char *text = NULL;

    *faulted = false;
    fault_armed = true;
    if (setjmp(fault_jump) == 0) {
        text = use_bl ? bl_image_result_text((image_result_t)value)
                      : image_result_text((image_result_t)value);
        fault_armed = false;
    } else {
        *faulted = true;
    }
    return text;
}

/**
 * @brief Compares the result texts of the two modules for every result value and for values
 * outside the enum.
 *
 * The texts go to the UART log and to the ground (check codes are separate); they are compared as
 * text. A value outside image_result_t is a corrupted variable: both modules must take the fault
 * reaction, as the existing tests require of the common one.
 *
 * Catches: a text that differs by a character in the bootloader's table (fails at that result),
 * two results with swapped texts, a table that is one entry short (the last result falls through
 * to the fault reaction), "unknown" or an empty text for an outside value instead of the fault
 * reaction (rule A6).
 */
static void test_result_text(void)
{
    bool all = true;
    bool fault_common = false;
    bool fault_boot = false;

    for (uint32_t v = 0U; v < (RESULT_COUNT + RESULT_OUTSIDE); v++) {
        const char *c = run_result_text(false, (int)v, &fault_common);
        const char *b = run_result_text(true, (int)v, &fault_boot);
        bool same = (fault_common == fault_boot) &&
                    (fault_common || ((c != NULL) && (b != NULL) && (strcmp(c, b) == 0)));
        if (!same || (fault_common != (v >= RESULT_COUNT))) {
            all = false;
            if (detail[0] == '\0') {
                (void)snprintf(detail, sizeof detail,
                               "result value %u: common (fault %d, \"%s\"), bootloader "
                               "(fault %d, \"%s\")", (unsigned)v, (int)fault_common,
                               (c != NULL) ? c : "-", (int)fault_boot, (b != NULL) ? b : "-");
            }
        }
    }
    const char *c = run_result_text(false, RESULT_FAR, &fault_common);
    const char *b = run_result_text(true, RESULT_FAR, &fault_boot);
    (void)c;
    (void)b;
    all = all && fault_common && fault_boot;
    report(all, "image check: result text equal for all 11 results; 11..15 and 0x7FFFFFFF take the "
                "fault reaction in both");
}

/**
 * @brief Compares image_read_u32 on chosen 4-byte patterns, every window of a random buffer, and
 * at four alignments.
 *
 * Catches: big-endian reads, a sign extension (0x80 in any byte), a shift by a promoted byte that
 * overflows (rule B5: the byte 0x80 shifted 24 left must stay unsigned), a read of the wrong byte
 * order in the middle bytes, an aligned 32-bit load that fails or differs at an odd address.
 */
static void test_read_u32(void)
{
    static uint8_t buffer[WORD_BYTES + 64U + ALIGNMENTS];
    const uint8_t patterns[][WORD_BYTES] = {
        {0x00U, 0x00U, 0x00U, 0x00U}, {0xFFU, 0xFFU, 0xFFU, 0xFFU}, {0x01U, 0x02U, 0x03U, 0x04U},
        {0x04U, 0x03U, 0x02U, 0x01U}, {0x80U, 0x00U, 0x00U, 0x00U}, {0x00U, 0x80U, 0x00U, 0x00U},
        {0x00U, 0x00U, 0x80U, 0x00U}, {0x00U, 0x00U, 0x00U, 0x80U}, {0x46U, 0x53U, 0x49U, 0x52U},
        {0xFFU, 0x00U, 0xFFU, 0x00U},
    };
    bool all = true;

    for (uint32_t p = 0U; p < (uint32_t)(sizeof patterns / sizeof patterns[0]); p++) {
        uint32_t common = image_read_u32(patterns[p]);
        uint32_t boot = bl_image_read_u32(patterns[p]);
        if (common != boot) {
            all = false;
            note_difference("pattern", p, common, boot);
        }
    }
    rng_fill(buffer, (uint32_t)sizeof buffer);
    for (uint32_t w = 0U; w < (uint32_t)(sizeof buffer - WORD_BYTES); w++) {
        uint32_t common = image_read_u32(&buffer[w]);
        uint32_t boot = bl_image_read_u32(&buffer[w]);
        if (common != boot) {
            all = false;
            note_difference("window", w, common, boot);
        }
    }
    report(all, "image check: read_u32 equal on 10 chosen patterns and every random window");
}

/**
 * @brief Runs the tests and prints the totals.
 *
 * @param[in]     argc  argument count; 3 expected
 * @param[in]     argv  program name, the firmware image directory, the test image directory
 * @return 0 if every test passed, 1 if one failed, 2 on a wrong command line
 */
int main(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: %s <firmware image directory> <test image directory>\n", argv[0]);
        return 2;
    }
    build_catalogue();
    test_crc_pinned_vectors();
    test_crc_every_length();
    test_crc_large_random();
    test_crc_all_short_inputs();
    test_other_slot_all_bytes();
    test_classify_catalogue();
    test_load_and_read_catalogue();
    test_write_from_every_state();
    test_write_orders_cross_read();
    test_write_refused_fields();
    test_write_power_cut();
    test_flipped_byte_in_stored_record();
    test_result_text();
    test_read_u32();
    test_image_sweep(argv[1], argv[2]);
    if (failures == 0U) {
        printf("separation unit tests: all %u passed\n", tests);
        return 0;
    }
    printf("separation unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
