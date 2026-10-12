/*
 * test_update.c — host unit tests of the update receiver
 * (src/controller/firmware/update-receiver/update_receiver.c) against Interfaces Sec. 4.2
 * (answers, refusal codes), Sec. 5.1 (UPLOAD commands) and REQ-007, with the chip replaced by
 * fake_port.c: a RAM copy of the flash in which every region holds its own byte pattern, so a
 * stray erase or write shows.
 *
 * The last group sends the build's real signed images through the receiver into the fake flash;
 * UPLOAD END then runs the bootloader's own checks on them (common/image_check.c, Monocypher).
 *
 * Built with the Mac's cc and the undefined-behaviour sanitizer (Monocypher without it).
 * Run: make -C test/controller unit. Prints PASS or FAIL per test; exit status 1 on a failure.
 * Usage: test_update <dir with firmware-A.img, firmware-B.img> <dir with firmware-B-wrong-key-a.img>
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fake_port.h"
#include "frame.h"
#include "image_check.h"
#include "image_format.h"
#include "layout.h"
#include "update_receiver.h"

/* Payload layout (Interfaces Sec. 5.1), written here byte by byte, apart from the parser. */
#define CHUNK_HEAD        7U
#define PAYLOAD_ROOM      (CHUNK_HEAD + UPLOAD_CHUNK_MAX + 8U)
/* A length that ends in a partial flash word, for the sequencing tests. */
#define TEST_LENGTH       1001U
/* Same code byte as the bench's damaged image (test/controller/Makefile, FLIP_OFFSET). */
#define FLIP_OFFSET       0x600U
#define ERASED            0xFFU
#define PAGES_PER_SLOT    ((uint32_t)SLOT_BYTES / (uint32_t)FLASH_PAGE_BYTES)
/* Address sweep for the barrier test: the whole flash and 8 KB either side. */
#define SWEEP_MARGIN      0x2000U

static uint32_t failures;
static uint32_t tests;

static void report(bool ok, const char *name)
{
    tests++;
    if (!ok) {
        failures++;
    }
    printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
}

/* ---- Payloads ------------------------------------------------------------------------ */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    for (uint32_t i = 0U; i < 4U; i++) {
        p[i] = (uint8_t)(v >> (8U * i));
    }
}

/* What main.c answers: REFUSED with the code from update_accept(), or RECEIVED and then the
 * result of update_run() (0x00 = DONE). */
typedef struct {
    bool received;
    uint8_t code;
} answer_t;

static answer_t send(update_receiver_t *rx, const uint8_t *payload, uint16_t length)
{
    update_command_t cmd;
    answer_t a = {false, update_accept(rx, payload, length, &cmd)};

    if (a.code == UPDATE_OK) {
        a.received = true;
        a.code = update_run(rx, &cmd);
    }
    return a;
}

static answer_t begin(update_receiver_t *rx, uint16_t number, uint32_t length)
{
    uint8_t p[CHUNK_HEAD];

    p[0] = CMD_UPLOAD_BEGIN;
    put_u16(&p[1], number);
    put_u32(&p[3], length);
    return send(rx, p, sizeof p);
}

static answer_t chunk(update_receiver_t *rx, uint16_t number, uint32_t offset,
                      const uint8_t *data, uint32_t n)
{
    uint8_t p[PAYLOAD_ROOM];

    p[0] = CMD_UPLOAD_CHUNK;
    put_u16(&p[1], number);
    put_u32(&p[3], offset);
    memcpy(&p[CHUNK_HEAD], data, n);
    return send(rx, p, (uint16_t)(CHUNK_HEAD + n));
}

static answer_t short_command(update_receiver_t *rx, uint8_t id, uint16_t number)
{
    uint8_t p[3];

    p[0] = id;
    put_u16(&p[1], number);
    return send(rx, p, sizeof p);
}

static bool done(answer_t a)
{
    return a.received && (a.code == UPDATE_OK);
}

/* Refused on arrival: no RECEIVED, only REFUSED with this code. */
static bool refused_only(answer_t a, uint8_t code)
{
    return !a.received && (a.code == code);
}

/* RECEIVED, then REFUSED with this code. */
static bool received_then_refused(answer_t a, uint8_t code)
{
    return a.received && (a.code == code);
}

/* The whole image in 256-byte chunks, stop-and-wait as the onboard computer sends it. The END
 * answer, or the first answer that was not DONE. */
static answer_t upload(update_receiver_t *rx, const uint8_t *image, uint32_t size, uint16_t number)
{
    answer_t a = begin(rx, number, size);

    for (uint32_t offset = 0U; done(a) && (offset < size); offset += UPLOAD_CHUNK_MAX) {
        uint32_t n = ((size - offset) < UPLOAD_CHUNK_MAX) ? (size - offset) : UPLOAD_CHUNK_MAX;
        a = chunk(rx, number, offset, &image[offset], n);
    }
    return done(a) ? short_command(rx, CMD_UPLOAD_END, number) : a;
}

/* Every byte outside `written_slot` still holds its region's pattern (fake_port.c). */
static bool others_untouched(uint8_t written_slot)
{
    uint32_t changed = fake_count_not(BOOTLOADER_START, BOOTLOADER_BYTES, 0xB0U) +
                       fake_count_not(LAST_RESORT_START, LAST_RESORT_BYTES, 0x1EU) +
                       fake_count_not(BOOT_STATE_START, 2U * BOOT_STATE_COPY_BYTES, 0x5EU);
    if (written_slot != BOOT_SLOT_A) {
        changed += fake_count_not(SLOT_A_START, SLOT_BYTES, 0xAAU);
    }
    if (written_slot != BOOT_SLOT_B) {
        changed += fake_count_not(SLOT_B_START, SLOT_BYTES, 0xBBU);
    }
    return (changed == 0U) && (fake.outside == 0U) && (fake.bad_reads == 0U);
}

static void fill_pattern(uint8_t *data, uint32_t n)
{
    for (uint32_t i = 0U; i < n; i++) {
        data[i] = (uint8_t)((i * 7U) + 1U);   /* first word not 0xFFFFFFFF: not "empty" */
    }
}

/* Running slot A, confirmed A, no trial: the upload target is slot B. */
static void start_running_a(update_receiver_t *rx)
{
    fake_reset();
    update_init(rx, BOOT_SLOT_A);
}

/* ---- Slot choice ----------------------------------------------------------------------- */

static void test_slot_choice(void)
{
    boot_state_t s = {0};
    uint8_t t = 0xEEU;

    s.confirmed = BOOT_SLOT_A;
    report((update_choose_target(true, &s, BOOT_SLOT_A, &t) == UPDATE_OK) && (t == BOOT_SLOT_B),
           "slot choice: confirmed A, running A -> target B");
    s.confirmed = BOOT_SLOT_B;
    report((update_choose_target(true, &s, BOOT_SLOT_B, &t) == UPDATE_OK) && (t == BOOT_SLOT_A),
           "slot choice: confirmed B, running B -> target A");
    s.confirmed = BOOT_SLOT_A;
    s.trial = BOOT_SLOT_B;
    bool trial_other = (update_choose_target(true, &s, BOOT_SLOT_A, &t) == REFUSED_TRIAL) &&
                       (t == BOOT_SLOT_NONE);
    bool trial_running = (update_choose_target(true, &s, BOOT_SLOT_B, &t) == REFUSED_TRIAL) &&
                         (t == BOOT_SLOT_NONE);
    report(trial_other && trial_running,
           "slot choice: trial set -> refused 0x07, running the trial or the confirmed slot");
    s.trial = BOOT_SLOT_NONE;
    s.confirmed = BOOT_SLOT_B;
    report((update_choose_target(true, &s, BOOT_SLOT_A, &t) == REFUSED_IMPOSSIBLE) &&
               (t == BOOT_SLOT_NONE),
           "slot choice: running A, confirmed B, no trial -> no slot is free, refused 0x06");
    report((update_choose_target(false, &s, BOOT_SLOT_A, &t) == REFUSED_IMPOSSIBLE) &&
               (update_choose_target(true, &s, BOOT_SLOT_NONE, &t) == REFUSED_IMPOSSIBLE),
           "slot choice: no valid boot state, or running slot not known -> refused 0x06");

    update_receiver_t rx;
    start_running_a(&rx);
    fake.state.trial = BOOT_SLOT_B;
    answer_t a[4] = {begin(&rx, 1U, TEST_LENGTH), short_command(&rx, CMD_UPLOAD_END, 1U),
                     short_command(&rx, CMD_UPLOAD_CANCEL, 1U), {false, 0U}};
    uint8_t one = 0x11U;
    a[3] = chunk(&rx, 1U, 0U, &one, 1U);
    bool all_07 = true;
    for (uint32_t i = 0U; i < 4U; i++) {
        all_07 = all_07 && refused_only(a[i], REFUSED_TRIAL);
    }
    report(all_07 && (fake.erases == 0U) && (fake.programs == 0U) && others_untouched(BOOT_SLOT_NONE),
           "during a trial BEGIN, CHUNK, END, CANCEL: REFUSED 0x07 only, flash untouched");
}

static void test_slot_of(void)
{
    struct {
        uint32_t address;
        uint8_t slot;
    } cases[] = {
        {FLASH_START, BOOT_SLOT_NONE},
        {SLOT_A_START - 1U, BOOT_SLOT_NONE},
        {SLOT_A_START, BOOT_SLOT_A},
        {SLOT_A_CODE_START, BOOT_SLOT_A},
        {SLOT_A_START + SLOT_BYTES - 1U, BOOT_SLOT_A},
        {SLOT_B_START, BOOT_SLOT_B},
        {SLOT_B_CODE_START, BOOT_SLOT_B},
        {SLOT_B_START + SLOT_BYTES - 1U, BOOT_SLOT_B},
        {LAST_RESORT_CODE_START, BOOT_SLOT_NONE},
        {0U, BOOT_SLOT_NONE},
        {0xFFFFFFFFU, BOOT_SLOT_NONE},
    };
    bool ok = true;

    for (uint32_t i = 0U; i < sizeof cases / sizeof cases[0]; i++) {
        ok = ok && (update_slot_of(cases[i].address) == cases[i].slot);
    }
    report(ok, "running slot from the vector table address: A, B, or none (edges included)");
}

/* ---- The address barrier ------------------------------------------------------------- */

static bool overlaps(uint64_t a, uint64_t n, uint64_t start, uint64_t bytes)
{
    return (a < (start + bytes)) && (start < (a + n));
}

/* What update_write_allowed() must answer, worked out in 64 bits so nothing wraps. */
static bool expected_allowed(uint8_t target, uint8_t running, uint64_t a, uint64_t n)
{
    uint64_t start = (target == BOOT_SLOT_A) ? SLOT_A_START : SLOT_B_START;
    bool real = (target == BOOT_SLOT_A) || (target == BOOT_SLOT_B);

    return real && (target != running) && (n > 0U) && (a >= start) &&
           ((a + n) <= (start + (uint64_t)SLOT_BYTES));
}

static bool touches_protected(uint8_t running, uint64_t a, uint64_t n)
{
    uint64_t run = (running == BOOT_SLOT_A) ? SLOT_A_START : SLOT_B_START;

    return overlaps(a, n, BOOTLOADER_START, BOOTLOADER_BYTES) ||
           overlaps(a, n, LAST_RESORT_START, LAST_RESORT_BYTES) ||
           overlaps(a, n, BOOT_STATE_START, 2U * (uint64_t)BOOT_STATE_COPY_BYTES) ||
           ((running != BOOT_SLOT_NONE) && overlaps(a, n, run, SLOT_BYTES));
}

static void test_address_barrier(void)
{
    static const uint8_t targets[] = {BOOT_SLOT_NONE, BOOT_SLOT_A, BOOT_SLOT_B, 3U, 0xFFU};
    static const uint8_t runnings[] = {BOOT_SLOT_NONE, BOOT_SLOT_A, BOOT_SLOT_B};
    static const uint32_t sizes[] = {0U, 1U, 8U, 256U, FLASH_PAGE_BYTES, SLOT_BYTES, SLOT_BYTES + 1U};
    static const uint32_t extremes[] = {0U, 8U, 0x7FFFFFFFU, 0xFFFFF800U, 0xFFFFFFF8U, 0xFFFFFFFFU};
    uint64_t first = (uint64_t)FLASH_START - SWEEP_MARGIN;
    uint64_t last = (uint64_t)FLASH_START + FAKE_FLASH_BYTES + SWEEP_MARGIN;
    uint32_t cases = 0U;
    uint32_t wrong = 0U;
    uint32_t protected_hits = 0U;

    for (uint32_t t = 0U; t < sizeof targets; t++) {
        for (uint32_t r = 0U; r < sizeof runnings; r++) {
            for (uint32_t s = 0U; s < sizeof sizes / sizeof sizes[0]; s++) {
                for (uint64_t a = first; a < last + (sizeof extremes / sizeof extremes[0]); a++) {
                    uint64_t address = (a < last) ? a : extremes[a - last];
                    bool got = update_write_allowed(targets[t], runnings[r], (uint32_t)address,
                                                    sizes[s]);
                    cases++;
                    wrong += (got != expected_allowed(targets[t], runnings[r], address, sizes[s]))
                                 ? 1U : 0U;
                    protected_hits += (got && touches_protected(runnings[r], address, sizes[s]))
                                          ? 1U : 0U;
                }
            }
        }
    }
    char line[200];
    snprintf(line, sizeof line,
             "address barrier: %u cases (every byte address of the flash +-8 KB), %u wrong; "
             "%u allowed ranges touch bootloader/running slot/last-resort/boot state",
             cases, wrong, protected_hits);
    report((wrong == 0U) && (protected_hits == 0U), line);
}

/* ---- Answer codes -------------------------------------------------------------------- */

static void test_result_codes(void)
{
    struct {
        image_result_t result;
        uint8_t code;
    } cases[] = {
        {IMAGE_OK, UPDATE_OK},
        {IMAGE_EMPTY, REFUSED_DAMAGED},
        {IMAGE_BAD_MAGIC, REFUSED_DAMAGED},
        {IMAGE_BAD_FORMAT, REFUSED_DAMAGED},
        {IMAGE_BAD_KIND, REFUSED_DAMAGED},
        {IMAGE_BAD_TARGET, REFUSED_OTHER_CONTROLLER},
        {IMAGE_BAD_LENGTH, REFUSED_DAMAGED},
        {IMAGE_BAD_START, REFUSED_OTHER_SLOT},
        {IMAGE_BAD_BODY_CHECK, REFUSED_DAMAGED},
        {IMAGE_BAD_SIGNATURE_A, REFUSED_NOT_OPERATOR},
        {IMAGE_BAD_SIGNATURE_B, REFUSED_NOT_OPERATOR},
    };
    bool ok = (REFUSED_DAMAGED == 0x08U) && (REFUSED_NOT_OPERATOR == 0x09U) &&
              (REFUSED_OTHER_CONTROLLER == 0x0BU) && (REFUSED_OTHER_SLOT == 0x0EU) &&
              (REFUSED_TRIAL == 0x07U) && (REFUSED_IMPOSSIBLE == 0x06U);

    for (uint32_t i = 0U; i < sizeof cases / sizeof cases[0]; i++) {
        ok = ok && (update_refusal_for(cases[i].result) == cases[i].code);
    }
    report(ok, "image check result -> code: passed 0x00, body/structure 0x08, signature 0x09, "
               "target 0x0B, start address 0x0E (all 11 results)");
}

/* ---- Payload sizes and length bounds ------------------------------------------------- */

static void test_payload_sizes(void)
{
    update_receiver_t rx;
    uint8_t p[PAYLOAD_ROOM + 2U] = {0};
    struct {
        uint8_t id;
        uint16_t length;
    } bad[] = {
        {CMD_UPLOAD_BEGIN, 6U}, {CMD_UPLOAD_BEGIN, 8U}, {CMD_UPLOAD_CHUNK, CHUNK_HEAD},
        {CMD_UPLOAD_CHUNK, CHUNK_HEAD + UPLOAD_CHUNK_MAX + 1U}, {CMD_UPLOAD_END, 2U},
        {CMD_UPLOAD_END, 4U}, {CMD_UPLOAD_CANCEL, 2U}, {CMD_UPLOAD_CANCEL, 4U}, {0x05U, 3U},
    };
    bool ok = true;

    start_running_a(&rx);
    for (uint32_t i = 0U; i < sizeof bad / sizeof bad[0]; i++) {
        p[0] = bad[i].id;
        ok = ok && refused_only(send(&rx, p, bad[i].length), REFUSED_IMPOSSIBLE);
    }
    ok = ok && refused_only(send(&rx, p, 0U), REFUSED_IMPOSSIBLE);
    report(ok && (fake.erases == 0U),
           "wrong payload sizes (BEGIN 6/8, CHUNK 0/257 data bytes, END/CANCEL 2/4, empty, "
           "id 0x05): REFUSED 0x06 only");

    static const uint8_t ids[] = {0x00U, 0x01U, 0x04U, 0x05U, 0x25U};
    static const bool is_upload[] = {false, true, true, false, false};
    ok = !update_is_upload(p, 0U);
    for (uint32_t i = 0U; i < sizeof ids; i++) {
        p[0] = ids[i];
        ok = ok && (update_is_upload(p, 1U) == is_upload[i]);
    }
    report(ok, "upload commands are identifiers 0x01 to 0x04 only (Interfaces Sec. 5.1)");
}

static void test_length_bounds(void)
{
    update_receiver_t rx;
    uint32_t smallest = (uint32_t)IMAGE_HEADER_BYTES + (uint32_t)IMAGE_SIGNATURES_BYTES;

    start_running_a(&rx);
    bool low = refused_only(begin(&rx, 1U, 0U), REFUSED_IMPOSSIBLE) &&
               refused_only(begin(&rx, 1U, smallest - 1U), REFUSED_IMPOSSIBLE);
    bool high = refused_only(begin(&rx, 1U, SLOT_BYTES + 1U), REFUSED_IMPOSSIBLE) &&
                refused_only(begin(&rx, 1U, 0xFFFFFFFFU), REFUSED_IMPOSSIBLE);
    report(low && high && (fake.erases == 0U),
           "BEGIN length 0, 191, slot size + 1, 2^32 - 1: REFUSED 0x06 only, nothing erased");

    bool fits = done(begin(&rx, 1U, smallest));
    uint32_t erased_once = fake.erases;
    fits = fits && done(begin(&rx, 2U, SLOT_BYTES));
    report(fits && (erased_once == PAGES_PER_SLOT) && (fake.erases == 2U * PAGES_PER_SLOT) &&
               (fake_count_not(SLOT_B_START, SLOT_BYTES, ERASED) == 0U) &&
               (fake.reloads >= fake.erases) && others_untouched(BOOT_SLOT_B),
           "BEGIN length 192 and 40960: DONE after all 20 pages of slot B are erased, watchdog "
           "reloaded after each page, nothing else touched");
}

/* ---- Offset sequencing --------------------------------------------------------------- */

static void test_offsets(void)
{
    update_receiver_t rx;
    static uint8_t data[TEST_LENGTH];
    uint8_t one = 0x11U;

    fill_pattern(data, sizeof data);
    start_running_a(&rx);
    report(refused_only(chunk(&rx, 1U, 0U, data, UPLOAD_CHUNK_MAX), REFUSED_IMPOSSIBLE) &&
               refused_only(short_command(&rx, CMD_UPLOAD_END, 1U), REFUSED_IMPOSSIBLE) &&
               refused_only(short_command(&rx, CMD_UPLOAD_CANCEL, 1U), REFUSED_IMPOSSIBLE),
           "CHUNK, END, CANCEL before any BEGIN: REFUSED 0x06 only");

    bool ok = done(begin(&rx, 1U, TEST_LENGTH));
    ok = ok && refused_only(chunk(&rx, 1U, 8U, data, UPLOAD_CHUNK_MAX), REFUSED_IMPOSSIBLE);
    ok = ok && done(chunk(&rx, 1U, 0U, data, UPLOAD_CHUNK_MAX));
    report(ok, "first CHUNK must start at offset 0; offset 0 then DONE");

    report(refused_only(chunk(&rx, 1U, 0U, data, UPLOAD_CHUNK_MAX), REFUSED_IMPOSSIBLE) &&
               refused_only(chunk(&rx, 1U, 512U, &data[512], UPLOAD_CHUNK_MAX), REFUSED_IMPOSSIBLE),
           "CHUNK repeating offset 0, or skipping to 512 (expected 256): REFUSED 0x06 only");
    report(refused_only(chunk(&rx, 1U, 256U, &data[256], 100U), REFUSED_IMPOSSIBLE),
           "CHUNK of 100 bytes that is not the last: REFUSED 0x06 (whole flash words only)");
    report(refused_only(chunk(&rx, 2U, 256U, &data[256], UPLOAD_CHUNK_MAX), REFUSED_IMPOSSIBLE),
           "CHUNK with another image number: REFUSED 0x06 only");
    report(refused_only(short_command(&rx, CMD_UPLOAD_END, 1U), REFUSED_IMPOSSIBLE),
           "END before the last byte: REFUSED 0x06 only");

    ok = done(chunk(&rx, 1U, 256U, &data[256], UPLOAD_CHUNK_MAX)) &&
         done(chunk(&rx, 1U, 512U, &data[512], UPLOAD_CHUNK_MAX));
    ok = ok && refused_only(chunk(&rx, 1U, 768U, &data[768], 240U), REFUSED_IMPOSSIBLE);
    ok = ok && done(chunk(&rx, 1U, 768U, &data[768], TEST_LENGTH - 768U));
    ok = ok && refused_only(chunk(&rx, 1U, TEST_LENGTH, &one, 1U), REFUSED_IMPOSSIBLE);
    report(ok, "CHUNK past the announced length refused 0x06; last partial chunk (233 bytes) "
               "DONE; a CHUNK after the last byte refused 0x06");

    const uint8_t *slot = &fake.flash[SLOT_B_START - FLASH_START];
    report((memcmp(slot, data, TEST_LENGTH) == 0) &&
               (fake_count_not(SLOT_B_START + TEST_LENGTH, SLOT_BYTES - TEST_LENGTH, ERASED) == 0U) &&
               others_untouched(BOOT_SLOT_B),
           "slot B holds the 1001 bytes in order, the rest of the slot erased (last flash word "
           "padded with 0xFF), nothing else touched");

    report(received_then_refused(short_command(&rx, CMD_UPLOAD_END, 1U), REFUSED_DAMAGED) &&
               !rx.image_passed,
           "END on bytes that are not an image: RECEIVED, then REFUSED 0x08");
    report(refused_only(chunk(&rx, 1U, TEST_LENGTH, &one, 1U), REFUSED_IMPOSSIBLE) &&
               refused_only(short_command(&rx, CMD_UPLOAD_END, 1U), REFUSED_IMPOSSIBLE),
           "after END: CHUNK and END refused 0x06 (transfer closed)");

    ok = refused_only(short_command(&rx, CMD_UPLOAD_CANCEL, 9U), REFUSED_IMPOSSIBLE);
    ok = ok && done(short_command(&rx, CMD_UPLOAD_CANCEL, 1U));
    ok = ok && (fake_count_not(SLOT_B_START, FLASH_PAGE_BYTES, ERASED) == 0U);
    ok = ok && refused_only(short_command(&rx, CMD_UPLOAD_CANCEL, 1U), REFUSED_IMPOSSIBLE);
    uint32_t signed_bytes = 0U;
    ok = ok && (image_check_contents(slot, SLOT_B_START, SLOT_BYTES, IMAGE_KIND_MAIN,
                                     &signed_bytes) == IMAGE_EMPTY);
    report(ok && others_untouched(BOOT_SLOT_B),
           "CANCEL with another number refused 0x06; CANCEL DONE erases slot B's first page "
           "(bootloader check: empty); a second CANCEL refused 0x06");
}

static void test_begin_restarts(void)
{
    update_receiver_t rx;
    static uint8_t data[TEST_LENGTH];

    fill_pattern(data, sizeof data);
    start_running_a(&rx);
    bool ok = done(begin(&rx, 1U, TEST_LENGTH)) && done(chunk(&rx, 1U, 0U, data, UPLOAD_CHUNK_MAX));
    ok = ok && done(begin(&rx, 2U, TEST_LENGTH));
    ok = ok && (fake_count_not(SLOT_B_START, SLOT_BYTES, ERASED) == 0U);
    ok = ok && refused_only(chunk(&rx, 1U, 256U, &data[256], UPLOAD_CHUNK_MAX), REFUSED_IMPOSSIBLE);
    ok = ok && done(chunk(&rx, 2U, 0U, data, UPLOAD_CHUNK_MAX));
    report(ok, "BEGIN during an open transfer restarts it: slot erased again, old image number "
               "refused, new one starts at offset 0");
}

static void test_trial_mid_transfer(void)
{
    update_receiver_t rx;
    static uint8_t data[TEST_LENGTH];

    fill_pattern(data, sizeof data);
    start_running_a(&rx);
    bool ok = done(begin(&rx, 1U, TEST_LENGTH));
    uint32_t programs = fake.programs;
    fake.state.trial = BOOT_SLOT_B;   /* e.g. SWITCH ran (step 2d) */
    ok = ok && refused_only(chunk(&rx, 1U, 0U, data, UPLOAD_CHUNK_MAX), REFUSED_TRIAL);
    ok = ok && (fake.programs == programs);
    fake.state.trial = BOOT_SLOT_NONE;
    ok = ok && done(chunk(&rx, 1U, 0U, data, UPLOAD_CHUNK_MAX));
    report(ok, "trial set after BEGIN: next CHUNK REFUSED 0x07, nothing written; trial cleared: "
               "the transfer continues");

    start_running_a(&rx);
    ok = done(begin(&rx, 1U, TEST_LENGTH));
    fake.state.confirmed = BOOT_SLOT_B;   /* boot state changed under the transfer */
    ok = ok && refused_only(chunk(&rx, 1U, 0U, data, UPLOAD_CHUNK_MAX), REFUSED_IMPOSSIBLE);
    report(ok && (fake.programs == 0U), "confirmed slot changed after BEGIN: CHUNK REFUSED 0x06, "
                                        "nothing written");
}

static void test_flash_faults(void)
{
    update_receiver_t rx;
    static uint8_t data[TEST_LENGTH];

    fill_pattern(data, sizeof data);
    start_running_a(&rx);
    fake.fail_erase = true;
    bool ok = received_then_refused(begin(&rx, 1U, TEST_LENGTH), REFUSED_DAMAGED);
    ok = ok && refused_only(chunk(&rx, 1U, 0U, data, UPLOAD_CHUNK_MAX), REFUSED_IMPOSSIBLE);
    ok = ok && received_then_refused(short_command(&rx, CMD_UPLOAD_CANCEL, 1U), REFUSED_DAMAGED);
    fake.fail_erase = false;
    ok = ok && done(short_command(&rx, CMD_UPLOAD_CANCEL, 1U));
    report(ok, "erase fault: BEGIN RECEIVED then REFUSED 0x08; transfer closed; CANCEL repeatable");

    start_running_a(&rx);
    ok = done(begin(&rx, 1U, TEST_LENGTH));
    fake.fail_program = true;
    ok = ok && received_then_refused(chunk(&rx, 1U, 0U, data, UPLOAD_CHUNK_MAX), REFUSED_DAMAGED);
    fake.fail_program = false;
    ok = ok && refused_only(chunk(&rx, 1U, 0U, data, UPLOAD_CHUNK_MAX), REFUSED_IMPOSSIBLE);
    report(ok, "write fault: CHUNK RECEIVED then REFUSED 0x08; transfer closed (BEGIN again)");

    start_running_a(&rx);
    ok = done(begin(&rx, 1U, TEST_LENGTH));
    fake.ecc_error = true;
    ok = ok && received_then_refused(chunk(&rx, 1U, 0U, data, UPLOAD_CHUNK_MAX), REFUSED_DAMAGED);
    report(ok, "read-back hits a damaged flash word (ECC): CHUNK REFUSED 0x08");
}

/* ---- Real images ---------------------------------------------------------------------- */

static uint32_t load(const char *dir, const char *name, uint8_t *buf, uint32_t room)
{
    char path[1024];
    FILE *f;
    size_t n = 0U;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (f != NULL) {
        n = fread(buf, 1U, room, f);
        fclose(f);
    }
    if (n == 0U) {
        printf("cannot read %s\n", path);
    }
    return (uint32_t)n;
}

static void test_images(const char *src_images, const char *test_images)
{
    static uint8_t a[SLOT_BYTES];
    static uint8_t b[SLOT_BYTES];
    static uint8_t bad_key[SLOT_BYTES];
    static uint8_t changed[SLOT_BYTES];
    uint32_t a_size = load(src_images, "firmware-A.img", a, sizeof a);
    uint32_t b_size = load(src_images, "firmware-B.img", b, sizeof b);
    uint32_t key_size = load(test_images, "firmware-B-wrong-key-a.img", bad_key, sizeof bad_key);
    update_receiver_t rx;

    if ((a_size == 0U) || (b_size == 0U) || (key_size == 0U)) {
        report(false, "real images: could not read the image files");
        return;
    }

    start_running_a(&rx);
    answer_t end = upload(&rx, b, b_size, 1U);
    report(done(end) && rx.image_passed &&
               (memcmp(&fake.flash[SLOT_B_START - FLASH_START], b, b_size) == 0) &&
               others_untouched(BOOT_SLOT_B),
           "firmware-B.img into slot B (running A): END DONE 0x00, slot equals the image, "
           "nothing else touched");

    memcpy(changed, b, b_size);
    changed[FLIP_OFFSET] ^= 0xFFU;
    report(received_then_refused(upload(&rx, changed, b_size, 2U), REFUSED_DAMAGED),
           "firmware-B.img with byte 0x600 inverted: END RECEIVED, then REFUSED 0x08");

    report(received_then_refused(upload(&rx, bad_key, key_size, 3U), REFUSED_NOT_OPERATOR),
           "firmware-B signed with an unknown key A: END REFUSED 0x09");

    memcpy(changed, b, b_size);
    changed[IMAGE_OFFSET_TARGET] = 0x32U;   /* controller identity 0x0432, not 0x0431 */
    report(received_then_refused(upload(&rx, changed, b_size, 4U), REFUSED_OTHER_CONTROLLER),
           "firmware-B with another controller identity in the header: END REFUSED 0x0B");

    report(received_then_refused(upload(&rx, a, a_size, 5U), REFUSED_OTHER_SLOT),
           "firmware-A.img (linked for slot A) into slot B: END REFUSED 0x0E");

    report(others_untouched(BOOT_SLOT_B),
           "after these five uploads: bootloader, slot A, last-resort and boot state untouched");

    start_running_a(&rx);
    answer_t a_ok = begin(&rx, 8U, b_size);
    for (uint32_t off = 0U; done(a_ok) && (off < b_size); off += UPLOAD_CHUNK_MAX) {
        uint32_t n = ((b_size - off) < UPLOAD_CHUNK_MAX) ? (b_size - off) : UPLOAD_CHUNK_MAX;
        a_ok = chunk(&rx, 8U, off, &b[off], n);
    }
    fake.ecc_error = true;   /* a damaged word read during END's checks */
    report(done(a_ok) && received_then_refused(short_command(&rx, CMD_UPLOAD_END, 8U),
                                               REFUSED_DAMAGED),
           "good image, but a flash read error during END's checks: REFUSED 0x08");
    fake.ecc_error = false;

    fake_reset();
    fake.state.confirmed = BOOT_SLOT_B;
    fake.allowed_slot = BOOT_SLOT_A;
    update_init(&rx, BOOT_SLOT_B);
    end = upload(&rx, a, a_size, 9U);
    report(done(end) && (memcmp(&fake.flash[SLOT_A_START - FLASH_START], a, a_size) == 0) &&
               others_untouched(BOOT_SLOT_A),
           "running B, confirmed B: firmware-A.img into slot A, END DONE 0x00, slot B untouched");
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: %s <src image dir> <test image dir>\n", argv[0]);
        return 2;
    }
    test_slot_choice();
    test_slot_of();
    test_address_barrier();
    test_result_codes();
    test_payload_sizes();
    test_length_bounds();
    test_offsets();
    test_begin_restarts();
    test_trial_mid_transfer();
    test_flash_faults();
    test_images(argv[1], argv[2]);
    if (failures == 0U) {
        printf("update receiver unit tests: all %u passed\n", tests);
        return 0;
    }
    printf("update receiver unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
