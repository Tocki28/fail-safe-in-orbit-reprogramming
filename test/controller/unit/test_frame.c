/*
 * test_frame.c — host unit tests of the frame layer (src/controller/firmware/data-link/frame.c)
 * against the Interfaces document, Sec. 4.0 (frame), Sec. 4.2 (answer payload) and Sec. 6.2
 * (10 ms gap). Built with the Mac's cc and the undefined-behaviour sanitizer, whose array
 * bounds check stops the test on any index outside the parser's buffer. (The address
 * sanitizer is not used: on this Mac even an empty program built with it stops at once.)
 *
 * The two known-answer frames below were computed apart from this code, with Python's
 * struct and zlib.crc32 (same CRC-32 as Interfaces Sec. 4.0, tools/check_crc32.py).
 *
 * Run: make -C test/controller unit. Prints PASS or FAIL per test; exit status 1 on a failure.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "crc32.h"
#include "frame.h"

/* GET STATUS (0x25) to the controller, sequence 7, expiry 0xFFFFFFFF. */
static const uint8_t GET_STATUS_FRAME[] = {
    0xA5, 0x5A, 0x01, 0x01, 0x02, 0x00, 0x01, 0x00, 0x07, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
    0xFF, 0x25, 0x12, 0x77, 0x1A, 0x06};
/* Answer to the onboard computer, sequence 1: DONE 0x00 for sequence 0x12345678. */
static const uint8_t ANSWER_FRAME[] = {
    0xA5, 0x5A, 0x01, 0x02, 0x01, 0x00, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x78, 0x56, 0x34, 0x12, 0x02, 0x00, 0x53, 0x0E, 0x01, 0x68};

#define MAX_SEEN          4U
#define LATER_MS          100U   /* past the 10 ms gap, so the parser starts afresh */
#define NOISE_BYTES       100000U
#define BIG_PAYLOAD       100U
#define LCG_MUL           1103515245U   /* fixed pseudo-random sequence, repeatable */
#define LCG_ADD           12345U

static uint32_t failures;
static uint32_t tests;

/* What the parser accepted during one feed() call. */
typedef struct {
    uint32_t count;
    frame_t frames[MAX_SEEN];
    uint8_t payloads[MAX_SEEN][FRAME_PAYLOAD_MAX];
} seen_t;

static void report(bool ok, const char *name)
{
    tests++;
    if (!ok) {
        failures++;
    }
    printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
}

/* Feed n bytes, all arriving at ms; record every frame accepted. */
static void feed(frame_parser_t *p, const uint8_t *bytes, uint32_t n, uint32_t ms, seen_t *seen)
{
    frame_t f;

    for (uint32_t i = 0U; i < n; i++) {
        if (frame_parser_feed(p, bytes[i], ms, &f)) {
            if (seen->count < MAX_SEEN) {
                memcpy(seen->payloads[seen->count], f.payload, f.length);
                seen->frames[seen->count] = f;
                seen->frames[seen->count].payload = seen->payloads[seen->count];
            }
            seen->count++;
        }
    }
}

static uint32_t accepted(const uint8_t *bytes, uint32_t n)
{
    frame_parser_t p;
    seen_t seen = {0};

    frame_parser_init(&p);
    feed(&p, bytes, n, 0U, &seen);
    return seen.count;
}

static bool same_frame(const frame_t *a, const frame_t *b)
{
    return (a->type == b->type) && (a->destination == b->destination) && (a->flags == b->flags) &&
           (a->length == b->length) && (a->sequence == b->sequence) && (a->expiry == b->expiry) &&
           (memcmp(a->payload, b->payload, a->length) == 0);
}

static void put_crc(uint8_t *bytes, uint32_t covered)
{
    uint32_t crc = crc32_compute(bytes, covered);

    for (uint32_t i = 0U; i < FRAME_CHECK_BYTES; i++) {
        bytes[covered + i] = (uint8_t)(crc >> (8U * i));
    }
}

/* ---- Tests ---------------------------------------------------------------------- */

static void test_known_frame_accepted(void)
{
    frame_parser_t p;
    seen_t seen = {0};

    frame_parser_init(&p);
    feed(&p, GET_STATUS_FRAME, sizeof GET_STATUS_FRAME, 0U, &seen);
    const frame_t *f = &seen.frames[0];
    report((seen.count == 1U) && (f->type == FRAME_TYPE_COMMAND) &&
               (f->destination == FRAME_DEST_CONTROLLER) && (f->flags == 0U) && (f->length == 1U) &&
               (f->sequence == 7U) && (f->expiry == 0xFFFFFFFFU) && (f->payload[0] == 0x25U),
           "good frame accepted: GET STATUS (bytes from zlib) decoded field by field");
}

static void test_answer_layout(void)
{
    uint8_t payload[ANSWER_PAYLOAD_BYTES];
    static const uint8_t expected[ANSWER_PAYLOAD_BYTES] = {0x78, 0x56, 0x34, 0x12, 0x02, 0x00};

    answer_payload(0x12345678U, ANSWER_DONE, ANSWER_CODE_SUCCESS, payload);
    report(memcmp(payload, expected, sizeof expected) == 0,
           "answer payload: sequence answered (4, little-endian), kind (1), code (1)");

    uint8_t out[FRAME_MAX_BYTES];
    frame_t answer = {FRAME_TYPE_ANSWER, FRAME_DEST_ONBOARD, 0U, ANSWER_PAYLOAD_BYTES, 1U, 0U,
                      payload};
    uint32_t size = frame_build(&answer, out, sizeof out);
    report((size == sizeof ANSWER_FRAME) && (memcmp(out, ANSWER_FRAME, size) == 0),
           "answer frame from the builder equals the bytes computed with zlib");
}

/* Flip each bit of a good frame in turn: no frame may be accepted, and the parser must take
 * the good frame that follows after a pause. */
static void flips_rejected(const uint8_t *good, uint32_t n, const char *name)
{
    uint8_t bad[FRAME_MAX_BYTES];
    uint32_t rejected = 0U;
    uint32_t recovered = 0U;

    for (uint32_t bit = 0U; bit < (n * 8U); bit++) {
        frame_parser_t p;
        seen_t bad_seen = {0};
        seen_t good_seen = {0};

        memcpy(bad, good, n);
        bad[bit / 8U] ^= (uint8_t)(1U << (bit % 8U));
        frame_parser_init(&p);
        feed(&p, bad, n, 0U, &bad_seen);
        feed(&p, good, n, LATER_MS, &good_seen);
        rejected += (bad_seen.count == 0U) ? 1U : 0U;
        recovered += (good_seen.count == 1U) ? 1U : 0U;
    }
    char line[160];
    snprintf(line, sizeof line, "single-bit flips, %s: %u of %u rejected, next frame taken %u times",
             name, rejected, n * 8U, recovered);
    report((rejected == (n * 8U)) && (recovered == (n * 8U)), line);
}

static void test_bit_flips(void)
{
    uint8_t payload[BIG_PAYLOAD];
    uint8_t big[FRAME_MAX_BYTES];
    uint32_t x = 1U;

    for (uint32_t i = 0U; i < BIG_PAYLOAD; i++) {
        x = (x * LCG_MUL) + LCG_ADD;
        payload[i] = (uint8_t)(x >> 16);
    }
    frame_t f = {FRAME_TYPE_REPORT, FRAME_DEST_ONBOARD, 0U, BIG_PAYLOAD, 0xA55A0001U, 0U, payload};
    uint32_t big_size = frame_build(&f, big, sizeof big);

    flips_rejected(GET_STATUS_FRAME, sizeof GET_STATUS_FRAME, "GET STATUS (21 bytes)");
    flips_rejected(ANSWER_FRAME, sizeof ANSWER_FRAME, "answer (26 bytes)");
    flips_rejected(big, big_size, "100-byte payload (120 bytes)");
}

static void test_wrong_version(void)
{
    uint8_t bytes[sizeof GET_STATUS_FRAME];
    bool ok = true;
    static const uint8_t versions[] = {0x00U, 0x02U, 0xFFU};

    for (uint32_t i = 0U; i < sizeof versions; i++) {
        memcpy(bytes, GET_STATUS_FRAME, sizeof bytes);
        bytes[FRAME_OFFSET_VERSION] = versions[i];
        put_crc(bytes, sizeof bytes - FRAME_CHECK_BYTES);   /* only the version is wrong */
        ok = ok && (accepted(bytes, sizeof bytes) == 0U);
    }
    report(ok, "wrong version (0x00, 0x02, 0xFF) with a correct check value rejected");
}

static void test_length_limit(void)
{
    static uint8_t bytes[FRAME_MAX_BYTES + 1U];
    uint32_t sizes[2] = {FRAME_PAYLOAD_MAX, FRAME_PAYLOAD_MAX + 1U};
    uint32_t counts[2];

    for (uint32_t i = 0U; i < 2U; i++) {
        uint32_t length = sizes[i];
        memset(bytes, 0x33, sizeof bytes);
        memcpy(bytes, GET_STATUS_FRAME, FRAME_HEADER_BYTES);
        bytes[FRAME_OFFSET_LENGTH] = (uint8_t)length;
        bytes[FRAME_OFFSET_LENGTH + 1U] = (uint8_t)(length >> 8);
        put_crc(bytes, FRAME_HEADER_BYTES + length);   /* a correct check value */
        counts[i] = accepted(bytes, FRAME_HEADER_BYTES + length + FRAME_CHECK_BYTES);
    }
    report((counts[0] == 1U) && (counts[1] == 0U),
           "Length 1024 accepted; Length 1025 rejected even with a correct check value");

    uint8_t payload[FRAME_PAYLOAD_MAX + 1U] = {0};
    uint8_t out[FRAME_MAX_BYTES];
    frame_t too_long = {FRAME_TYPE_REPORT, FRAME_DEST_ONBOARD, 0U, FRAME_PAYLOAD_MAX + 1U, 1U, 0U,
                        payload};
    frame_t fits = too_long;
    fits.length = 6U;
    report((frame_build(&too_long, out, sizeof out) == 0U) && (frame_build(&fits, out, 25U) == 0U) &&
               (frame_build(&fits, out, 26U) == 26U),
           "builder refuses Length 1025 and a too-small buffer (25 bytes for 26)");
}

static void test_garbage_before_sync(void)
{
    /* Stray bytes, lone Sync bytes, a Sync with a bad version, a header with Length 0xFFFF,
     * and a first Sync byte repeated just before the real frame. */
    static const uint8_t garbage[] = {0x00, 0xA5, 0x00, 0x5A, 0xA5, 0xA5, 0x5A, 0x02, 0x5A, 0xFF,
                                      0xA5, 0x5A, 0x01, 0x01, 0x02, 0x00, 0xFF, 0xFF, 0x42, 0xA5};
    frame_parser_t p;
    seen_t seen = {0};

    frame_parser_init(&p);
    feed(&p, garbage, sizeof garbage, 0U, &seen);
    feed(&p, GET_STATUS_FRAME, sizeof GET_STATUS_FRAME, 0U, &seen);
    report((seen.count == 1U) && (seen.frames[0].sequence == 7U),
           "garbage before Sync skipped (incl. false starts), then the frame accepted");
}

static void test_back_to_back(void)
{
    frame_parser_t p;
    seen_t seen = {0};

    frame_parser_init(&p);
    feed(&p, GET_STATUS_FRAME, sizeof GET_STATUS_FRAME, 0U, &seen);
    feed(&p, ANSWER_FRAME, sizeof ANSWER_FRAME, 0U, &seen);
    report((seen.count == 2U) && (seen.frames[0].sequence == 7U) &&
               (seen.frames[1].type == FRAME_TYPE_ANSWER) && (seen.frames[1].sequence == 1U),
           "two frames back to back: both accepted, in order");
}

/* First half of a frame at t1, the rest at t2: how many frames are accepted? */
static uint32_t split_at(uint32_t t1, uint32_t t2)
{
    frame_parser_t p;
    seen_t seen = {0};
    uint32_t half = sizeof GET_STATUS_FRAME / 2U;

    frame_parser_init(&p);
    feed(&p, GET_STATUS_FRAME, half, t1, &seen);
    feed(&p, &GET_STATUS_FRAME[half], sizeof GET_STATUS_FRAME - half, t2, &seen);
    feed(&p, GET_STATUS_FRAME, sizeof GET_STATUS_FRAME, t2 + LATER_MS, &seen);   /* recovers */
    return seen.count;
}

static void test_gap(void)
{
    report(split_at(1000U, 1011U) == 1U,
           "frame split by an 11 ms gap dropped; next frame accepted");
    report(split_at(1000U, 1010U) == 2U,
           "frame split by a 10 ms gap kept (the rule is 'more than 10 ms')");
    report((split_at(0xFFFFFFFAU, 4U) == 2U) && (split_at(0xFFFFFFFAU, 5U) == 1U),
           "gap measured across the millisecond count's wrap: 10 ms kept, 11 ms dropped");
}

static void test_round_trip(void)
{
    static const uint16_t lengths[] = {0U, 1U, 6U, 255U, FRAME_PAYLOAD_MAX};
    uint8_t payload[FRAME_PAYLOAD_MAX];
    uint8_t out[FRAME_MAX_BYTES];
    bool ok = true;

    for (uint32_t i = 0U; i < FRAME_PAYLOAD_MAX; i++) {
        payload[i] = (uint8_t)(i * 7U);
    }
    for (uint32_t i = 0U; i < sizeof lengths / sizeof lengths[0]; i++) {
        frame_t in = {FRAME_TYPE_COMMAND, FRAME_DEST_CONTROLLER, FRAME_FLAG_RELEASED, lengths[i],
                      0xFFFFFFFFU - i, 0x01020304U + i, payload};
        frame_parser_t p;
        seen_t seen = {0};
        uint32_t size = frame_build(&in, out, sizeof out);

        frame_parser_init(&p);
        feed(&p, out, size, 0U, &seen);
        ok = ok && (size == FRAME_HEADER_BYTES + lengths[i] + FRAME_CHECK_BYTES) &&
             (seen.count == 1U) && same_frame(&in, &seen.frames[0]);
    }
    report(ok, "builder output parsed back equals the input (payload 0, 1, 6, 255, 1024 bytes)");
}

static void test_noise(void)
{
    frame_parser_t p;
    seen_t seen = {0};
    uint32_t x = 2026U;

    frame_parser_init(&p);
    for (uint32_t i = 0U; i < NOISE_BYTES; i++) {
        uint8_t byte;
        x = (x * LCG_MUL) + LCG_ADD;
        byte = (uint8_t)(x >> 16);
        feed(&p, &byte, 1U, i / 16U, &seen);   /* ~16 bytes per ms, about the line rate */
    }
    feed(&p, GET_STATUS_FRAME, sizeof GET_STATUS_FRAME, NOISE_BYTES, &seen);
    report((seen.count == 1U) && (seen.frames[0].sequence == 7U),
           "100000 random bytes: no frame accepted, no access outside the buffer; then a frame taken");
}

int main(void)
{
    test_known_frame_accepted();
    test_answer_layout();
    test_bit_flips();
    test_wrong_version();
    test_length_limit();
    test_garbage_before_sync();
    test_back_to_back();
    test_gap();
    test_round_trip();
    test_noise();
    if (failures == 0U) {
        printf("frame unit tests: all %u passed\n", tests);
        return 0;
    }
    printf("frame unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
