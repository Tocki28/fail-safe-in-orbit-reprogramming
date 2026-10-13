/*
 * test_frame.cpp — host unit tests of the frame layer (src/controller/firmware/data-link/
 * frame.cpp) against the Interfaces document, Sec. 4.0 (frame), Sec. 4.2 (answer payload) and
 * Sec. 6.2 (10 ms gap). Built with the Mac's clang++, the undefined-behaviour sanitizer and
 * libc++'s debug hardening: the sanitizer checks indexes into plain arrays, the hardening
 * checks indexes into std::array (the parser's buffer is one), and either stops the test on any
 * index outside its array. (The address sanitizer is not used: on this Mac even an empty program
 * built with it stops at once.)
 *
 * The two known-answer frames below were computed apart from this code, with Python's
 * struct and zlib.crc32 (same CRC-32 as Interfaces Sec. 4.0, tools/check_crc32.py).
 *
 * Run: make -C test/controller unit. Prints PASS or FAIL per test; exit status 1 on a failure.
 */
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "crc32.h"
#include "frame.hpp"

namespace {

/* GET STATUS (0x25) to the controller, sequence 7, expiry 0xFFFFFFFF. */
const uint8_t kGetStatusFrame[] = {
    0xA5, 0x5A, 0x01, 0x01, 0x02, 0x00, 0x01, 0x00, 0x07, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
    0xFF, 0x25, 0x12, 0x77, 0x1A, 0x06};
constexpr uint32_t kGetStatusBytes = sizeof kGetStatusFrame;
/* Answer to the onboard computer, sequence 1: DONE 0x00 for sequence 0x12345678. */
const uint8_t kAnswerFrame[] = {
    0xA5, 0x5A, 0x01, 0x02, 0x01, 0x00, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x78, 0x56, 0x34, 0x12, 0x02, 0x00, 0x53, 0x0E, 0x01, 0x68};
constexpr uint32_t kAnswerBytes = sizeof kAnswerFrame;

constexpr uint32_t kMaxSeen = 4U;
constexpr uint32_t kLaterMs = 100U;   /* past the 10 ms gap, so the parser starts afresh */
constexpr uint32_t kNoiseBytes = 100000U;
constexpr uint32_t kBigPayload = 100U;
constexpr uint32_t kLcgMul = 1103515245U;   /* fixed pseudo-random sequence, repeatable */
constexpr uint32_t kLcgAdd = 12345U;

uint32_t failures;
uint32_t tests;

/* What the parser accepted during one feed() call. */
struct Seen {
    uint32_t count = 0U;
    std::array<Frame, kMaxSeen> frames{};
    std::array<std::array<uint8_t, kFramePayloadMax>, kMaxSeen> payloads{};
};

/**
 * @brief Counts one test result and prints a PASS or FAIL line for it.
 *
 * Increments the global test count, and the failure count when the check failed.
 *
 * @param[in] ok    true if the check passed
 * @param[in] name  text printed after PASS or FAIL
 */
void report(bool ok, const char *name)
{
    tests++;
    if (!ok) {
        failures++;
    }
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
}

/**
 * @brief Feeds bytes to the parser one by one and records every frame it accepts.
 *
 * Feed n bytes, all arriving at ms; record every frame accepted. An accepted frame is copied
 * into the record together with its payload; the count keeps rising past the record size,
 * but only the first kMaxSeen frames are stored.
 *
 * @param[in,out] p      parser under test
 * @param[in]     bytes  bytes to feed, in order
 * @param[in]     n      number of bytes to feed
 * @param[in]     ms     millisecond count handed to the parser with every byte
 * @param[in,out] seen   record of accepted frames; new frames are added after the old ones
 */
void feed(FrameParser &p, const uint8_t *bytes, uint32_t n, uint32_t ms, Seen &seen)
{
    Frame f{};

    for (uint32_t i = 0U; i < n; i++) {
        if (p.feed(bytes[i], ms, f)) {
            if (seen.count < kMaxSeen) {
                std::memcpy(seen.payloads[seen.count].data(), f.payload, f.length);
                seen.frames[seen.count] = f;
                seen.frames[seen.count].payload = seen.payloads[seen.count].data();
            }
            seen.count++;
        }
    }
}

/**
 * @brief Runs a byte string through a fresh parser and counts the frames it accepts.
 *
 * @param[in] bytes  bytes to feed, all at millisecond 0
 * @param[in] n      number of bytes
 * @return number of frames the parser accepted
 */
uint32_t accepted(const uint8_t *bytes, uint32_t n)
{
    FrameParser p;
    Seen seen;

    feed(p, bytes, n, 0U, seen);
    return seen.count;
}

/**
 * @brief Compares two frames field by field, including the payload bytes.
 *
 * @param[in] a  first frame
 * @param[in] b  second frame; the payload is compared over a.length bytes
 * @return true if type, destination, flags, length, sequence, expiry and payload all match,
 *         false otherwise
 */
bool sameFrame(const Frame &a, const Frame &b)
{
    return (a.type == b.type) && (a.destination == b.destination) && (a.flags == b.flags) &&
           (a.length == b.length) && (a.sequence == b.sequence) && (a.expiry == b.expiry) &&
           (std::memcmp(a.payload, b.payload, a.length) == 0);
}

/**
 * @brief Writes the CRC-32 of the first bytes into the check field that follows them.
 *
 * The check value is stored little-endian, kFrameCheckBytes bytes, at bytes[covered].
 *
 * @param[in,out] bytes    frame buffer; must hold covered + kFrameCheckBytes bytes
 * @param[in]     covered  number of leading bytes the check value covers
 */
void putCrc(uint8_t *bytes, uint32_t covered)
{
    uint32_t crc = crc32_compute(bytes, covered);

    for (uint32_t i = 0U; i < kFrameCheckBytes; i++) {
        bytes[covered + i] = static_cast<uint8_t>(crc >> (8U * i));
    }
}

/* ---- Tests ---------------------------------------------------------------------- */

/**
 * @brief Checks that the GET STATUS known-answer frame is accepted and decoded field by field.
 *
 * Passes if exactly one frame is accepted and its type, destination, flags, length, sequence,
 * expiry and payload byte equal the values the frame was built from.
 */
void testKnownFrameAccepted()
{
    FrameParser p;
    Seen seen;

    feed(p, kGetStatusFrame, kGetStatusBytes, 0U, seen);
    const Frame &f = seen.frames[0];
    report((seen.count == 1U) && (f.type == FrameType::kCommand) &&
               (f.destination == Destination::kController) && (f.flags == 0U) && (f.length == 1U) &&
               (f.sequence == 7U) && (f.expiry == 0xFFFFFFFFU) && (f.payload[0] == 0x25U),
           "good frame accepted: GET STATUS (bytes from zlib) decoded field by field");
}

/**
 * @brief Checks the answer payload layout and the answer frame produced by the builder.
 *
 * Passes if the payload is the sequence answered (4 bytes, little-endian), the kind and the
 * code, and if the built frame equals the known-answer bytes.
 */
void testAnswerLayout()
{
    std::array<uint8_t, kAnswerPayloadBytes> payload{};
    const std::array<uint8_t, kAnswerPayloadBytes> expected = {0x78, 0x56, 0x34, 0x12, 0x02, 0x00};

    answerPayload(0x12345678U, AnswerKind::kDone, AnswerCode::kSuccess, payload);
    report(payload == expected,
           "answer payload: sequence answered (4, little-endian), kind (1), code (1)");

    std::array<uint8_t, kFrameMaxBytes> out{};
    const Frame answer{FrameType::kAnswer, Destination::kOnboard, 0U, kAnswerPayloadBytes, 1U, 0U,
                       payload.data()};
    uint32_t size = buildFrame(answer, out.data(), kFrameMaxBytes);
    report((size == kAnswerBytes) && (std::memcmp(out.data(), kAnswerFrame, size) == 0),
           "answer frame from the builder equals the bytes computed with zlib");
}

/**
 * @brief Flips each bit of a good frame in turn and checks the damaged copy is rejected.
 *
 * Flip each bit of a good frame in turn: no frame may be accepted, and the parser must take
 * the good frame that follows after a pause. Reports one result line with the counts.
 *
 * @param[in] good  bytes of a valid frame
 * @param[in] n     number of bytes in the frame
 * @param[in] name  frame description used in the printed line
 */
void flipsRejected(const uint8_t *good, uint32_t n, const char *name)
{
    std::array<uint8_t, kFrameMaxBytes> bad{};
    uint32_t rejected = 0U;
    uint32_t recovered = 0U;

    for (uint32_t bit = 0U; bit < (n * 8U); bit++) {
        FrameParser p;
        Seen bad_seen;
        Seen good_seen;

        std::memcpy(bad.data(), good, n);
        bad[bit / 8U] = static_cast<uint8_t>(bad[bit / 8U] ^ (1U << (bit % 8U)));
        feed(p, bad.data(), n, 0U, bad_seen);
        feed(p, good, n, kLaterMs, good_seen);
        rejected += (bad_seen.count == 0U) ? 1U : 0U;
        recovered += (good_seen.count == 1U) ? 1U : 0U;
    }
    char line[160];
    std::snprintf(line, sizeof line,
                  "single-bit flips, %s: %u of %u rejected, next frame taken %u times", name,
                  rejected, n * 8U, recovered);
    report((rejected == (n * 8U)) && (recovered == (n * 8U)), line);
}

/**
 * @brief Runs the single-bit-flip check on three frames of different size.
 *
 * Passes (through flipsRejected) if every flip in the GET STATUS frame, the answer frame and a
 * 100-byte-payload frame is rejected and the next good frame is taken each time.
 */
void testBitFlips()
{
    std::array<uint8_t, kBigPayload> payload{};
    std::array<uint8_t, kFrameMaxBytes> big{};
    uint32_t x = 1U;

    for (uint32_t i = 0U; i < kBigPayload; i++) {
        x = (x * kLcgMul) + kLcgAdd;
        payload[i] = static_cast<uint8_t>(x >> 16);
    }
    const Frame f{FrameType::kReport, Destination::kOnboard, 0U, kBigPayload, 0xA55A0001U, 0U,
                  payload.data()};
    uint32_t big_size = buildFrame(f, big.data(), kFrameMaxBytes);

    flipsRejected(kGetStatusFrame, kGetStatusBytes, "GET STATUS (21 bytes)");
    flipsRejected(kAnswerFrame, kAnswerBytes, "answer (26 bytes)");
    flipsRejected(big.data(), big_size, "100-byte payload (120 bytes)");
}

/**
 * @brief Checks that a frame with a wrong version byte is rejected even with a correct CRC.
 *
 * Passes if versions 0x00, 0x02 and 0xFF, each with the check value recomputed, yield no frame.
 */
void testWrongVersion()
{
    std::array<uint8_t, kGetStatusBytes> bytes{};
    bool ok = true;
    const uint8_t versions[] = {0x00U, 0x02U, 0xFFU};

    for (uint8_t version : versions) {
        std::memcpy(bytes.data(), kGetStatusFrame, kGetStatusBytes);
        bytes[kFrameOffsetVersion] = version;
        putCrc(bytes.data(), kGetStatusBytes - kFrameCheckBytes);   /* only the version is wrong */
        ok = ok && (accepted(bytes.data(), kGetStatusBytes) == 0U);
    }
    report(ok, "wrong version (0x00, 0x02, 0xFF) with a correct check value rejected");
}

/**
 * @brief Checks the payload length limit in the parser and in the builder.
 *
 * Passes if the parser accepts Length 1024 and rejects Length 1025 (both with a correct CRC),
 * and if the builder returns 0 for Length 1025 and for a buffer one byte too small, and the
 * frame size for a buffer of exactly the right size.
 */
void testLengthLimit()
{
    static std::array<uint8_t, kFrameMaxBytes + 1U> bytes;
    const uint32_t sizes[2] = {kFramePayloadMax, kFramePayloadMax + 1U};
    uint32_t counts[2] = {0U, 0U};

    for (uint32_t i = 0U; i < 2U; i++) {
        uint32_t length = sizes[i];
        bytes.fill(0x33U);
        std::memcpy(bytes.data(), kGetStatusFrame, kFrameHeaderBytes);
        bytes[kFrameOffsetLength] = static_cast<uint8_t>(length);
        bytes[kFrameOffsetLength + 1U] = static_cast<uint8_t>(length >> 8);
        putCrc(bytes.data(), kFrameHeaderBytes + length);   /* a correct check value */
        counts[i] = accepted(bytes.data(), kFrameHeaderBytes + length + kFrameCheckBytes);
    }
    report((counts[0] == 1U) && (counts[1] == 0U),
           "Length 1024 accepted; Length 1025 rejected even with a correct check value");

    std::array<uint8_t, kFramePayloadMax + 1U> payload{};
    std::array<uint8_t, kFrameMaxBytes> out{};
    const Frame too_long{FrameType::kReport, Destination::kOnboard, 0U, kFramePayloadMax + 1U, 1U,
                         0U, payload.data()};
    Frame fits = too_long;
    fits.length = 6U;
    report((buildFrame(too_long, out.data(), kFrameMaxBytes) == 0U) &&
               (buildFrame(fits, out.data(), 25U) == 0U) && (buildFrame(fits, out.data(), 26U) == 26U),
           "builder refuses Length 1025 and a too-small buffer (25 bytes for 26)");
}

/**
 * @brief Checks that stray bytes and false starts before the Sync bytes are skipped.
 *
 * Passes if, after a run of garbage, exactly one frame (sequence 7) is accepted.
 */
void testGarbageBeforeSync()
{
    /* Stray bytes, lone Sync bytes, a Sync with a bad version, a header with Length 0xFFFF,
     * and a first Sync byte repeated just before the real frame. */
    const uint8_t garbage[] = {0x00, 0xA5, 0x00, 0x5A, 0xA5, 0xA5, 0x5A, 0x02, 0x5A, 0xFF,
                               0xA5, 0x5A, 0x01, 0x01, 0x02, 0x00, 0xFF, 0xFF, 0x42, 0xA5};
    FrameParser p;
    Seen seen;

    feed(p, garbage, sizeof garbage, 0U, seen);
    feed(p, kGetStatusFrame, kGetStatusBytes, 0U, seen);
    report((seen.count == 1U) && (seen.frames[0].sequence == 7U),
           "garbage before Sync skipped (incl. false starts), then the frame accepted");
}

/**
 * @brief Checks that two frames sent with no pause between them are both accepted in order.
 *
 * Passes if the GET STATUS frame then the answer frame are accepted, in that order.
 */
void testBackToBack()
{
    FrameParser p;
    Seen seen;

    feed(p, kGetStatusFrame, kGetStatusBytes, 0U, seen);
    feed(p, kAnswerFrame, kAnswerBytes, 0U, seen);
    report((seen.count == 2U) && (seen.frames[0].sequence == 7U) &&
               (seen.frames[1].type == FrameType::kAnswer) && (seen.frames[1].sequence == 1U),
           "two frames back to back: both accepted, in order");
}

/**
 * @brief Feeds a frame in two halves at two times and counts the frames accepted.
 *
 * First half of a frame at t1, the rest at t2: how many frames are accepted? A complete good
 * frame is fed afterwards at t2 plus kLaterMs, so the parser recovers if it dropped the first.
 *
 * @param[in] t1  millisecond count of the first half
 * @param[in] t2  millisecond count of the second half
 * @return number of frames accepted: 2 if the split frame was kept, 1 if it was dropped
 */
uint32_t splitAt(uint32_t t1, uint32_t t2)
{
    FrameParser p;
    Seen seen;
    uint32_t half = kGetStatusBytes / 2U;

    feed(p, kGetStatusFrame, half, t1, seen);
    feed(p, &kGetStatusFrame[half], kGetStatusBytes - half, t2, seen);
    feed(p, kGetStatusFrame, kGetStatusBytes, t2 + kLaterMs, seen);   /* recovers */
    return seen.count;
}

/**
 * @brief Checks the inter-byte gap rule: a gap of more than 10 ms drops the frame.
 *
 * Passes if an 11 ms gap drops the frame, a 10 ms gap keeps it, and the same holds when the
 * millisecond count wraps around between the two halves.
 */
void testGap()
{
    report(splitAt(1000U, 1011U) == 1U,
           "frame split by an 11 ms gap dropped; next frame accepted");
    report(splitAt(1000U, 1010U) == 2U,
           "frame split by a 10 ms gap kept (the rule is 'more than 10 ms')");
    report((splitAt(0xFFFFFFFAU, 4U) == 2U) && (splitAt(0xFFFFFFFAU, 5U) == 1U),
           "gap measured across the millisecond count's wrap: 10 ms kept, 11 ms dropped");
}

/**
 * @brief Checks that builder output parsed back equals the input frame.
 *
 * Passes for payload lengths 0, 1, 6, 255 and 1024 if the built size is header plus payload plus
 * check, exactly one frame is accepted, and it equals the input in every field.
 */
void testRoundTrip()
{
    const uint16_t lengths[] = {0U, 1U, 6U, 255U, kFramePayloadMax};
    std::array<uint8_t, kFramePayloadMax> payload{};
    std::array<uint8_t, kFrameMaxBytes> out{};
    bool ok = true;
    uint32_t i = 0U;

    for (uint32_t k = 0U; k < kFramePayloadMax; k++) {
        payload[k] = static_cast<uint8_t>(k * 7U);
    }
    for (uint16_t length : lengths) {
        const Frame in{FrameType::kCommand, Destination::kController, kFrameFlagReleased, length,
                       0xFFFFFFFFU - i, 0x01020304U + i, payload.data()};
        FrameParser p;
        Seen seen;
        uint32_t size = buildFrame(in, out.data(), kFrameMaxBytes);

        feed(p, out.data(), size, 0U, seen);
        ok = ok && (size == kFrameHeaderBytes + length + kFrameCheckBytes) && (seen.count == 1U) &&
             sameFrame(in, seen.frames[0]);
        i++;
    }
    report(ok, "builder output parsed back equals the input (payload 0, 1, 6, 255, 1024 bytes)");
}

/**
 * @brief Checks that a frame with any Flags bit 1 to 7 set is dropped, with a valid check value.
 *
 * Interfaces Sec. 4.0 (frame table): Flags bit 0 is "released", the other bits are 0. For each
 * Flags byte the frame is built with buildFrame, so its check value is right and only the Flags
 * differ. Passes if Flags 0x00 and 0x01 are accepted, each of 0x02, 0x04, 0x80, 0xFE and 0xFF
 * is not, and a good frame that follows a rejected one is taken. Before the check was added the
 * parser accepted every one of them.
 */
void testReservedFlags()
{
    const uint8_t payload[1] = {0x25U};
    const uint8_t flagsAccepted[] = {0x00U, 0x01U};
    const uint8_t flagsDropped[] = {0x02U, 0x04U, 0x80U, 0xFEU, 0xFFU};
    std::array<uint8_t, kFrameMaxBytes> out{};
    bool ok = true;

    for (uint8_t flags : flagsAccepted) {
        const Frame in{FrameType::kCommand, Destination::kController, flags, 1U, 5U, 0U, payload};
        uint32_t size = buildFrame(in, out.data(), kFrameMaxBytes);
        ok = ok && (accepted(out.data(), size) == 1U);
    }
    report(ok, "Flags 0x00 and 0x01 (bit 0 only) accepted");

    ok = true;
    for (uint8_t flags : flagsDropped) {
        const Frame in{FrameType::kCommand, Destination::kController, flags, 1U, 5U, 0U, payload};
        uint32_t size = buildFrame(in, out.data(), kFrameMaxBytes);
        ok = ok && (size > 0U) && (accepted(out.data(), size) == 0U);
    }
    report(ok, "Flags 0x02, 0x04, 0x80, 0xFE, 0xFF (a bit 1 to 7 set), right check value: dropped");

    /* A dropped frame is consumed whole: the good frame right behind it is still taken. */
    const Frame bad{FrameType::kCommand, Destination::kController, 0x02U, 1U, 5U, 0U, payload};
    std::array<uint8_t, 2U * kFrameMaxBytes> two{};
    uint32_t first = buildFrame(bad, two.data(), kFrameMaxBytes);
    std::memcpy(&two[first], kGetStatusFrame, kGetStatusBytes);
    report(accepted(two.data(), first + kGetStatusBytes) == 1U,
           "frame with Flags 0x02 dropped whole; the good frame right behind it is taken");
}

/**
 * @brief Checks the parser against 100000 pseudo-random bytes, then a good frame.
 *
 * The bytes come from a fixed linear congruential sequence at about 16 bytes per millisecond.
 * Passes if the following GET STATUS frame is the only frame accepted. Run under the
 * sanitizers, an index outside the buffer stops the test.
 */
void testNoise()
{
    FrameParser p;
    Seen seen;
    uint32_t x = 2026U;

    for (uint32_t i = 0U; i < kNoiseBytes; i++) {
        x = (x * kLcgMul) + kLcgAdd;
        uint8_t byte = static_cast<uint8_t>(x >> 16);
        feed(p, &byte, 1U, i / 16U, seen);   /* ~16 bytes per ms, about the line rate */
    }
    feed(p, kGetStatusFrame, kGetStatusBytes, kNoiseBytes, seen);
    report((seen.count == 1U) && (seen.frames[0].sequence == 7U),
           "100000 random bytes: no frame accepted, no access outside the buffer; then a frame taken");
}

}  // namespace

/**
 * @brief Runs every frame test and prints the totals.
 *
 * @return 0 if all tests passed, 1 if any failed
 */
int main()
{
    testKnownFrameAccepted();
    testAnswerLayout();
    testBitFlips();
    testWrongVersion();
    testLengthLimit();
    testGarbageBeforeSync();
    testBackToBack();
    testGap();
    testRoundTrip();
    testReservedFlags();
    testNoise();
    if (failures == 0U) {
        std::printf("frame unit tests: all %u passed\n", tests);
        return 0;
    }
    std::printf("frame unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
