/*
 * powercut_bootstate.c — host stress test of the controller's boot state through a power cut at
 * every step of a write (stress audit, Part A.3; code rule B7, REQ-017).
 *
 * What it is: one program that links the firmware's writer and reader (common/boot_state.c) and
 * the bootloader's own writer and reader (bootloader/bl_boot_state.c) against one fake flash of
 * its own. The fake records every erase and every doubleword program the writers make, and it can
 * stop the power at any one of those steps. It does not use the flash stubs of test/controller/
 * unit, so the cut model here is independent of the unit tests' model.
 *
 * Why it is like this: the boot state decides which firmware runs. A cut in the middle of a write
 * must leave the old state or the new state, never a third one (REQ-017, rule B7: two copies,
 * write the older, data then CRC). The cut model is the brief's: a step is not done, is done, or
 * (for a page erase) half done. Two additions: a program step can leave a torn doubleword (one
 * word programmed), with or without an ECC error on read (flash.h: a cut during programming can
 * leave a word with two wrong bits, RM0440 Rev 9, Sec. 5.3.2, p. 190); and a random mix.
 *
 * Phases (each prints its own table):
 *   1. checker self-test: the checker must call a forged third state, a lost state and a
 *      disagreement by their right names, or the later phases prove nothing (rule E1);
 *   2. matrix: every boot-state write the code makes, written by both writers, from both copy
 *      parities, cut at every step with every variant, read by both readers;
 *   3. random: SEQUENCE_COUNT sequences of WRITES_PER_SEQUENCE random writes with random cuts;
 *   4. first write from blank flash with a cut (observation, not an invariant, see the log);
 *   5. sequence-number wrap (observation, not an invariant, see the log).
 *
 * Built with the Mac's cc, -std=c11, the board's warning set and the undefined-behaviour
 * sanitizer (rule E5). Usage: powercut_bootstate [--case S W P K C] [--seq N]. No argument runs
 * everything; --case replays one matrix case and --seq one random sequence, with a trace.
 * Exit status 1 if an invariant failed.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bl_boot_state.h"
#include "boot_state.h"
#include "crc32.h"
#include "fault.h"
#include "flash.h"
#include "layout.h"

/* ---- Constants ------------------------------------------------------------------------- */

/* Seed of the random numbers: fixed, so a failure can be repeated (as test_separation.c, whose
 * value this is: 15 Oct 2026, the date of owner's decision 35). */
#define RNG_SEED          0x20261015U
/* Marsaglia xorshift32 shifts [G. Marsaglia, "Xorshift RNGs", J. Stat. Softw. 8(14), 2003,
 * p. 3, the 13-17-5 triple]. Any generator would do: the bytes only have to be uneven. */
#define RNG_SHIFT_A       13U
#define RNG_SHIFT_B       17U
#define RNG_SHIFT_C       5U
/* Spreads the sequence number into the seed of one random sequence (Knuth, TAOCP vol. 3,
 * Sec. 6.4, multiplicative hashing constant 2654435761). No other reason for the value. */
#define SEED_SPREAD       2654435761U

/* Number of random sequences, and writes in each: the brief asks for 10^4 sequences. 16 writes
 * is long enough for both copies to alternate several times; no other reason for the value. */
#define SEQUENCE_COUNT    10000U
#define WRITES_PER_SEQ    16U
/* Share of random writes that are cut, in percent. No technical reason: it makes runs of two or
 * three cuts in a row common (36 % for two), which is the "repeated cuts" case. */
#define CUT_PERCENT       60U
#define PERCENT           100U
/* First writes from blank flash that are cut (phase 4). */
#define BLANK_CUT_COUNT   2000U
/* Chance in 2 that a random torn doubleword raises an ECC error when read. */
#define RANDOM_CHOICES    2U

/* Boot state window: both copies, 4 KB each (layout.h, BOOT_STATE_START). The fake holds only
 * these bytes; a read or write anywhere else is counted as a violation. */
#define WINDOW_START      ((uint32_t)BOOT_STATE_START)
#define COPY_BYTES        ((uint32_t)BOOT_STATE_COPY_BYTES)
#define COPIES            2U
#define WINDOW_BYTES      (COPIES * COPY_BYTES)
#define PAGE_BYTES        ((uint32_t)FLASH_PAGE_BYTES)
#define PAGE_WORDS        (PAGE_BYTES / 4U)
/* A doubleword is the unit the chip programs [RM0440 Rev 9, Sec. 5.3.7, p. 196]. */
#define DW_BYTES          8U
#define WORD_BYTES        4U
#define BITS_PER_BYTE     8U
#define ERASED            0xFFU
/* A record is 16 bytes: two doublewords (boot_state.h). The first 12 bytes are checked. */
#define RECORD_BYTES      ((uint32_t)sizeof(boot_state_t))
#define CHECKED_BYTES     ((uint32_t)offsetof(boot_state_t, check))
/* Sentinel: no step is cut / no erase seen. */
#define NO_STEP           0xFFFFFFFFU
/* Most flash steps one write may make before the harness gives up on the count (a bound, rule
 * B2). A write makes 3 (one erase, two doubleword programs); 8 leaves room for a change. */
#define MAX_STEPS         8U

/* Test inputs. An image body check value, as given_up_check holds (boot_state.h): any non-zero
 * value will do, there is no source; three distinct values so a mix-up shows. */
#define MARK_A            0xC0DEC0DEU
#define MARK_B            0x5EED1234U
/* Sequence numbers for phase 5 (sequence wrap). */
#define SEQ_MAX           0xFFFFFFFFU

/* ---- Fake flash ------------------------------------------------------------------------ */

/* Variants of a power cut at one step (see the file comment). */
typedef enum {
    CUT_NONE = 0,         /* power stays on */
    CUT_NOT_DONE,         /* the step did not happen */
    CUT_DONE,             /* the step finished, then the power failed */
    CUT_HALF_LOW,         /* erase: the first half of the page's words erased, the rest old */
    CUT_HALF_HIGH,        /* erase: the second half erased, the first half old */
    CUT_HALF_EVEN,        /* erase: every even word erased, every odd word old */
    CUT_TORN_FIRST,       /* program: first word of the doubleword programmed, second erased */
    CUT_TORN_FIRST_ECC,   /* the same, and a read of that copy raises the ECC error */
    CUT_TORN_SECOND,      /* program: second word programmed, first erased */
    CUT_TORN_SECOND_ECC,  /* the same, with the ECC error */
    CUT_RANDOM            /* erase: each word erased or old at random; program: random bits */
} cut_t;

/* The cuts tried at an erase step, and at a program step, in table order. */
#define ERASE_CUTS_N      5U
#define PROGRAM_CUTS_N    6U
static const cut_t ERASE_CUTS[ERASE_CUTS_N] = {CUT_NOT_DONE, CUT_HALF_LOW, CUT_HALF_HIGH,
                                               CUT_HALF_EVEN, CUT_DONE};
static const cut_t PROGRAM_CUTS[PROGRAM_CUTS_N] = {CUT_NOT_DONE, CUT_TORN_FIRST,
                                                   CUT_TORN_FIRST_ECC, CUT_TORN_SECOND,
                                                   CUT_TORN_SECOND_ECC, CUT_DONE};

static uint8_t g_mem[WINDOW_BYTES];         /* the flash window */
static const uint8_t g_outside[BOOT_STATE_COPY_BYTES] = {0};   /* served for a read outside */
static uint32_t g_rng = RNG_SEED;            /* random state */
static uint32_t g_step;                      /* flash steps made in the current write */
static uint32_t g_cut_step = NO_STEP;        /* step at which the power fails */
static cut_t g_cut = CUT_NONE;               /* what happens at that step */
static bool g_powered = true;                /* false once the cut has happened */
static uint32_t g_ecc_copy = NO_STEP;        /* address of a copy whose read raises ECC */
static bool g_ecc_flag;                      /* the flag flash_ecc_error_seen() answers */
static uint32_t g_violations;                /* illegal flash operations seen */
static uint32_t g_first_erase = NO_STEP;     /* page address of the first erase of a write */
static uint32_t g_last_program = NO_STEP;    /* address of the last doubleword programmed */
static char g_kind[MAX_STEPS];               /* 'E' or 'P' per step of the current write */
static bool g_verbose;                       /* replay mode: print every write */

/**
 * @brief Returns the next pseudo-random number (xorshift32).
 *
 * Fixed seed, so a failing sequence repeats; see RNG_SEED.
 *
 * @return the next 32-bit value
 */
static uint32_t rng_next(void)
{
    g_rng ^= g_rng << RNG_SHIFT_A;
    g_rng ^= g_rng >> RNG_SHIFT_B;
    g_rng ^= g_rng << RNG_SHIFT_C;
    return g_rng;
}

/**
 * @brief Returns a pseudo-random number below a limit.
 *
 * The small bias of the modulo does not matter here: the numbers only pick test cases.
 *
 * @param[in]     limit  exclusive upper bound, at least 1
 * @return a value from 0 to limit - 1
 */
static uint32_t rng_below(uint32_t limit)
{
    return rng_next() % limit;
}

/**
 * @brief Returns true if a chip address lies inside the boot state window.
 *
 * @param[in]     address  chip address
 * @param[in]     bytes    length of the range from address
 * @return true if the whole range is inside both copies
 */
static bool in_window(uint32_t address, uint32_t bytes)
{
    return (address >= WINDOW_START) && (address < (WINDOW_START + WINDOW_BYTES)) &&
           (bytes <= ((WINDOW_START + WINDOW_BYTES) - address));
}

/**
 * @brief Returns the address of the first byte of the copy that holds an address.
 *
 * @param[in]     address  chip address inside the window
 * @return start of copy 0 or copy 1
 */
static uint32_t copy_of(uint32_t address)
{
    return WINDOW_START + (((address - WINDOW_START) / COPY_BYTES) * COPY_BYTES);
}

/**
 * @brief Sets the 4 bytes of one word of the window to the erased value.
 *
 * @param[in]     offset  byte offset of the word in the window, a multiple of 4
 */
static void erase_word(uint32_t offset)
{
    for (uint32_t i = 0U; i < WORD_BYTES; i++) {
        g_mem[offset + i] = ERASED;
    }
}

/**
 * @brief Applies a cut at an erase step to the page that starts at page_offset.
 *
 * NOT_DONE changes nothing; DONE erases the page; the half variants erase half of the page's
 * words and keep the old bytes of the rest (the brief's model of a page erase cut in the
 * middle; which words a real chip clears first is not published, so three patterns are tried).
 * A fully erased page also clears a pending ECC error of the copy it belongs to.
 *
 * @param[in]     page_offset  byte offset of the page in the window
 */
static void apply_erase_cut(uint32_t page_offset)
{
    for (uint32_t w = 0U; w < PAGE_WORDS; w++) {
        bool erase = false;

        switch (g_cut) {
        case CUT_DONE:
            erase = true;
            break;
        case CUT_HALF_LOW:
            erase = (w < (PAGE_WORDS / 2U));
            break;
        case CUT_HALF_HIGH:
            erase = (w >= (PAGE_WORDS / 2U));
            break;
        case CUT_HALF_EVEN:
            erase = ((w % 2U) == 0U);
            break;
        case CUT_RANDOM:
            erase = (rng_below(RANDOM_CHOICES) == 0U);
            break;
        case CUT_NONE:
        case CUT_NOT_DONE:
        case CUT_TORN_FIRST:
        case CUT_TORN_FIRST_ECC:
        case CUT_TORN_SECOND:
        case CUT_TORN_SECOND_ECC:
            erase = false;
            break;
        }
        if (erase) {
            erase_word(page_offset + (w * WORD_BYTES));
        }
    }
    if (g_cut == CUT_DONE) {
        g_ecc_copy = NO_STEP;
    }
}

/**
 * @brief Programs bits of one word into the window: flash can only clear bits, so the stored
 * value is the old value AND the programmed value, limited to the bits in mask.
 *
 * @param[in]     offset  byte offset of the word in the window
 * @param[in]     value   the 32-bit value being programmed, little-endian
 * @param[in]     mask    bits that were programmed before the cut (all ones for a whole word)
 */
static void program_word(uint32_t offset, uint32_t value, uint32_t mask)
{
    for (uint32_t i = 0U; i < WORD_BYTES; i++) {
        uint32_t shift = i * BITS_PER_BYTE;
        uint32_t bits = (value >> shift) & 0xFFU;
        uint32_t take = (mask >> shift) & 0xFFU;
        uint32_t old = g_mem[offset + i];

        g_mem[offset + i] = (uint8_t)(old & (bits | (~take & 0xFFU)));
    }
}

/**
 * @brief Applies a cut at a program step to the doubleword at dw_address.
 *
 * NOT_DONE changes nothing; DONE programs both words; TORN_FIRST programs the first word only and
 * TORN_SECOND the second only; the _ECC forms also make a read of that copy raise the ECC error;
 * RANDOM programs a random subset of the 64 bits and picks the ECC error at random.
 *
 * @param[in]     dw_address  chip address of the doubleword, 8-byte aligned
 * @param[in]     w0          first word being programmed
 * @param[in]     w1          second word being programmed
 */
static void apply_program_cut(uint32_t dw_address, uint32_t w0, uint32_t w1)
{
    uint32_t off = dw_address - WINDOW_START;
    uint32_t m0 = 0U;
    uint32_t m1 = 0U;
    bool ecc = false;

    switch (g_cut) {
    case CUT_DONE:
        m0 = 0xFFFFFFFFU;
        m1 = 0xFFFFFFFFU;
        break;
    case CUT_TORN_FIRST_ECC:
        ecc = true;
        m0 = 0xFFFFFFFFU;
        break;
    case CUT_TORN_FIRST:
        m0 = 0xFFFFFFFFU;
        break;
    case CUT_TORN_SECOND_ECC:
        ecc = true;
        m1 = 0xFFFFFFFFU;
        break;
    case CUT_TORN_SECOND:
        m1 = 0xFFFFFFFFU;
        break;
    case CUT_RANDOM:
        m0 = rng_next();
        m1 = rng_next();
        ecc = (rng_below(RANDOM_CHOICES) == 0U);
        break;
    case CUT_NONE:
    case CUT_NOT_DONE:
    case CUT_HALF_LOW:
    case CUT_HALF_HIGH:
    case CUT_HALF_EVEN:
        break;
    }
    program_word(off, w0, m0);
    program_word(off + WORD_BYTES, w1, m1);
    if (ecc) {
        g_ecc_copy = copy_of(dw_address);
    }
}

/**
 * @brief Page erase on the fake flash, shared by flash_erase_page and bl_flash_erase_page.
 *
 * Counts a step; if this is the cut step, applies the cut and removes the power. An address
 * outside the boot state window is a violation: a boot-state write must touch nothing else.
 *
 * @param[in]     page_start  any address inside the page
 * @return true if the erase finished before any cut
 */
static bool fake_erase(uint32_t page_start)
{
    if (!g_powered) {
        return false;
    }
    if (!in_window(page_start, 1U)) {
        g_violations++;
        return false;
    }
    uint32_t page_offset = ((page_start - WINDOW_START) / PAGE_BYTES) * PAGE_BYTES;

    if (g_first_erase == NO_STEP) {
        g_first_erase = WINDOW_START + page_offset;
    }
    if (g_step < MAX_STEPS) {
        g_kind[g_step] = 'E';
    }
    if (g_step == g_cut_step) {
        apply_erase_cut(page_offset);
        g_powered = false;
        g_step++;
        return false;
    }
    for (uint32_t w = 0U; w < PAGE_WORDS; w++) {
        erase_word(page_offset + (w * WORD_BYTES));
    }
    if (copy_of(WINDOW_START + page_offset) == g_ecc_copy) {
        g_ecc_copy = NO_STEP;
    }
    g_step++;
    return true;
}

/**
 * @brief Doubleword programming on the fake flash, shared by flash_program and bl_flash_program.
 *
 * One step per doubleword, in address order, first word before second inside a doubleword as the
 * real driver does (flash.c, flash_program). A start that is not 8-byte aligned, a range outside
 * the window, or a doubleword that is not erased is a violation (the chip needs an erased
 * doubleword [RM0440 Rev 9, Sec. 5.3.7, p. 196]); the bits are still ANDed, as the chip would.
 *
 * @param[in]     start  chip address of the first doubleword
 * @param[in]     words  32-bit words to program
 * @param[in]     pairs  number of doublewords
 * @return true if every doubleword finished before any cut
 */
static bool fake_program(uint32_t start, const uint32_t *words, uint32_t pairs)
{
    if (!g_powered) {
        return false;
    }
    if (((start % DW_BYTES) != 0U) || (pairs > (WINDOW_BYTES / DW_BYTES)) ||
        !in_window(start, pairs * DW_BYTES)) {
        g_violations++;
        return false;
    }
    for (uint32_t i = 0U; i < pairs; i++) {
        uint32_t dw = start + (i * DW_BYTES);
        uint32_t off = dw - WINDOW_START;

        g_last_program = dw;
        if (g_step < MAX_STEPS) {
            g_kind[g_step] = 'P';
        }
        for (uint32_t b = 0U; b < DW_BYTES; b++) {
            if (g_mem[off + b] != ERASED) {
                g_violations++;
                break;
            }
        }
        if (g_step == g_cut_step) {
            apply_program_cut(dw, words[2U * i], words[(2U * i) + 1U]);
            g_powered = false;
            g_step++;
            return false;
        }
        program_word(off, words[2U * i], 0xFFFFFFFFU);
        program_word(off + WORD_BYTES, words[(2U * i) + 1U], 0xFFFFFFFFU);
        g_step++;
    }
    return true;
}

/**
 * @brief The firmware's page erase (common/flash.h), served by the fake flash.
 *
 * @param[in]     page_start  any address inside the page
 * @return true if the erase finished
 */
bool flash_erase_page(uint32_t page_start)
{
    return fake_erase(page_start);
}

/**
 * @brief The firmware's doubleword programming (common/flash.h), served by the fake flash.
 *
 * @param[in]     start  chip address of the first doubleword
 * @param[in]     words  32-bit words to program
 * @param[in]     pairs  number of doublewords
 * @return true if every doubleword finished
 */
bool flash_program(uint32_t start, const uint32_t *words, uint32_t pairs)
{
    return fake_program(start, words, pairs);
}

/**
 * @brief The bootloader's page erase (bootloader/bl_flash.h), served by the fake flash.
 *
 * @param[in]     page_start  any address inside the page
 * @return true if the erase finished
 */
bool bl_flash_erase_page(uint32_t page_start)
{
    return fake_erase(page_start);
}

/**
 * @brief The bootloader's doubleword programming (bootloader/bl_flash.h), served by the fake.
 *
 * @param[in]     start  chip address of the first doubleword
 * @param[in]     words  32-bit words to program
 * @param[in]     pairs  number of doublewords
 * @return true if every doubleword finished
 */
bool bl_flash_program(uint32_t start, const uint32_t *words, uint32_t pairs)
{
    return fake_program(start, words, pairs);
}

/**
 * @brief Forgets earlier ECC errors (common/flash.h).
 */
void flash_ecc_clear(void)
{
    g_ecc_flag = false;
}

/**
 * @brief Returns true if a read since flash_ecc_clear() hit a torn doubleword (common/flash.h).
 *
 * @return the ECC flag
 */
bool flash_ecc_error_seen(void)
{
    return g_ecc_flag;
}

/**
 * @brief Forgets earlier ECC errors (bootloader/bl_flash.h).
 */
void bl_flash_ecc_clear(void)
{
    g_ecc_flag = false;
}

/**
 * @brief Returns true if a read since bl_flash_ecc_clear() hit a torn doubleword.
 *
 * @return the ECC flag
 */
bool bl_flash_ecc_error_seen(void)
{
    return g_ecc_flag;
}

/**
 * @brief Serves a flash read from the window; a read of a copy that holds a torn doubleword with
 * the ECC variant sets the ECC flag, as a two-bit error would (flash.h).
 *
 * Both writers and readers read a record from the start of its copy, so the flag is set when
 * that start address is read. A read outside the window is a violation and returns zeros.
 *
 * @param[in]     address  chip address of the first byte
 * @return pointer to the byte
 */
static const uint8_t *fake_at(uint32_t address)
{
    if (!in_window(address, RECORD_BYTES)) {
        g_violations++;
        return g_outside;
    }
    if (address == g_ecc_copy) {
        g_ecc_flag = true;
    }
    return &g_mem[address - WINDOW_START];
}

/**
 * @brief The firmware's flash read (common/flash.h), served by the fake flash.
 *
 * @param[in]     address  chip address of the first byte
 * @return pointer to the byte
 */
const uint8_t *flash_at(uint32_t address)
{
    return fake_at(address);
}

/**
 * @brief The bootloader's flash read (bootloader/bl_flash.h), served by the fake flash.
 *
 * @param[in]     address  chip address of the first byte
 * @return pointer to the byte
 */
const uint8_t *bl_flash_at(uint32_t address)
{
    return fake_at(address);
}

/**
 * @brief The fault reaction (common/fault.h). No phase may reach it: a record read from flash is
 * judged by its checks, never the fault reaction (boot_state.c, fields_valid).
 *
 * @param[in]     location  fault-location code
 * @param[in]     where     name of the function
 * @note Does not return.
 */
void fault_impossible_value(uint8_t location, const char *where)
{
    (void)printf("FAIL  fault reaction reached: location 0x%02x in %s\n", (unsigned)location,
                 where);
    exit(2);
}

/* ---- Records, views, the checker ------------------------------------------------------- */

/* The fields a command or the bootloader sets (the rest of boot_state_t is set by the write). */
typedef struct {
    uint8_t confirmed;
    uint8_t trial;
    uint8_t starts;
    uint8_t flags;
    uint32_t mark;
} fields_t;

/* What one reader saw. */
typedef struct {
    boot_state_load_t cls;
    boot_state_t st;
} view_t;

/* Which writer: the firmware's (common/) or the bootloader's. */
typedef enum { WRITER_FW = 0, WRITER_BL = 1 } writer_t;
#define WRITERS 2U

/* What the checker calls one case (one letter in the tables). */
typedef enum {
    OUT_OLD = 0,        /* o: the old state */
    OUT_NEW,            /* n: the new state */
    OUT_BLANK_DAMAGED,  /* D: old state was blank (never written), now damaged (observation) */
    OUT_THIRD,          /* X: a valid state that is neither the old nor the new one */
    OUT_LOST,           /* L: a valid state existed before, now blank or damaged */
    OUT_DISAGREE        /* d: the two readers disagree */
} outcome_t;

/**
 * @brief Returns the table letter of an outcome.
 *
 * @param[in]     o  the outcome
 * @return the letter
 */
static char token_of(outcome_t o)
{
    switch (o) {
    case OUT_OLD:
        return 'o';
    case OUT_NEW:
        return 'n';
    case OUT_BLANK_DAMAGED:
        return 'D';
    case OUT_THIRD:
        return 'X';
    case OUT_LOST:
        return 'L';
    case OUT_DISAGREE:
        return 'd';
    }
    return '?';
}

/**
 * @brief Fills a boot state record from fields; sequence and check are left for the write.
 *
 * @param[in]     f    the fields
 * @param[out]    rec  the record
 */
static void record_of(const fields_t *f, boot_state_t *rec)
{
    memset(rec, 0, sizeof(*rec));
    rec->confirmed = f->confirmed;
    rec->trial = f->trial;
    rec->failed_starts = f->starts;
    rec->flags = f->flags;
    rec->given_up_check = f->mark;
}

/**
 * @brief Writes a record with one of the two writers.
 *
 * @param[in]     w    the writer
 * @param[in,out] rec  the record; the writer sets sequence and check
 * @return the writer's answer
 */
static bool write_with(writer_t w, boot_state_t *rec)
{
    switch (w) {
    case WRITER_FW:
        return boot_state_write(rec);
    case WRITER_BL:
        return bl_boot_state_write(rec);
    }
    (void)printf("FAIL  unknown writer\n");
    exit(2);
}

/**
 * @brief Reads the flash with the firmware's reader and with the bootloader's reader.
 *
 * Also checks boot_state_read and bl_boot_state_read against the load result: read is true
 * exactly when the load is VALID. A disagreement is returned in agree_read.
 *
 * @param[out]    fw          what boot_state_load saw
 * @param[out]    bl          what bl_boot_state_load saw
 * @param[out]    agree_read  false if a read function disagrees with its load function
 */
static void read_both(view_t *fw, view_t *bl, bool *agree_read)
{
    boot_state_t scratch;

    memset(fw, 0, sizeof(*fw));
    memset(bl, 0, sizeof(*bl));
    fw->cls = boot_state_load(&fw->st);
    bl->cls = bl_boot_state_load(&bl->st);
    *agree_read = (boot_state_read(&scratch) == (fw->cls == BOOT_STATE_VALID)) &&
                  (bl_boot_state_read(&scratch) == (bl->cls == BOOT_STATE_VALID));
}

/**
 * @brief Returns true if two views are the same: same class and, when valid, same record.
 *
 * @param[in]     a  first view
 * @param[in]     b  second view
 * @return true if equal
 */
static bool views_equal(const view_t *a, const view_t *b)
{
    if (a->cls != b->cls) {
        return false;
    }
    return (a->cls != BOOT_STATE_VALID) || (memcmp(&a->st, &b->st, sizeof(a->st)) == 0);
}

/**
 * @brief Names what the flash holds after a write that may have been cut.
 *
 * The invariant (REQ-017, rule B7): the old state or the new state, never a third; never "no
 * valid state" when a valid one existed before; the two readers agree.
 *
 * @param[in]     before  the view before the write (both readers agreed on it)
 * @param[in]     fresh   the record the write meant to store, with sequence and check set
 * @param[in]     fw      what the firmware's reader saw after the write
 * @param[in]     bl      what the bootloader's reader saw after the write
 * @return the outcome
 */
static outcome_t evaluate(const view_t *before, const boot_state_t *fresh, const view_t *fw,
                          const view_t *bl)
{
    if (!views_equal(fw, bl)) {
        return OUT_DISAGREE;
    }
    switch (fw->cls) {
    case BOOT_STATE_VALID:
        if ((before->cls == BOOT_STATE_VALID) &&
            (memcmp(&fw->st, &before->st, sizeof(fw->st)) == 0)) {
            return OUT_OLD;
        }
        return (memcmp(&fw->st, fresh, sizeof(fw->st)) == 0) ? OUT_NEW : OUT_THIRD;
    case BOOT_STATE_BLANK:
        return (before->cls == BOOT_STATE_BLANK) ? OUT_OLD : OUT_LOST;
    case BOOT_STATE_DAMAGED:
        return (before->cls == BOOT_STATE_BLANK) ? OUT_BLANK_DAMAGED : OUT_LOST;
    }
    return OUT_THIRD;
}

/**
 * @brief Writes a record straight into the window, with its sequence number and CRC-32, as a
 * completed write would have left it. Used to forge states the writers do not make.
 *
 * The check value is the firmware's (crc32_compute over the first 12 bytes); the bootloader's
 * reader must agree with it, which the matrix also shows.
 *
 * @param[in]     copy  0 or 1
 * @param[in]     rec   the record; its sequence is used, its check is set here
 */
static void forge(uint32_t copy, boot_state_t *rec)
{
    rec->check = crc32_compute((const uint8_t *)rec, CHECKED_BYTES);
    memset(&g_mem[copy * COPY_BYTES], ERASED, PAGE_BYTES);
    memcpy(&g_mem[copy * COPY_BYTES], rec, sizeof(*rec));
}

/**
 * @brief Erases the whole window and clears every fake's state, as a fresh chip.
 */
static void flash_reset(void)
{
    memset(g_mem, ERASED, sizeof(g_mem));
    g_step = 0U;
    g_cut_step = NO_STEP;
    g_cut = CUT_NONE;
    g_powered = true;
    g_ecc_copy = NO_STEP;
    g_ecc_flag = false;
    g_violations = 0U;
    g_first_erase = NO_STEP;
    g_last_program = NO_STEP;
}

/**
 * @brief Counts a check that failed, and prints a line that names it.
 *
 * @param[in,out] counter  the counter to increase
 * @param[in]     what     what failed
 */
static void note_failure(uint32_t *counter, const char *what)
{
    (*counter)++;
    (void)printf("  FAILURE %s\n", what);
}

/* ---- One case --------------------------------------------------------------------------- */

/* Totals of all phases. */
typedef struct {
    uint32_t cases;
    uint32_t token[6];       /* by outcome_t */
    uint32_t outside;        /* bytes outside the target page changed, or wrong target */
    uint32_t violations;     /* illegal flash operations */
    uint32_t recovery;       /* the write after the cut failed or gave the wrong state */
    uint32_t readers;        /* boot_state_read / load disagree */
    uint32_t baseline;       /* an uncut write did not give the new state */
} totals_t;
static totals_t g_tot;

/* The result of one case, for the table and for the next write. */
typedef struct {
    outcome_t outcome;
    view_t after;
    boot_state_t fresh;
    bool wrote_ok;           /* what the writer answered */
    bool structure_ok;       /* target copy, outside bytes, no violation */
} case_t;

/**
 * @brief Finds the copy that holds the current (newest valid) state, by its bytes.
 *
 * Independent of the readers' choice: compares the stored bytes with the state both readers
 * returned.
 *
 * @param[in]     before  the view before the write
 * @return 0 or 1, or COPIES if the state is not in either copy (or none was valid)
 */
static uint32_t current_copy(const view_t *before)
{
    if (before->cls != BOOT_STATE_VALID) {
        return COPIES;
    }
    for (uint32_t c = 0U; c < COPIES; c++) {
        if (memcmp(&g_mem[c * COPY_BYTES], &before->st, sizeof(before->st)) == 0) {
            return c;
        }
    }
    return COPIES;
}

/**
 * @brief Runs one write with a power cut and checks everything about it.
 *
 * Checks: (1) the outcome letter; (2) the write went to the copy that is not current (rule B7);
 * (3) no byte outside that copy's first page changed, so the current copy is untouched;
 * (4) no illegal flash operation; (5) the read functions agree with the load functions. The
 * caller decides what to do with the result and with the flash after it.
 *
 * @param[in]     w         the writer
 * @param[in]     f         the fields to write
 * @param[in]     cut_step  the step to cut at, or NO_STEP
 * @param[in]     cut       what happens at that step
 * @param[out]    out       the result
 */
static void run_case(writer_t w, const fields_t *f, uint32_t cut_step, cut_t cut, case_t *out)
{
    uint8_t snapshot[WINDOW_BYTES];
    view_t b_fw;
    view_t b_bl;
    view_t a_fw;
    view_t a_bl;
    bool agree_before;
    bool agree_after;
    boot_state_t rec;

    memcpy(snapshot, g_mem, sizeof(snapshot));
    read_both(&b_fw, &b_bl, &agree_before);
    uint32_t cur = current_copy(&b_fw);

    record_of(f, &rec);
    g_step = 0U;
    g_cut_step = cut_step;
    g_cut = cut;
    g_powered = true;
    g_first_erase = NO_STEP;
    g_last_program = NO_STEP;
    g_violations = 0U;
    out->wrote_ok = write_with(w, &rec);
    g_cut_step = NO_STEP;
    g_cut = CUT_NONE;
    g_powered = true;
    read_both(&a_fw, &a_bl, &agree_after);
    out->fresh = rec;
    out->after = a_fw;
    out->outcome = evaluate(&b_fw, &rec, &a_fw, &a_bl);
    if (!views_equal(&b_fw, &b_bl) || !agree_before || !agree_after) {
        g_tot.readers++;
    }

    bool structure = (g_violations == 0U);
    uint32_t want_target = (cur == 0U) ? 1U : 0U;
    uint32_t want_page = WINDOW_START + (want_target * COPY_BYTES);

    structure = structure && (g_first_erase == want_page);
    for (uint32_t i = 0U; i < WINDOW_BYTES; i++) {
        bool in_target = (i >= (want_target * COPY_BYTES)) &&
                         (i < ((want_target * COPY_BYTES) + PAGE_BYTES));
        if (!in_target && (g_mem[i] != snapshot[i])) {
            structure = false;
        }
    }
    if (g_violations != 0U) {
        g_tot.violations++;
    }
    if (!structure) {
        g_tot.outside++;
    }
    out->structure_ok = structure;
    g_tot.cases++;
    g_tot.token[out->outcome]++;
}

/**
 * @brief After a cut, runs the same write again with the other writer and checks the result.
 *
 * What a restart does: the bootloader or the firmware finds the state and writes again. The write
 * must succeed, and the new state must read back with sequence one above what the cut left.
 *
 * @param[in]     w     the writer that was cut; the other one writes now
 * @param[in]     f     the fields being written
 * @param[in]     seen  the view the cut left
 * @return true if the retry wrote the right record
 */
static bool recovery_ok(writer_t w, const fields_t *f, const view_t *seen)
{
    boot_state_t rec;
    view_t fw;
    view_t bl;
    bool agree;
    writer_t other = (w == WRITER_FW) ? WRITER_BL : WRITER_FW;

    record_of(f, &rec);
    g_violations = 0U;
    bool wrote = write_with(other, &rec);
    read_both(&fw, &bl, &agree);
    uint32_t want_seq = (seen->cls == BOOT_STATE_VALID) ? (seen->st.sequence + 1U) : 1U;

    return wrote && agree && views_equal(&fw, &bl) && (fw.cls == BOOT_STATE_VALID) &&
           (memcmp(&fw.st, &rec, sizeof(rec)) == 0) && (rec.sequence == want_seq) &&
           (g_violations == 0U);
}

/* ---- Phase 1: checker self-test ---------------------------------------------------------- */

/**
 * @brief Checks that the checker names a forged third state, a lost state, a blank-to-damaged
 * state and a reader disagreement correctly (rule E1: prove the test catches the defect).
 *
 * @return true if every forged case got its right name
 */
static bool selftest(void)
{
    fields_t f = {BOOT_SLOT_A, BOOT_SLOT_NONE, 0U, 0U, 0U};
    fields_t g = {BOOT_SLOT_A, BOOT_SLOT_B, 0U, 0U, 0U};
    boot_state_t rec;
    boot_state_t third;
    view_t before_fw;
    view_t before_bl;
    view_t a_fw;
    view_t a_bl;
    bool agree;
    bool ok = true;

    flash_reset();
    record_of(&f, &rec);
    ok = ok && boot_state_write(&rec);
    read_both(&before_fw, &before_bl, &agree);
    record_of(&g, &third);
    third.sequence = rec.sequence + 1U;
    /* A third state: valid, higher sequence, but not what the write meant. */
    boot_state_t meant;
    record_of(&f, &meant);
    meant.sequence = rec.sequence + 1U;
    meant.check = crc32_compute((const uint8_t *)&meant, CHECKED_BYTES);
    forge(1U, &third);
    read_both(&a_fw, &a_bl, &agree);
    ok = ok && (evaluate(&before_fw, &meant, &a_fw, &a_bl) == OUT_THIRD);
    /* A lost state: both copies erased. */
    flash_reset();
    read_both(&a_fw, &a_bl, &agree);
    ok = ok && (evaluate(&before_fw, &meant, &a_fw, &a_bl) == OUT_LOST);
    /* Blank before, damaged after: one copy with a bad check. */
    boot_state_t blank_before = {0};
    view_t blank_view;
    memset(&blank_view, 0, sizeof(blank_view));
    blank_view.cls = BOOT_STATE_BLANK;
    g_mem[0] = 0U;
    read_both(&a_fw, &a_bl, &agree);
    ok = ok && (evaluate(&blank_view, &blank_before, &a_fw, &a_bl) == OUT_BLANK_DAMAGED);
    /* A disagreement. */
    a_bl.cls = BOOT_STATE_BLANK;
    ok = ok && (evaluate(&before_fw, &meant, &a_fw, &a_bl) == OUT_DISAGREE);
    return ok;
}

/* ---- Phase 2: the matrix ------------------------------------------------------------------ */

/* One boot-state write the code makes: the old state, and the state the write stores. */
typedef struct {
    const char *name;      /* what writes it, with the source */
    bool old_blank;        /* the old state is "never written" */
    fields_t old;
    fields_t next;
} scenario_t;

#define SLOT_A   BOOT_SLOT_A
#define SLOT_B   BOOT_SLOT_B
#define NO_SLOT  BOOT_SLOT_NONE
#define FALLBK   BOOT_STATE_FLAG_FALLBACK

/* Every boot-state write of the controller code (sources in the names). Each is cut under both
 * writers, although in the product one writer makes it: the second is a superset. */
static const scenario_t SCENARIOS[] = {
    {"bl default save on blank (main.c load_state)", true, {0U, 0U, 0U, 0U, 0U},
     {SLOT_A, NO_SLOT, 0U, 0U, 0U}},
    {"fw SWITCH (mode_manager.cpp runSwitch), no mark", false, {SLOT_A, NO_SLOT, 0U, 0U, 0U},
     {SLOT_A, SLOT_B, 0U, 0U, 0U}},
    {"fw SWITCH keeps mark, clears FALLBACK", false, {SLOT_A, NO_SLOT, 0U, FALLBK, MARK_A},
     {SLOT_A, SLOT_B, 0U, 0U, MARK_A}},
    {"fw START FAILED clears mark (runSwitch)", false, {SLOT_A, NO_SLOT, 0U, FALLBK, MARK_A},
     {SLOT_A, SLOT_B, 0U, 0U, 0U}},
    {"bl counts start 1 (main.c count_trial_start)", false, {SLOT_A, SLOT_B, 0U, 0U, 0U},
     {SLOT_A, SLOT_B, 1U, 0U, 0U}},
    {"bl counts start 2", false, {SLOT_A, SLOT_B, 1U, 0U, 0U}, {SLOT_A, SLOT_B, 2U, 0U, 0U}},
    {"bl counts start 3", false, {SLOT_A, SLOT_B, 2U, 0U, 0U}, {SLOT_A, SLOT_B, 3U, 0U, 0U}},
    {"fw CONFIRM keeps mark (runConfirm)", false, {SLOT_A, SLOT_B, 2U, 0U, MARK_B},
     {SLOT_B, NO_SLOT, 0U, 0U, MARK_B}},
    {"bl give-up after 3 starts (give_up_trial), mark", false, {SLOT_A, SLOT_B, 3U, 0U, 0U},
     {SLOT_A, NO_SLOT, 0U, FALLBK, MARK_A}},
    {"fw RETURN TO PREVIOUS / giveUp (giveUpState)", false, {SLOT_A, SLOT_B, 1U, 0U, 0U},
     {SLOT_A, NO_SLOT, 0U, FALLBK, MARK_A}},
    {"bl give-up with B confirmed, A on trial", false, {SLOT_B, SLOT_A, 3U, 0U, 0U},
     {SLOT_B, NO_SLOT, 0U, FALLBK, MARK_B}},
    {"fw LEAVE FALLBACK (runLeaveFallback)", false, {SLOT_A, NO_SLOT, 0U, FALLBK, MARK_A},
     {SLOT_A, NO_SLOT, 0U, 0U, MARK_A}},
    {"bl give-up replaces an older mark", false, {SLOT_A, SLOT_B, 3U, FALLBK, MARK_B},
     {SLOT_A, NO_SLOT, 0U, FALLBK, MARK_A}},
};
#define SCENARIO_COUNT ((uint32_t)(sizeof(SCENARIOS) / sizeof(SCENARIOS[0])))
/* Filler state written first for parity 1, so the old state sits in copy 1. */
static const fields_t FILLER = {SLOT_A, NO_SLOT, 0U, 0U, 0U};
#define PARITIES 2U

/**
 * @brief Builds the flash for one matrix row: blank, or the old state in copy 0 (parity 0) or in
 * copy 1 (parity 1, after a filler write).
 *
 * @param[in]     sc      the scenario
 * @param[in]     parity  0 or 1
 * @return true if every preparing write succeeded
 */
static bool prepare(const scenario_t *sc, uint32_t parity)
{
    boot_state_t rec;
    bool ok = true;

    flash_reset();
    if (sc->old_blank) {
        return true;
    }
    if (parity == 1U) {
        record_of(&FILLER, &rec);
        ok = ok && boot_state_write(&rec);
    }
    record_of(&sc->old, &rec);
    return ok && boot_state_write(&rec);
}

/**
 * @brief Runs and prints one matrix row: one scenario, one writer, one parity, every step and
 * every variant of the cut.
 *
 * The first line is the uncut write, which gives the number of steps and their kinds. Then one
 * letter per (step, variant). Failures are printed and counted.
 *
 * @param[in]     si      scenario number
 * @param[in]     w       the writer
 * @param[in]     parity  0 or 1
 * @return true if every case in the row held the invariant
 */
static bool matrix_row(uint32_t si, writer_t w, uint32_t parity)
{
    const scenario_t *sc = &SCENARIOS[si];
    uint8_t base[WINDOW_BYTES];
    case_t res;
    bool clean = true;
    char line[160];
    uint32_t used = 0U;

    if (!prepare(sc, parity)) {
        (void)printf("FAIL  S%02u preparation failed\n", (unsigned)si);
        return false;
    }
    memcpy(base, g_mem, sizeof(base));
    /* The uncut write: steps, kinds, and the new state. */
    run_case(w, &sc->next, NO_STEP, CUT_NONE, &res);
    uint32_t steps = g_step;
    char kinds[MAX_STEPS + 1U];
    uint32_t kn = (steps < MAX_STEPS) ? steps : MAX_STEPS;
    memcpy(kinds, g_kind, kn);
    kinds[kn] = '\0';
    bool order = (kn >= 2U) && (kinds[0] == 'E') && (g_last_program == (g_first_erase + DW_BYTES));
    for (uint32_t i = 1U; i < kn; i++) {
        order = order && (kinds[i] == 'P');
    }
    if ((res.outcome != OUT_NEW) || !res.structure_ok || !res.wrote_ok || !order) {
        note_failure(&g_tot.baseline, "uncut write did not give the new state or wrong order");
        clean = false;
    }
    (void)printf("S%02u %-8s par%u steps=%u [%s]", (unsigned)si,
                 (w == WRITER_FW) ? "fw-write" : "bl-write", (unsigned)parity, (unsigned)steps,
                 kinds);
    for (uint32_t k = 0U; (k < kn) && (k < MAX_STEPS); k++) {
        uint32_t n = (kinds[k] == 'E') ? ERASE_CUTS_N : PROGRAM_CUTS_N;
        const cut_t *list = (kinds[k] == 'E') ? ERASE_CUTS : PROGRAM_CUTS;

        used = 0U;
        line[0] = '\0';
        for (uint32_t v = 0U; v < n; v++) {
            memcpy(g_mem, base, sizeof(base));
            g_ecc_copy = NO_STEP;
            run_case(w, &sc->next, k, list[v], &res);
            bool recovered = recovery_ok(w, &sc->next, &res.after);
            if (!recovered) {
                note_failure(&g_tot.recovery, "write after the cut failed or gave a wrong state");
            }
            bool bad = (res.outcome != OUT_OLD) && (res.outcome != OUT_NEW) &&
                       !((res.outcome == OUT_BLANK_DAMAGED));
            if (bad || !res.structure_ok || !recovered) {
                clean = false;
                (void)printf("\nFAILCASE %u %u %u %u %u", (unsigned)si, (unsigned)w,
                             (unsigned)parity, (unsigned)k, (unsigned)list[v]);
            }
            if (used < (sizeof(line) - 2U)) {
                line[used] = token_of(res.outcome);
                used++;
                line[used] = '\0';
            }
        }
        (void)printf(" %c:%s", kinds[k], line);
    }
    (void)printf("  %s\n", sc->name);
    memcpy(g_mem, base, sizeof(base));
    return clean;
}

/**
 * @brief Runs the whole matrix and prints its table.
 *
 * @return true if every row held the invariant
 */
static bool matrix(void)
{
    bool clean = true;

    (void)printf("legend: o old state, n new state, D blank->damaged (observation), X third state,"
                 " L state lost, d readers disagree\n");
    (void)printf("per step letters in variant order: E: not-done, half-low, half-high, half-even,"
                 " done; P: not-done, torn-1st, torn-1st+ECC, torn-2nd, torn-2nd+ECC, done\n");
    for (uint32_t s = 0U; s < SCENARIO_COUNT; s++) {
        for (uint32_t w = 0U; w < WRITERS; w++) {
            for (uint32_t p = 0U; p < PARITIES; p++) {
                if (SCENARIOS[s].old_blank && (p == 1U)) {
                    continue;
                }
                clean = matrix_row(s, (writer_t)w, p) && clean;
            }
        }
    }
    return clean;
}

/* ---- Phase 3: random sequences ------------------------------------------------------------ */

/* Random sequences' own counters. */
static uint32_t g_cut_writes;
static uint32_t g_cut_pairs;
static uint32_t g_cut_by_cut[CUT_RANDOM + 1U];
static uint32_t g_fw_writes;
static uint32_t g_bl_writes;

/**
 * @brief Returns a random record with allowed fields (boot_state.c, fields_valid).
 *
 * @return the fields
 */
static fields_t random_fields(void)
{
    fields_t f;

    f.confirmed = (rng_below(RANDOM_CHOICES) == 0U) ? SLOT_A : SLOT_B;
    f.trial = (rng_below(RANDOM_CHOICES) == 0U) ? NO_SLOT : boot_state_other_slot(f.confirmed);
    f.starts = (uint8_t)rng_below(BOOT_STATE_MAX_STARTS + 1U);
    f.flags = (rng_below(RANDOM_CHOICES) == 0U) ? 0U : FALLBK;
    f.mark = (rng_below(RANDOM_CHOICES) == 0U) ? 0U : rng_next();
    return f;
}

/**
 * @brief Picks a random cut for a step of a given kind.
 *
 * One of the listed variants of that kind, or CUT_RANDOM (random bits, random words).
 *
 * @param[in]     kind  'E' or 'P'
 * @return the cut
 */
static cut_t random_cut(char kind)
{
    uint32_t n = (kind == 'E') ? ERASE_CUTS_N : PROGRAM_CUTS_N;
    const cut_t *list = (kind == 'E') ? ERASE_CUTS : PROGRAM_CUTS;
    uint32_t pick = rng_below(n + 1U);

    return (pick == n) ? CUT_RANDOM : list[pick];
}

/**
 * @brief Runs one random sequence: a complete first write, then WRITES_PER_SEQ random writes,
 * most of them cut, checking the invariant after each.
 *
 * Every cut write must leave the old or the new state. The expected "old" for the next write is
 * whichever the readers report, because both are allowed.
 *
 * @param[in]     index    number of the sequence (its seed)
 * @param[in]     steps    flash steps of one uncut write, from the matrix
 * @param[in]     kinds    'E' or 'P' per step
 * @return true if the invariant held in every write
 */
static bool random_sequence(uint32_t index, uint32_t steps, const char *kinds)
{
    boot_state_t rec;
    case_t res;
    bool clean = true;
    bool prev_cut = false;

    g_rng = RNG_SEED + (index * SEED_SPREAD);
    if (g_rng == 0U) {
        g_rng = 1U;
    }
    flash_reset();
    record_of(&FILLER, &rec);
    if (!boot_state_write(&rec)) {
        return false;
    }
    for (uint32_t i = 0U; i < WRITES_PER_SEQ; i++) {
        fields_t f = random_fields();
        writer_t w = (rng_below(RANDOM_CHOICES) == 0U) ? WRITER_FW : WRITER_BL;
        bool cut = (rng_below(PERCENT) < CUT_PERCENT);
        uint32_t step = rng_below(steps);
        cut_t c = cut ? random_cut(kinds[step]) : CUT_NONE;

        run_case(w, &f, cut ? step : NO_STEP, c, &res);
        if (w == WRITER_FW) {
            g_fw_writes++;
        } else {
            g_bl_writes++;
        }
        if (cut) {
            g_cut_writes++;
            g_cut_by_cut[c]++;
            if (prev_cut) {
                g_cut_pairs++;
            }
        }
        prev_cut = cut;
        if (g_verbose) {
            (void)printf("  write %2u %s step %d cut %d -> %c\n", (unsigned)i,
                         (w == WRITER_FW) ? "fw" : "bl", cut ? (int)step : -1, (int)c,
                         token_of(res.outcome));
        }
        bool bad = ((res.outcome != OUT_OLD) && (res.outcome != OUT_NEW)) || !res.structure_ok;
        bool complete_ok = cut || ((res.outcome == OUT_NEW) && res.wrote_ok);
        if (bad || !complete_ok) {
            (void)printf("FAILSEQ %u write %u writer %u step %u cut %u outcome %c\n",
                         (unsigned)index, (unsigned)i, (unsigned)w, (unsigned)step, (unsigned)c,
                         token_of(res.outcome));
            clean = false;
            break;
        }
    }
    return clean;
}

/* ---- Phase 4 and 5: observations ---------------------------------------------------------- */

/**
 * @brief First write from blank flash with a random cut, many times (phase 4).
 *
 * Not an invariant of REQ-017 as worded in the code (the old state here is "never written"), so
 * the result is counted, not failed: how often a cut in the first write leaves the flash
 * damaged (decision D: the bootloader then starts only the last-resort firmware) rather than
 * blank or written.
 *
 * @param[in]     steps  flash steps of one uncut write
 * @param[in]     kinds  'E' or 'P' per step
 */
static void blank_first_write(uint32_t steps, const char *kinds)
{
    uint32_t count[6] = {0};
    fields_t f = {SLOT_A, NO_SLOT, 0U, 0U, 0U};
    case_t res;

    g_rng = RNG_SEED;
    for (uint32_t i = 0U; i < BLANK_CUT_COUNT; i++) {
        uint32_t step = rng_below(steps);
        writer_t w = (rng_below(RANDOM_CHOICES) == 0U) ? WRITER_FW : WRITER_BL;

        flash_reset();
        run_case(w, &f, step, random_cut(kinds[step]), &res);
        count[res.outcome]++;
    }
    (void)printf("phase 4  first write from blank flash, %u random cuts: blank(o) %u, written(n)"
                 " %u, damaged(D) %u, other %u\n", (unsigned)BLANK_CUT_COUNT,
                 (unsigned)count[OUT_OLD], (unsigned)count[OUT_NEW],
                 (unsigned)count[OUT_BLANK_DAMAGED],
                 (unsigned)(count[OUT_THIRD] + count[OUT_LOST] + count[OUT_DISAGREE]));
}

/**
 * @brief Writes at the top of the 32-bit sequence number (phase 5, observation).
 *
 * boot_state.c says the number never wraps because the flash wears out first (about 20 000
 * writes). A record with sequence 0xFFFFFFFF is forged to see what the code does anyway: the next
 * write gets sequence 0 and the readers prefer the older copy with the higher number.
 *
 * @param[in]     w  the writer
 */
static void sequence_wrap(writer_t w)
{
    boot_state_t rec;
    boot_state_t next;
    view_t fw;
    view_t bl;
    bool agree;
    fields_t f = {SLOT_A, SLOT_B, 0U, 0U, 0U};
    fields_t g = {SLOT_A, NO_SLOT, 0U, 0U, 0U};

    flash_reset();
    record_of(&g, &rec);
    rec.sequence = SEQ_MAX - 1U;
    forge(0U, &rec);
    record_of(&f, &next);
    bool first = write_with(w, &next);
    read_both(&fw, &bl, &agree);
    (void)printf("phase 5  %s: write at sequence 0xFFFFFFFE answered %d, flash now sequence 0x%08X"
                 " trial %u\n", (w == WRITER_FW) ? "fw" : "bl", (int)first,
                 (unsigned)fw.st.sequence, (unsigned)fw.st.trial);
    record_of(&g, &next);
    bool second = write_with(w, &next);
    read_both(&fw, &bl, &agree);
    (void)printf("         write at sequence 0xFFFFFFFF answered %d (record sequence %u);"
                 " readers: %s, sequence 0x%08X trial %u (the new record has trial 0)\n",
                 (int)second, (unsigned)next.sequence, views_equal(&fw, &bl) ? "agree" : "DISAGREE",
                 (unsigned)fw.st.sequence, (unsigned)fw.st.trial);
}

/* ---- main ---------------------------------------------------------------------------------- */

/**
 * @brief Replays one matrix case, with a trace of the records before and after.
 *
 * @param[in]     si  scenario number
 * @param[in]     w   writer
 * @param[in]     p   parity
 * @param[in]     k   step
 * @param[in]     c   cut
 */
static void replay_case(uint32_t si, uint32_t w, uint32_t p, uint32_t k, uint32_t c)
{
    case_t res;
    view_t fw;
    view_t bl;
    bool agree;

    if (si >= SCENARIO_COUNT) {
        (void)printf("no such scenario\n");
        return;
    }
    (void)prepare(&SCENARIOS[si], p);
    read_both(&fw, &bl, &agree);
    (void)printf("before: class %d seq %u conf %u trial %u starts %u flags %u mark 0x%08X\n",
                 (int)fw.cls, (unsigned)fw.st.sequence, (unsigned)fw.st.confirmed,
                 (unsigned)fw.st.trial, (unsigned)fw.st.failed_starts, (unsigned)fw.st.flags,
                 (unsigned)fw.st.given_up_check);
    run_case((writer_t)w, &SCENARIOS[si].next, k, (cut_t)c, &res);
    read_both(&fw, &bl, &agree);
    (void)printf("after : fw class %d bl class %d seq %u conf %u trial %u starts %u outcome %c"
                 " structure %d\n", (int)fw.cls, (int)bl.cls, (unsigned)fw.st.sequence,
                 (unsigned)fw.st.confirmed, (unsigned)fw.st.trial, (unsigned)fw.st.failed_starts,
                 token_of(res.outcome), (int)res.structure_ok);
}

/**
 * @brief Runs the phases and prints the totals.
 *
 * @param[in]     argc  argument count
 * @param[in]     argv  arguments; see the file comment
 * @return 0 if every invariant held, 1 if one failed, 2 on a harness error
 */
int main(int argc, char **argv)
{
    uint32_t steps = 0U;
    char kinds[MAX_STEPS + 1U] = {0};

    if ((argc == 7) && (strcmp(argv[1], "--case") == 0)) {
        replay_case((uint32_t)atoi(argv[2]), (uint32_t)atoi(argv[3]), (uint32_t)atoi(argv[4]),
                    (uint32_t)atoi(argv[5]), (uint32_t)atoi(argv[6]));
        return 0;
    }
    bool checker_ok = selftest();

    (void)printf("phase 1  checker self-test: %s\n", checker_ok ? "PASS" : "FAIL");
    if (!checker_ok) {
        return 2;
    }
    (void)printf("phase 2  matrix: %u scenarios x 2 writers x 2 copy parities\n",
                 (unsigned)SCENARIO_COUNT);
    bool clean = matrix();

    /* Steps of one uncut write, measured (not assumed) for the random phase. */
    (void)prepare(&SCENARIOS[1], 0U);
    case_t base;
    run_case(WRITER_FW, &SCENARIOS[1].next, NO_STEP, CUT_NONE, &base);
    steps = (g_step < MAX_STEPS) ? g_step : MAX_STEPS;
    memcpy(kinds, g_kind, steps);

    uint32_t seq_fail = 0U;
    uint32_t first = 0U;
    uint32_t last = SEQUENCE_COUNT;

    if ((argc == 3) && (strcmp(argv[1], "--seq") == 0)) {
        first = (uint32_t)atoi(argv[2]);
        last = first + 1U;
        g_verbose = true;
    }
    for (uint32_t i = first; i < last; i++) {
        if (!random_sequence(i, steps, kinds)) {
            seq_fail++;
        }
    }
    (void)printf("phase 3  random: %u sequences x %u writes, %u writes cut (%u fw, %u bl writes),"
                 " %u cut writes directly after a cut write, failing sequences %u\n",
                 (unsigned)(last - first), (unsigned)WRITES_PER_SEQ, (unsigned)g_cut_writes,
                 (unsigned)g_fw_writes, (unsigned)g_bl_writes, (unsigned)g_cut_pairs,
                 (unsigned)seq_fail);
    (void)printf("         cuts by kind: not-done %u, done %u, half-low %u, half-high %u,"
                 " half-even %u, torn-1st %u(+ECC %u), torn-2nd %u(+ECC %u), random %u\n",
                 (unsigned)g_cut_by_cut[CUT_NOT_DONE], (unsigned)g_cut_by_cut[CUT_DONE],
                 (unsigned)g_cut_by_cut[CUT_HALF_LOW], (unsigned)g_cut_by_cut[CUT_HALF_HIGH],
                 (unsigned)g_cut_by_cut[CUT_HALF_EVEN], (unsigned)g_cut_by_cut[CUT_TORN_FIRST],
                 (unsigned)g_cut_by_cut[CUT_TORN_FIRST_ECC],
                 (unsigned)g_cut_by_cut[CUT_TORN_SECOND],
                 (unsigned)g_cut_by_cut[CUT_TORN_SECOND_ECC], (unsigned)g_cut_by_cut[CUT_RANDOM]);
    blank_first_write(steps, kinds);
    sequence_wrap(WRITER_FW);
    sequence_wrap(WRITER_BL);

    (void)printf("totals   cases %u: old %u, new %u, blank->damaged %u, third %u, lost %u,"
                 " readers-disagree %u\n", (unsigned)g_tot.cases, (unsigned)g_tot.token[OUT_OLD],
                 (unsigned)g_tot.token[OUT_NEW], (unsigned)g_tot.token[OUT_BLANK_DAMAGED],
                 (unsigned)g_tot.token[OUT_THIRD], (unsigned)g_tot.token[OUT_LOST],
                 (unsigned)g_tot.token[OUT_DISAGREE]);
    (void)printf("         structure failures %u, illegal flash operations %u, recovery failures"
                 " %u, read/load mismatches %u, uncut-write failures %u\n",
                 (unsigned)g_tot.outside, (unsigned)g_tot.violations, (unsigned)g_tot.recovery,
                 (unsigned)g_tot.readers, (unsigned)g_tot.baseline);
    bool invariant = clean && (seq_fail == 0U) && (g_tot.token[OUT_THIRD] == 0U) &&
                     (g_tot.token[OUT_LOST] == 0U) && (g_tot.token[OUT_DISAGREE] == 0U) &&
                     (g_tot.outside == 0U) && (g_tot.violations == 0U) &&
                     (g_tot.recovery == 0U) && (g_tot.readers == 0U) && (g_tot.baseline == 0U);
    (void)printf("%s  boot state through a power cut (invariant)\n", invariant ? "PASS" : "FAIL");
    return invariant ? 0 : 1;
}
