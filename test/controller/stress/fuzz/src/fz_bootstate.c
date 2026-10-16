/*
 * fz_bootstate.c — fuzz target 4: the boot state. The firmware's boot_state_load and
 * boot_state_read [common/boot_state.h:65-69] and the bootloader's bl_boot_state_load and
 * bl_boot_state_read [bootloader/bl_boot_state.h:25-29] are run on fuzzed contents of both
 * stored copies in a RAM flash and must agree on every input (REQ-119: the two share no code).
 *
 * Oracles: the two implementations against each other, and a third reading written here from
 * the rule of boot_state.h and boot_state_classify (a copy is valid if it was read without a flash
 * ECC error, its CRC-32 matches, and its fields are allowed: confirmed A or B, trial none or the
 * other slot, failed starts 0 to 3, flags 0 or 1; the newest valid copy wins, copy 0 on a tie;
 * both copies all 0xFF and readable is BLANK; anything else without a valid copy is DAMAGED).
 * The CRC-32 of the oracle is zlib's.
 *
 * The cases the brief asks to state, with the counts of what each implementation did, are in the
 * log table: two valid copies, one valid copy, none, and a copy whose CRC is valid but whose
 * fields cannot exist. The fault reaction is recorded; the rule of CODE_RULES B1 (a value that
 * passes its CRC but cannot exist is corruption) against the rule of boot_state.c and
 * boot_state.h (such a copy is damage, not a fault; owner's fault decision of 13 Oct 2026,
 * point 4) is reported, not decided.
 *
 * One in eight inputs also compares boot_state_write against bl_boot_state_write from the same
 * flash contents (same result, same bytes in flash).
 */
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include "bl_boot_state.h"
#include "boot_state.h"
#include "fz_common.h"
#include "fz_fault.h"
#include "fz_ramflash.h"
#include "layout.h"

/* ---- Constants -------------------------------------------------------------------------- */

#define COPIES            2U
#define RECORD_BYTES      ((uint32_t)sizeof(boot_state_t))
/* The check value covers the 12 bytes before it [boot_state.h:50-51]. */
#define CHECKED_BYTES     12U
#define COPY_BYTES        ((uint32_t)BOOT_STATE_COPY_BYTES)
#define AREA_BYTES        (COPIES * COPY_BYTES)
#define SLOT_ERASED       0xFFU
/* Fill that shows whether a function wrote its output. */
#define SENTINEL_BYTE     0xA5U
#define CHANCE_ECC        100U
#define CHANCE_TIE        250U
#define CHANCE_WRITE      125U
#define KIND_COUNT        10U

static const uint32_t COPY_START[COPIES] = {BOOT_STATE_COPY_0, BOOT_STATE_COPY_1};

/* Outcome table: rows are how many copies are valid (0, 1, 2) with a column for "a copy with a
 * valid CRC and impossible fields is present"; columns are VALID, BLANK, DAMAGED. */
static uint64_t g_table[3][2][3];
static uint64_t g_faults;
static uint64_t g_ecc_cases;
static uint64_t g_write_compared;
static uint64_t g_crc_impossible_present;

/**
 * @brief Returns the allowed-fields rule of boot_state.h as an oracle.
 *
 * @param[in]     r  the record
 * @return true if confirmed, trial, failed starts and flags are all allowed
 */
static bool ref_fields_ok(const boot_state_t *r)
{
    bool confirmed_ok = (r->confirmed == BOOT_SLOT_A) || (r->confirmed == BOOT_SLOT_B);
    uint8_t other = (r->confirmed == BOOT_SLOT_A) ? (uint8_t)BOOT_SLOT_B : (uint8_t)BOOT_SLOT_A;

    return confirmed_ok && ((r->trial == BOOT_SLOT_NONE) || (r->trial == other)) &&
           (r->failed_starts <= BOOT_STATE_MAX_STARTS) &&
           ((r->flags == 0U) || (r->flags == BOOT_STATE_FLAG_FALLBACK));
}

/**
 * @brief Returns true if the CRC-32 of the first 12 bytes matches the check field (zlib's CRC).
 *
 * @param[in]     r  the record
 * @return true if it matches
 */
static bool ref_crc_ok(const boot_state_t *r)
{
    return (uint32_t)crc32(0U, (const Bytef *)r, CHECKED_BYTES) == r->check;
}

/**
 * @brief Returns true if all 16 bytes of a record are 0xFF.
 *
 * @param[in]     r  the record
 * @return true if blank
 */
static bool ref_blank(const boot_state_t *r)
{
    const uint8_t *b = (const uint8_t *)r;

    for (uint32_t i = 0U; i < RECORD_BYTES; i++) {
        if (b[i] != SLOT_ERASED) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Writes the CRC-32 of the first 12 bytes into a record's check field.
 *
 * @param[in,out] r  the record
 */
static void set_crc(boot_state_t *r)
{
    r->check = (uint32_t)crc32(0U, (const Bytef *)r, CHECKED_BYTES);
}

/**
 * @brief Fills a record of one kind.
 *
 * Kinds: 0 valid; 1 valid CRC with fields that cannot exist; 2 valid with 1 to 3 flipped bits;
 * 3 blank; 4 blank but one byte; 5 all zero; 6 random; 7 valid with a random check value; 8, 9
 * valid with the other kind's fields.
 *
 * @param[in,out] rng   the generator
 * @param[in]     kind  0 to KIND_COUNT - 1
 * @param[out]    r     the record
 */
static void make_record(fz_rng_t *rng, uint32_t kind, boot_state_t *r)
{
    static const uint8_t odd_slot[] = {0U, 3U, 4U, 0x7FU, 0x80U, 0xFEU, 0xFFU};
    static const uint8_t odd_count[] = {4U, 5U, 0x7FU, 0x80U, 0xFFU};
    static const uint8_t odd_flags[] = {2U, 3U, 0x80U, 0xFFU, 0x81U};
    uint8_t *b = (uint8_t *)r;

    memset(r, 0, sizeof *r);
    r->sequence = fz_rng_chance(rng, 300U) ? fz_rng_edge32(rng) : (uint32_t)fz_rng_next(rng);
    r->confirmed = fz_rng_chance(rng, 500U) ? (uint8_t)BOOT_SLOT_A : (uint8_t)BOOT_SLOT_B;
    r->trial = fz_rng_chance(rng, 500U) ? (uint8_t)BOOT_SLOT_NONE
                                        : (uint8_t)((r->confirmed == BOOT_SLOT_A) ? BOOT_SLOT_B
                                                    : BOOT_SLOT_A);
    r->failed_starts = (uint8_t)fz_rng_below(rng, BOOT_STATE_MAX_STARTS + 1U);
    r->flags = (uint8_t)fz_rng_below(rng, 2U);
    r->given_up_check = fz_rng_chance(rng, 500U) ? 0U : (uint32_t)fz_rng_next(rng);
    switch (kind) {
    case 1U:
        switch (fz_rng_below(rng, 5U)) {
        case 0U:
            r->confirmed = odd_slot[fz_rng_below(rng, sizeof odd_slot)];
            break;
        case 1U:
            r->trial = (uint8_t)((fz_rng_chance(rng, 500U)) ? r->confirmed
                                 : odd_slot[fz_rng_below(rng, sizeof odd_slot)]);
            break;
        case 2U:
            r->failed_starts = odd_count[fz_rng_below(rng, sizeof odd_count)];
            break;
        case 3U:
            r->flags = odd_flags[fz_rng_below(rng, sizeof odd_flags)];
            break;
        case 4U:
            r->confirmed = odd_slot[fz_rng_below(rng, sizeof odd_slot)];
            r->trial = odd_slot[fz_rng_below(rng, sizeof odd_slot)];
            r->failed_starts = odd_count[fz_rng_below(rng, sizeof odd_count)];
            r->flags = odd_flags[fz_rng_below(rng, sizeof odd_flags)];
            break;
        }
        set_crc(r);
        break;
    case 2U:
        set_crc(r);
        for (uint32_t i = 1U + fz_rng_below(rng, 3U); i > 0U; i--) {
            b[fz_rng_below(rng, RECORD_BYTES)] ^= (uint8_t)(1U << fz_rng_below(rng, 8U));
        }
        break;
    case 3U:
        memset(r, SLOT_ERASED, sizeof *r);
        break;
    case 4U:
        memset(r, SLOT_ERASED, sizeof *r);
        b[fz_rng_below(rng, RECORD_BYTES)] = (uint8_t)fz_rng_next(rng);
        break;
    case 5U:
        memset(r, 0, sizeof *r);
        break;
    case 6U:
        for (uint32_t i = 0U; i < RECORD_BYTES; i++) {
            b[i] = (uint8_t)fz_rng_next(rng);
        }
        break;
    case 7U:
        set_crc(r);
        r->check ^= (uint32_t)(1ULL << fz_rng_below(rng, 32U));
        break;
    case 0U:
    case 8U:
    case 9U:
        set_crc(r);
        break;
    }
}

/**
 * @brief Counts a copy's validity for the table and for the oracle.
 *
 * @param[in]     r         the record
 * @param[in]     readable  false if reading the copy raises a flash ECC error
 * @param[out]    impossible  set true if the CRC is right and the fields cannot exist
 * @return true if the copy is valid
 */
static bool ref_valid(const boot_state_t *r, bool readable, bool *impossible)
{
    if (readable && ref_crc_ok(r) && !ref_fields_ok(r)) {
        *impossible = true;
    }
    return readable && ref_crc_ok(r) && ref_fields_ok(r);
}

/**
 * @brief Compares two records field by field.
 *
 * @param[in]     a  one record
 * @param[in]     b  the other
 * @return true if all 16 bytes are equal
 */
static bool same_record(const boot_state_t *a, const boot_state_t *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

/**
 * @brief Returns true if a record still holds the 0xA5 fill the target gave it.
 *
 * @param[in]     r  the record
 * @return true if every byte is 0xA5
 */
static bool is_sentinel(const boot_state_t *r)
{
    const uint8_t *b = (const uint8_t *)r;

    for (uint32_t i = 0U; i < RECORD_BYTES; i++) {
        if (b[i] != SENTINEL_BYTE) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Builds the area for one input and puts it in the RAM flash.
 *
 * @param[in,out] rng        the generator
 * @param[out]    copies     the two records as stored
 * @param[out]    readable   false for a copy given an ECC range
 */
static void build_area(fz_rng_t *rng, boot_state_t copies[COPIES], bool readable[COPIES])
{
    uint32_t kinds[COPIES];

    for (uint32_t c = 0U; c < COPIES; c++) {
        kinds[c] = fz_rng_below(rng, KIND_COUNT);
        make_record(rng, kinds[c], &copies[c]);
    }
    if (fz_rng_chance(rng, CHANCE_TIE) && (kinds[1] != 3U) && (kinds[1] != 5U)) {
        uint32_t how = fz_rng_below(rng, 4U);
        copies[1].sequence = (how == 0U) ? copies[0].sequence
                           : (how == 1U) ? (copies[0].sequence + 1U)
                           : (how == 2U) ? (copies[0].sequence - 1U) : 0xFFFFFFFFU;
        if (how == 3U) {
            copies[0].sequence = 0U;
        }
        if ((kinds[1] == 0U) || (kinds[1] == 1U) || (kinds[1] == 8U) || (kinds[1] == 9U)) {
            set_crc(&copies[1]);
        }
        if ((kinds[0] == 0U) || (kinds[0] == 1U) || (kinds[0] == 8U) || (kinds[0] == 9U)) {
            set_crc(&copies[0]);
        }
    }
    if (fz_rng_chance(rng, 100U)) {   /* two identical copies */
        copies[1] = copies[0];
    }
    ramflash_reset();
    ramflash_clear_ecc();
    for (uint32_t c = 0U; c < COPIES; c++) {
        memcpy(ramflash_ptr(COPY_START[c]), &copies[c], sizeof copies[c]);
        readable[c] = true;
    }
    if (fz_rng_chance(rng, CHANCE_ECC)) {
        uint32_t which = 1U + fz_rng_below(rng, 3U);
        for (uint32_t c = 0U; c < COPIES; c++) {
            if ((which & (1U << c)) != 0U) {
                ramflash_set_ecc(c, COPY_START[c], COPY_BYTES);
                readable[c] = false;
            }
        }
        g_ecc_cases++;
    }
}

/**
 * @brief Runs one input.
 *
 * @param[in]     seed   run seed
 * @param[in]     index  input index
 */
static void run_input(uint64_t seed, uint64_t index)
{
    fz_rng_t rng;
    boot_state_t copies[COPIES];
    bool readable[COPIES];
    boot_state_t sc;
    boot_state_t sb;
    boot_state_t rc;
    boot_state_t rb;
    bool fault_c = false;
    bool fault_b = false;
    bool fault_rc = false;
    bool fault_rb = false;
    boot_state_load_t lc = BOOT_STATE_DAMAGED;
    boot_state_load_t lb = BOOT_STATE_DAMAGED;
    bool read_c = false;
    bool read_b = false;
    char note[240];

    fz_rng_seed(&rng, seed, index);
    build_area(&rng, copies, readable);
    fz_input_begin(index, ramflash_ptr(BOOT_STATE_START), AREA_BYTES);
    memset(&sc, SENTINEL_BYTE, sizeof sc);
    memset(&sb, SENTINEL_BYTE, sizeof sb);
    memset(&rc, SENTINEL_BYTE, sizeof rc);
    memset(&rb, SENTINEL_BYTE, sizeof rb);
    fz_fault_clear();
    FZ_FAULT_GUARD(fault_c, lc = boot_state_load(&sc));
    FZ_FAULT_GUARD(fault_b, lb = bl_boot_state_load(&sb));
    FZ_FAULT_GUARD(fault_rc, read_c = boot_state_read(&rc));
    FZ_FAULT_GUARD(fault_rb, read_b = bl_boot_state_read(&rb));
    g_faults += fz_fault_calls;

    bool impossible = false;
    bool v0 = ref_valid(&copies[0], readable[0], &impossible);
    bool v1 = ref_valid(&copies[1], readable[1], &impossible);
    uint32_t valid_count = (v0 ? 1U : 0U) + (v1 ? 1U : 0U);
    int newest = (v0 && (!v1 || (copies[0].sequence >= copies[1].sequence))) ? 0 : (v1 ? 1 : -1);
    boot_state_load_t expect = (newest >= 0) ? BOOT_STATE_VALID
                             : ((readable[0] && readable[1] && ref_blank(&copies[0])
                                 && ref_blank(&copies[1]))
                                    ? BOOT_STATE_BLANK : BOOT_STATE_DAMAGED);

    if (impossible) {
        g_crc_impossible_present++;
    }
    if (!fault_c && !fault_b) {
        g_table[valid_count][impossible ? 1 : 0][(int)lc]++;
    }
    if (fault_c || fault_b || fault_rc || fault_rb) {
        (void)snprintf(note, sizeof note,
                       "fault reaction reached by a stored value (load: common %d bootloader %d; "
                       "read: common %d bootloader %d)",
                       fault_c, fault_b, fault_rc, fault_rb);
        fz_fail("fault-from-flash", index, ramflash_ptr(BOOT_STATE_START), AREA_BYTES, note);
    } else {
        if ((lc != lb) || (lc != expect)) {
            (void)snprintf(note, sizeof note, "load: common %d, bootloader %d, reference %d",
                           (int)lc, (int)lb, (int)expect);
            fz_fail("load-differs", index, ramflash_ptr(BOOT_STATE_START), AREA_BYTES, note);
        }
        if ((read_c != read_b) || (read_c != (newest >= 0))) {
            (void)snprintf(note, sizeof note, "read: common %d, bootloader %d, reference %d",
                           read_c, read_b, newest >= 0);
            fz_fail("read-differs", index, ramflash_ptr(BOOT_STATE_START), AREA_BYTES, note);
        }
        if ((lc == BOOT_STATE_VALID) && (lb == BOOT_STATE_VALID) &&
            (!same_record(&sc, &sb) || (newest < 0) || !same_record(&sc, &copies[newest]))) {
            fz_fail("state-differs", index, ramflash_ptr(BOOT_STATE_START), AREA_BYTES,
                    "VALID, but the state differs from the newest valid copy");
        }
        if (read_c && read_b && (!same_record(&rc, &rb) || (newest < 0)
                                 || !same_record(&rc, &copies[newest]))) {
            fz_fail("read-state-differs", index, ramflash_ptr(BOOT_STATE_START), AREA_BYTES,
                    "read state differs from the newest valid copy");
        }
        if (((lc != BOOT_STATE_VALID) && !is_sentinel(&sc))
            || ((lb != BOOT_STATE_VALID) && !is_sentinel(&sb))) {
            fz_fail("state-written-on-failure", index, ramflash_ptr(BOOT_STATE_START), AREA_BYTES,
                    "load wrote *state although it did not return VALID");
        }
    }
    if (fz_rng_chance(&rng, CHANCE_WRITE)) {
        static uint8_t area[AREA_BYTES];
        static uint8_t after_c[AREA_BYTES];
        boot_state_t w = {0};
        bool ok_c = false;
        bool ok_b = false;
        bool f1 = false;
        bool f2 = false;

        memcpy(area, ramflash_ptr(BOOT_STATE_START), AREA_BYTES);
        w.confirmed = fz_rng_chance(&rng, 900U) ? (uint8_t)((fz_rng_below(&rng, 2U) != 0U)
                                                            ? BOOT_SLOT_A : BOOT_SLOT_B)
                                                : (uint8_t)fz_rng_below(&rng, 4U);
        w.trial = (uint8_t)fz_rng_below(&rng, 4U);
        w.failed_starts = (uint8_t)fz_rng_below(&rng, 6U);
        w.flags = (uint8_t)fz_rng_below(&rng, 3U);
        w.given_up_check = (uint32_t)fz_rng_next(&rng);
        boot_state_t w2 = w;
        ramflash_clear_ecc();
        FZ_FAULT_GUARD(f1, ok_c = boot_state_write(&w));
        memcpy(after_c, ramflash_ptr(BOOT_STATE_START), AREA_BYTES);
        memcpy(ramflash_ptr(BOOT_STATE_START), area, AREA_BYTES);
        FZ_FAULT_GUARD(f2, ok_b = bl_boot_state_write(&w2));
        g_write_compared++;
        if ((f1 != f2) || (ok_c != ok_b) || (memcmp(after_c, ramflash_ptr(BOOT_STATE_START),
                                                    AREA_BYTES) != 0) ||
            (ok_c && !same_record(&w, &w2))) {
            (void)snprintf(note, sizeof note,
                           "write: common ok %d fault %d, bootloader ok %d fault %d, flash or "
                           "state differs", ok_c, f1, ok_b, f2);
            fz_fail("write-differs", index, area, AREA_BYTES, note);
        }
    }
    fz_input_end();
}

/**
 * @brief Prints the outcome table.
 */
static void log_table(void)
{
    static const char *const valid_name[3] = {"no valid copy", "one valid copy",
                                              "two valid copies"};
    static const char *const result_name[3] = {"VALID", "BLANK", "DAMAGED"};

    fz_log("outcome of boot_state_load (both implementations agree on every input counted here):");
    for (uint32_t v = 0U; v < 3U; v++) {
        for (uint32_t imp = 0U; imp < 2U; imp++) {
            fz_log("  %-16s%s: VALID %llu, BLANK %llu, DAMAGED %llu", valid_name[v],
                   imp ? ", a CRC-valid copy with impossible fields present"
                       : ", no such copy              ",
                   (unsigned long long)g_table[v][imp][0], (unsigned long long)g_table[v][imp][1],
                   (unsigned long long)g_table[v][imp][2]);
        }
    }
    (void)result_name;
    fz_log("inputs with a CRC-valid copy whose fields cannot exist: %llu; fault reactions reached "
           "by "
           "stored values: %llu; inputs with a flash ECC error on a copy: %llu; writes compared: "
               "%llu",
           (unsigned long long)g_crc_impossible_present, (unsigned long long)g_faults,
           (unsigned long long)g_ecc_cases, (unsigned long long)g_write_compared);
}

/**
 * @brief Runs the boot-state fuzz target.
 *
 * @param[in]     argc  argument count
 * @param[in]     argv  arguments: see fz_options_parse
 * @return 0 if no input failed, 1 otherwise
 */
int main(int argc, char **argv)
{
    fz_options_t options;
    uint64_t first = 0U;
    uint64_t last;

    fz_options_parse(&options, argc, argv);
    fz_run_open("fz_bootstate", &options, argc, argv);
    last = options.count;
    if (options.one) {
        first = options.one_index;
        last = first + 1U;
    }
    for (uint64_t index = first; index < last; index++) {
        run_input(options.seed, index);
    }
    log_table();
    return fz_run_finish(last - first);
}
