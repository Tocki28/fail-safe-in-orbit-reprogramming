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

void report(bool ok, const char *name)
{
    tests++;
    if (!ok) {
        failures++;
    }
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
}

/* Feed n bytes, all arriving at ms; record every frame accepted. */
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

uint32_t accepted(const uint8_t *bytes, uint32_t n)
{
    FrameParser p;
    Seen seen;

    feed(p, bytes, n, 0U, seen);
    return seen.count;
}

bool sameFrame(const Frame &a, const Frame &b)
{
    return (a.type == b.type) && (a.destination == b.destination) && (a.flags == b.flags) &&
           (a.length == b.length) && (a.sequence == b.sequence) && (a.expiry == b.expiry) &&
           (std::memcmp(a.payload, b.payload, a.length) == 0);
}

void putCrc(uint8_t *bytes, uint32_t covered)
{
    uint32_t crc = crc32_compute(bytes, covered);

    for (uint32_t i = 0U; i < kFrameCheckBytes; i++) {
        bytes[covered + i] = static_cast<uint8_t>(crc >> (8U * i));
    }
}

/* ---- Tests ---------------------------------------------------------------------- */

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

/* Flip each bit of a good frame in turn: no frame may be accepted, and the parser must take
 * the good frame that follows after a pause. */
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

/* First half of a frame at t1, the rest at t2: how many frames are accepted? */
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

void testGap()
{
    report(splitAt(1000U, 1011U) == 1U,
           "frame split by an 11 ms gap dropped; next frame accepted");
    report(splitAt(1000U, 1010U) == 2U,
           "frame split by a 10 ms gap kept (the rule is 'more than 10 ms')");
    report((splitAt(0xFFFFFFFAU, 4U) == 2U) && (splitAt(0xFFFFFFFAU, 5U) == 1U),
           "gap measured across the millisecond count's wrap: 10 ms kept, 11 ms dropped");
}

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
    testNoise();
    if (failures == 0U) {
        std::printf("frame unit tests: all %u passed\n", tests);
        return 0;
    }
    std::printf("frame unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
