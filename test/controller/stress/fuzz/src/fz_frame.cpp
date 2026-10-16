/*
 * fz_frame.cpp — fuzz target 1: the controller's data-link frame parser (FrameParser,
 * src/controller/firmware/data-link/frame.hpp, frame.cpp), byte by byte with random gaps around
 * kFrameGapMaxMs [frame.hpp:61].
 *
 * Oracles (the two ends of a data link share no software, REQ-099):
 *   1. the onboard computer's frame::Parser (src/onboard-computer/updater/frame.cpp) fed the same
 *      bytes at the same times, with a 64-bit clock where the controller gets the same time cut to
 *      32 bits (the wrap of the millisecond count);
 *   2. the onboard computer's frame::decode on the raw bytes of every frame the controller
 *      accepts (Interfaces Sec. 4.0: Sync, Version, Length equal to what follows, CRC-32), with
 *      the gap rule checked on the stream by this file;
 *   3. a clean frame after a gap of more than 10 ms must be accepted after any garbage
 *      (Interfaces Sec. 6.2: a gap of more than 10 ms inside a frame drops what was held).
 * The one difference that is by design and not a finding: the controller drops a frame whose
 * Flags bits 1 to 7 are not 0 (frame.cpp, flagsOk); the onboard computer's parser does not look
 * at Flags. It is counted, not reported.
 *
 * One input is one stream of 1 to 4 segments (valid frames, mutated frames, garbage, truncated
 * frames) with a time for every byte. The input is a pure function of (seed, index).
 */
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "data-link/frame.hpp"
#include "fz_common.h"
#include "fz_fault.h"
#include "updater/crc32.hpp"
#include "updater/frame.hpp"

/* The firmware's frame layer under its own names; the onboard computer's under frame::. The
 * two have the same names for most things, so the firmware's types are used unqualified and the
 * onboard computer's with the prefix. */

namespace {

/* Longest stream of one input: 4 segments of at most 1044 bytes (kFrameMaxBytes), garbage
 * segments of at most 1100 bytes, and 8 bytes of leading noise. */
constexpr size_t kStreamMax = 8192U;
constexpr size_t kSegmentMax = kFrameMaxBytes + 64U;
constexpr uint32_t kSegmentsMax = 4U;
constexpr uint32_t kNoiseMax = 8U;
/* Payload lengths of the seed frames: the limits of Interfaces Sec. 4.0 (0 and 1024), the
 * smallest and largest commands (1, 7, 263 = UPLOAD CHUNK of 256 data bytes) and neighbours. */
constexpr uint16_t kShapeLengths[] = {0U, 1U, 2U, 6U, 7U, 8U, 255U, 256U, 263U, 1023U, 1024U};
constexpr uint32_t kShapeCount = sizeof(kShapeLengths) / sizeof(kShapeLengths[0]);
/* Chances in thousandths, chosen for this audit (no technical reason). */
constexpr uint32_t kChanceMutate = 850U;
constexpr uint32_t kChanceGarbage = 70U;
constexpr uint32_t kChanceTruncated = 50U;
constexpr uint32_t kChanceFixCrc = 500U;
constexpr uint32_t kChanceWrapStart = 300U;
constexpr uint32_t kChanceSyncGarbage = 500U;
/* A gap that must drop a held frame: more than kFrameGapMaxMs; 11 ms up to this many more. */
constexpr uint32_t kRecoveryGapSpread = 500U;
/* Frame fields that boundary values are written to (Interfaces Sec. 4.0, Figure 2). */
constexpr fz_field_t kFields[] = {{0U, 2U}, {2U, 1U}, {3U, 1U}, {4U, 1U}, {5U, 1U},
                                  {6U, 2U}, {8U, 4U}, {12U, 4U}};
constexpr uint32_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);
/* Bytes of Flags that the controller refuses: bits 1 to 7 (Interfaces Sec. 4.0). */
constexpr uint8_t kFlagsUnused = static_cast<uint8_t>(~kFrameFlagReleased);
constexpr size_t kMinFrame = kFrameHeaderBytes + kFrameCheckBytes;

/* The stream of one input. */
struct Stream {
    uint8_t bytes[kStreamMax];
    uint64_t ms[kStreamMax];    /* arrival time of each byte, 64 bits; the controller gets it cut */
    size_t length;
};

Stream g_stream;
FrameParser g_firmware;
/* the whole-frame check has its own parser, so g_firmware keeps the stream's state */
FrameParser g_whole;
frame::Parser g_onboard;

/* Counters for the log. */
uint64_t g_frames_accepted;
uint64_t g_flags_difference;
uint64_t g_recovery_checked;
uint64_t g_whole_checked;
uint64_t g_bytes_fed;

/**
 * @brief Reads a little-endian 16-bit value.
 *
 * @param[in] p  two bytes
 * @return the value
 */
uint16_t readU16(const uint8_t *p)
{
    return static_cast<uint16_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8));
}

/**
 * @brief Builds one valid frame with the onboard computer's encoder (the independent
 * implementation) from random fields.
 *
 * @param[in,out] rng  the generator
 * @param[out]    out  room for kSegmentMax bytes
 * @return the frame's size
 */
size_t buildSeed(fz_rng_t *rng, uint8_t *out)
{
    uint8_t payload[kFramePayloadMax];
    frame::Frame f{};

    f.length = kShapeLengths[fz_rng_below(rng, kShapeCount)];
    if (fz_rng_chance(rng, 100U)) {
        f.length = static_cast<uint16_t>(fz_rng_below(rng, kFramePayloadMax + 1U));
    }
    for (uint32_t i = 0U; i < f.length; i++) {
        payload[i] = static_cast<uint8_t>(fz_rng_next(rng));
    }
    f.type = static_cast<uint8_t>(1U + fz_rng_below(rng, 3U));
    f.destination = static_cast<uint8_t>(1U + fz_rng_below(rng, 3U));
    f.flags = static_cast<uint8_t>(fz_rng_below(rng, 2U));
    f.sequence = fz_rng_chance(rng, 300U) ? fz_rng_edge32(rng)
        : static_cast<uint32_t>(fz_rng_next(rng));
    f.expiry = fz_rng_chance(rng, 300U) ? fz_rng_edge32(rng)
        : static_cast<uint32_t>(fz_rng_next(rng));
    f.payload = payload;
    return frame::encode(f, out, kSegmentMax);
}

/**
 * @brief Writes the CRC-32 of a frame's first length - 4 bytes into its last four bytes.
 *
 * Makes a mutated frame carry a right check value, so the checks after the CRC are reached
 * (Flags, and the Length that must equal what follows).
 *
 * @param[in,out] bytes   the frame
 * @param[in]     length  its size, at least 4
 */
void fixCrc(uint8_t *bytes, size_t length)
{
    uint32_t crc = crc::crc32(bytes, length - kFrameCheckBytes);

    for (uint32_t i = 0U; i < kFrameCheckBytes; i++) {
        bytes[length - kFrameCheckBytes + i] = static_cast<uint8_t>(crc >> (8U * i));
    }
}

/**
 * @brief Builds one segment: a valid frame, a mutated one, a truncated one or garbage.
 *
 * @param[in,out] rng    the generator
 * @param[out]    out    room for kSegmentMax bytes
 * @param[in]     other  another seed frame for splicing
 * @param[in]     other_len  its size
 * @return the segment's size
 */
size_t buildSegment(fz_rng_t *rng, uint8_t *out, const uint8_t *other, size_t other_len)
{
    uint32_t kind = fz_rng_below(rng, 1000U);
    size_t length;

    if (kind < kChanceGarbage) {
        length = fz_rng_below(rng, 1100U);
        for (size_t i = 0U; i < length; i++) {
            out[i] = static_cast<uint8_t>(fz_rng_next(rng));
        }
        if ((length >= 2U) && fz_rng_chance(rng, kChanceSyncGarbage)) {
            out[0] = kFrameSync0;
            out[1] = kFrameSync1;
        }
        return length;
    }
    length = buildSeed(rng, out);
    if (kind < (kChanceGarbage + kChanceTruncated)) {
        return (length > 0U) ? fz_rng_below(rng, static_cast<uint32_t>(length)) : 0U;
    }
    if (fz_rng_chance(rng, kChanceMutate)) {
        length = fz_mutate(rng, out, length, kSegmentMax, kFields, kFieldCount, other, other_len);
        if ((length >= kMinFrame) && fz_rng_chance(rng, kChanceFixCrc)) {
            fixCrc(out, length);
        }
    }
    return length;
}

/**
 * @brief Gives a byte its arrival time: mostly 0 or 1 ms after the one before, sometimes the
 * edge of the gap rule, sometimes far.
 *
 * @param[in,out] rng  the generator
 * @return the gap in milliseconds, below 2^32
 */
uint32_t pickGap(fz_rng_t *rng)
{
    uint32_t r = fz_rng_below(rng, 1000U);

    if (r < 900U) {
        return fz_rng_below(rng, 2U);
    }
    if (r < 960U) {
        return fz_rng_below(rng, kFrameGapMaxMs + 1U);
    }
    if (r < 980U) {
        return kFrameGapMaxMs;
    }
    if (r < 990U) {
        return kFrameGapMaxMs + 1U;
    }
    if (r < 995U) {
        return kFrameGapMaxMs + 2U + fz_rng_below(rng, 100U);
    }
    return fz_rng_edge32(rng);
}

/**
 * @brief Builds the stream of input `index`: noise, then 1 to 4 segments, with times.
 *
 * @param[in,out] rng  the generator
 */
void buildStream(fz_rng_t *rng)
{
    static uint8_t seed_a[kSegmentMax];
    static uint8_t seed_b[kSegmentMax];
    static uint8_t segment[kSegmentMax];
    uint32_t segments = 1U + fz_rng_below(rng, kSegmentsMax);
    size_t other_len = buildSeed(rng, seed_b);
    uint64_t now = fz_rng_chance(rng, kChanceWrapStart)
                       ? (0x100000000ULL - fz_rng_below(rng, 64U)) : fz_rng_below(rng, 1000U);

    (void)seed_a;
    g_stream.length = 0U;
    for (uint32_t i = fz_rng_below(rng, kNoiseMax + 1U); i > 0U; i--) {
        g_stream.bytes[g_stream.length] = static_cast<uint8_t>(fz_rng_next(rng));
        g_stream.length++;
    }
    for (uint32_t s = 0U; s < segments; s++) {
        size_t n = buildSegment(rng, segment, seed_b, other_len);
        if ((g_stream.length + n) > kStreamMax) {
            break;
        }
        memcpy(&g_stream.bytes[g_stream.length], segment, n);
        g_stream.length += n;
    }
    for (size_t i = 0U; i < g_stream.length; i++) {
        now += pickGap(rng);
        g_stream.ms[i] = now;
    }
}

/**
 * @brief Records a failure of the stream with a note naming the byte.
 *
 * @param[in]     kind   failure kind
 * @param[in]     index  input index
 * @param[in]     at     index of the byte in the stream
 * @param[in]     what   what differed
 */
void failAt(const char *kind, uint64_t index, size_t at, const char *what)
{
    char note[200];

    (void)snprintf(note, sizeof note, "byte %zu of %zu: %s", at, g_stream.length, what);
    fz_fail(kind, index, g_stream.bytes, g_stream.length, note);
}

/**
 * @brief Checks a frame the controller accepted against the raw bytes and the onboard
 * computer's decoder.
 *
 * @param[in]     index  input index
 * @param[in]     at     index of the byte that completed the frame
 * @param[in]     f      the controller's decoded frame
 */
void checkAccepted(uint64_t index, size_t at, const Frame &f)
{
    size_t total = kFrameHeaderBytes + f.length + kFrameCheckBytes;
    frame::Frame d{};

    if ((at + 1U) < total) {
        failAt("accepted-short", index, at, "accepted before as many bytes as its Length");
        return;
    }
    const uint8_t *raw = &g_stream.bytes[at + 1U - total];
    if (!frame::decode(raw, total, d)) {
        failAt("accepted-bad", index, at, "controller accepted; onboard decode refuses the bytes");
        return;
    }
    if ((d.type != static_cast<uint8_t>(f.type)) ||
        (d.destination != static_cast<uint8_t>(f.destination)) || (d.flags != f.flags) ||
        (d.length != f.length) || (d.sequence != f.sequence) || (d.expiry != f.expiry) ||
        (memcmp(d.payload, f.payload, f.length) != 0)) {
        failAt("fields", index, at, "decoded fields differ from the onboard decode");
    }
    if ((f.flags & kFlagsUnused) != 0U) {
        failAt("flags", index, at, "accepted a frame with Flags bits 1 to 7 set");
    }
    for (size_t j = at + 2U - total; j <= at; j++) {
        uint32_t gap = static_cast<uint32_t>(g_stream.ms[j])
            - static_cast<uint32_t>(g_stream.ms[j - 1U]);
        if (gap > kFrameGapMaxMs) {
            failAt("accepted-gap", index, at, "accepted a frame with a gap above 10 ms inside it");
            break;
        }
    }
    g_frames_accepted++;
}

/**
 * @brief Feeds the stream to both parsers and compares them byte by byte.
 *
 * @param[in]     index  input index
 */
void runStream(uint64_t index)
{
    g_firmware.reset();
    g_onboard.reset();
    for (size_t i = 0U; i < g_stream.length; i++) {
        Frame ff{};
        frame::Frame uf{};
        bool fa = g_firmware.feed(g_stream.bytes[i], static_cast<uint32_t>(g_stream.ms[i]), ff);
        bool ua = g_onboard.feed(g_stream.bytes[i], g_stream.ms[i], uf);

        g_bytes_fed++;
        if (fa) {
            checkAccepted(index, i, ff);
        }
        if (fa && !ua) {
            failAt("controller-only", index, i, "controller accepted, onboard parser did not");
        } else if (ua && !fa) {
            if ((uf.flags & kFlagsUnused) != 0U) {
                g_flags_difference++;
            } else {
                failAt("onboard-only", index, i, "onboard parser accepted, controller did not");
            }
        }
    }
}

/**
 * @brief Checks the first segment alone, fed with no gaps to a fresh parser: if the onboard
 * decoder takes the bytes as one frame (and Flags is right), the controller must accept at the
 * last byte.
 *
 * @param[in]     index   input index
 * @param[in]     first   size of the first segment (0 if none was recorded)
 */
void runWholeFrame(uint64_t index, size_t first)
{
    frame::Frame d{};
    bool should = (first >= kMinFrame) && frame::decode(g_stream.bytes, first, d) &&
                  ((d.flags & kFlagsUnused) == 0U);
    bool got = false;
    Frame ff{};

    g_whole.reset();
    for (size_t i = 0U; i < first; i++) {
        got = g_whole.feed(g_stream.bytes[i], 0U, ff);
    }
    g_whole_checked++;
    if (should && !got) {
        failAt("whole-refused", index, first,
               "onboard decode takes the bytes as a frame; controller did not");
    }
}

/**
 * @brief Appends a clean frame after a gap above 10 ms to the end of the stream and checks that
 * both parsers accept it (Interfaces Sec. 6.2).
 *
 * @param[in,out] rng    the generator
 * @param[in]     index  input index
 */
void runRecovery(fz_rng_t *rng, uint64_t index)
{
    static uint8_t clean[kSegmentMax];
    size_t n = buildSeed(rng, clean);
    uint64_t now = g_stream.ms[(g_stream.length > 0U) ? (g_stream.length - 1U) : 0U];
    bool fa = false;
    bool ua = false;
    Frame ff{};
    frame::Frame uf{};

    now += kFrameGapMaxMs + 1U + fz_rng_below(rng, kRecoveryGapSpread);
    for (size_t i = 0U; i < n; i++) {
        fa = g_firmware.feed(clean[i], static_cast<uint32_t>(now), ff);
        ua = g_onboard.feed(clean[i], now, uf);
        now += fz_rng_below(rng, 2U);
    }
    g_recovery_checked++;
    if (!fa || !ua) {
        failAt("recovery", index, g_stream.length,
               fa ? "onboard parser lost a clean frame after a gap"
                  : "controller lost a clean frame after a gap");
    } else if ((ff.sequence != uf.sequence) || (readU16(clean + kFrameOffsetLength) != ff.length)) {
        failAt("recovery", index, g_stream.length, "clean frame decoded with other fields");
    }
}

/**
 * @brief Runs one input.
 *
 * @param[in]     seed   run seed
 * @param[in]     index  input index
 */
void runInput(uint64_t seed, uint64_t index)
{
    fz_rng_t rng;
    bool faulted = false;

    fz_rng_seed(&rng, seed, index);
    buildStream(&rng);
    fz_input_begin(index, g_stream.bytes, g_stream.length);
    fz_fault_clear();
    FZ_FAULT_GUARD(faulted, runStream(index));
    {
        size_t first = 0U;
        /* The first segment starts after the noise; whole-frame check only when there is none:
         * the stream is then the segment, and its size is read back from its own Length field. */
        if (g_stream.length >= kFrameHeaderBytes) {
            first = kFrameHeaderBytes + readU16(&g_stream.bytes[kFrameOffsetLength])
                + kFrameCheckBytes;
        }
        if (first <= g_stream.length) {
            FZ_FAULT_GUARD(faulted, runWholeFrame(index, first));
        }
    }
    FZ_FAULT_GUARD(faulted, runRecovery(&rng, index));
    if (faulted || (fz_fault_calls != 0U)) {
        failAt("fault-reaction", index, 0U, "a byte from the link reached the fault reaction");
    }
    fz_input_end();
}

}  // namespace

/**
 * @brief Runs the frame-parser fuzz target.
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
    fz_run_open("fz_frame", &options, argc, argv);
    last = options.count;
    if (options.one) {
        first = options.one_index;
        last = options.one_index + 1U;
    }
    for (uint64_t index = first; index < last; index++) {
        runInput(options.seed, index);
    }
    fz_log("frames accepted by the controller: %llu; bytes fed: %llu",
           static_cast<unsigned long long>(g_frames_accepted),
               static_cast<unsigned long long>(g_bytes_fed));
    fz_log("whole-frame checks: %llu; recovery checks: %llu; accepted by the onboard parser only, "
           "Flags bits 1 to 7 set (by design, not a finding): %llu",
           static_cast<unsigned long long>(g_whole_checked),
               static_cast<unsigned long long>(g_recovery_checked),
           static_cast<unsigned long long>(g_flags_difference));
    return fz_run_finish(last - first);
}
