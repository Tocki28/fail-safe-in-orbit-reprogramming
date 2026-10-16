/*
 * fuzz_frames.cpp — fuzz targets 1 and 2 (fuzz_frames.hpp).
 *
 * Oracles. (1) The pinned C reference of commit 5c0b5f8 (frame.c, the controller's frame layer),
 * built in build/ref by the Makefile exactly as test/onboard-computer/Makefile does. (2) A spec
 * decoder written here from Interfaces Sec. 4.0 (docs/interfaces/main.tex), with its own CRC-32.
 * Any disagreement on accept or refuse, or on a decoded field, is a finding.
 */
#include "fuzz_frames.hpp"

#include <array>
#include <cstring>

extern "C" {
#include "frame.h"
}

#include "frame.hpp"

namespace fuzz
{

namespace
{

// Interfaces Sec. 4.0 Figure 2: the header is 16 bytes, the check value 4, the payload 0 to 1024.
constexpr size_t kHeader = 16;
constexpr size_t kCheck = 4;
constexpr size_t kPayloadLimit = 1024;
// Largest datagram built: a maximal frame plus a few bytes of extension.
constexpr size_t kDatagramMax = kHeader + kPayloadLimit + kCheck + 64;
// Offsets of the header fields (Interfaces Sec. 4.0 Figure 2).
constexpr size_t kOffLength = 6;
constexpr size_t kOffSequence = 8;
constexpr size_t kOffExpiry = 12;
// The longest stream one parser input holds, bytes. Bench value: a few frames and noise.
constexpr size_t kStreamMax = 3000;
// The gap rule of Interfaces Sec. 6.2: more than 10 ms inside a frame drops it.
constexpr uint64_t kGapMaxMs = 10;

// A frame as the spec decoder reads it.
struct SpecFrame
{
  uint8_t type;
  uint8_t destination;
  uint8_t flags;
  uint16_t length;
  uint32_t sequence;
  uint32_t expiry;
};

/**
 * @brief Builds one valid frame with the C reference's builder.
 *
 * @param[in]     type     Type byte
 * @param[in]     dest     Destination byte
 * @param[in]     flags    Flags byte
 * @param[in]     seq      sequence number
 * @param[in]     expiry   expiry
 * @param[in]     payload  payload bytes
 * @return the frame, header to check value
 */
std::vector<uint8_t> buildRef(uint8_t type, uint8_t dest, uint8_t flags, uint32_t seq,
  uint32_t expiry, const std::vector<uint8_t> & payload)
{
  frame_t f{};
  f.type = type;
  f.destination = dest;
  f.flags = flags;
  f.length = static_cast<uint16_t>(payload.size());
  f.sequence = seq;
  f.expiry = expiry;
  f.payload = payload.data();
  std::vector<uint8_t> out(kHeader + payload.size() + kCheck);
  uint32_t n = frame_build(&f, out.data(), static_cast<uint32_t>(out.size()));
  out.resize(n);
  return out;
}

/**
 * @brief The seed frames: one of each shape the ground and the controller send.
 *
 * Commands to both destinations with the payload sizes of Interfaces Sec. 5.1, answers of 6
 * bytes, HEALTH and EVENT reports (Sec. 5.4), the empty payload and the maximal one.
 *
 * @param[in,out] rng  the generator (payload bytes)
 * @return the seeds
 */
std::vector<std::vector<uint8_t>> frameSeeds(Rng & rng)
{
  std::vector<std::vector<uint8_t>> seeds;
  const size_t sizes[] = {0, 1, 2, 5, 6, 7, 24, 34, 47, 263, 1023, 1024};
  const uint8_t types[] = {1, 2, 3};
  const uint8_t dests[] = {1, 2, 3};
  for (size_t s : sizes) {
    for (uint8_t t : types) {
      uint8_t d = dests[below(rng, 3U)];
      seeds.push_back(buildRef(t, d, 0, static_cast<uint32_t>(nextU64(rng)),
        static_cast<uint32_t>(nextU64(rng)), randomBytes(rng, s)));
    }
  }
  return seeds;
}

/**
 * @brief Spec decoder of Interfaces Sec. 4.0 for one whole datagram.
 *
 * Sync A5 5A, Version 01, Length 0 to 1024 equal to what follows the header less the check value,
 * CRC-32 of every byte before it. No other field is checked.
 *
 * @param[in]     b     the bytes
 * @param[in]     size  number of bytes
 * @param[out]    f     the fields when true is returned
 * @return true: the datagram is one valid frame
 */
bool specDecode(const uint8_t * b, size_t size, SpecFrame & f)
{
  if (size < kHeader + kCheck || b[0] != 0xA5U || b[1] != 0x5AU || b[2] != 0x01U) {
    return false;
  }
  const uint16_t length = readLe16(&b[kOffLength]);
  if (length > kPayloadLimit || kHeader + length + kCheck != size) {
    return false;
  }
  if (refCrc32(b, kHeader + length) != readLe32(&b[kHeader + length])) {
    return false;
  }
  f = SpecFrame{b[3], b[4], b[5], length, readLe32(&b[kOffSequence]), readLe32(&b[kOffExpiry])};
  return true;
}

/**
 * @brief The C reference parser fed one whole datagram, as one datagram is judged.
 *
 * Accepted only if the frame completes on the last byte and covers the whole datagram. The
 * reference has no datagram call; a garbage prefix that makes it re-synchronise inside the
 * datagram does not count (the updater's decode wants Sync at byte 0).
 *
 * @param[in]     b     the bytes
 * @param[in]     size  number of bytes
 * @param[out]    f     the reference's fields when true is returned
 * @param[out]    payload  copy of the reference's payload
 * @return true: the reference accepts the datagram as one frame
 */
bool refDatagram(const uint8_t * b, size_t size, frame_t & f, std::vector<uint8_t> & payload)
{
  static frame_parser_t parser;
  frame_parser_init(&parser);
  for (size_t i = 0; i < size; i++) {
    frame_t out{};
    if (frame_parser_feed(&parser, b[i], 0U, &out)) {
      if (i + 1U == size && kHeader + out.length + kCheck == size && b[0] == 0xA5U) {
        f = out;
        payload.assign(out.payload, out.payload + out.length);
        return true;
      }
      return false;
    }
  }
  return false;
}

/**
 * @brief Makes one datagram for the decoder target.
 *
 * Ten per cent random bytes, ten per cent a splice of two seeds, the rest a seed with one to
 * four mutations; half of the mutated ones get their CRC-32 recomputed afterwards (so that the
 * header checks, not the CRC-32, decide) and a quarter get Length set to a boundary value first.
 *
 * @param[in,out] rng    the generator
 * @param[in]     seeds  the seed frames
 * @return the datagram
 */
std::vector<uint8_t> makeDatagram(Rng & rng, const std::vector<std::vector<uint8_t>> & seeds)
{
  const uint32_t mode = below(rng, 100U);
  if (mode < 10U) {
    return randomBytes(rng, below(rng, 1100U));
  }
  std::vector<uint8_t> v = seeds[below(rng, static_cast<uint32_t>(seeds.size()))];
  const std::vector<uint8_t> & other = seeds[below(rng, static_cast<uint32_t>(seeds.size()))];
  const std::vector<Field> fields = {{0, 2}, {2, 1}, {3, 1}, {4, 1}, {5, 1}, {kOffLength, 2},
    {kOffSequence, 4}, {kOffExpiry, 4}};
  if (mode >= 90U) {
    mutate(rng, v, fields, kDatagramMax, &other);
    return v;
  }
  if (chance(rng, 25U) && v.size() >= kHeader) {
    const uint64_t value = boundaryValue(rng, 2U);
    writeLe16(&v[kOffLength], static_cast<uint16_t>(value));
  }
  mutate(rng, v, fields, kDatagramMax, &other);
  if (chance(rng, 50U) && v.size() >= kHeader + kCheck) {
    writeLe32(&v[v.size() - kCheck], refCrc32(v.data(), v.size() - kCheck));
  }
  return v;
}

/**
 * @brief Compares the updater's decoded fields with the spec decoder's and the reference's.
 *
 * @param[in]     f     the updater's frame
 * @param[in]     b     the datagram
 * @param[in]     s     the spec decoder's fields
 * @param[in]     rf    the reference's fields
 * @param[in]     rp    the reference's payload
 * @return true: all equal
 */
bool fieldsEqual(const frame::Frame & f, const uint8_t * b, const SpecFrame & s,
  const frame_t & rf, const std::vector<uint8_t> & rp)
{
  return f.type == s.type && f.destination == s.destination && f.flags == s.flags &&
         f.length == s.length && f.sequence == s.sequence && f.expiry == s.expiry &&
         f.type == rf.type && f.destination == rf.destination && f.flags == rf.flags &&
         f.length == rf.length && f.sequence == rf.sequence && f.expiry == rf.expiry &&
         f.payload == &b[kHeader] && rp.size() == f.length &&
         (f.length == 0U || std::memcmp(f.payload, rp.data(), f.length) == 0);
}

/**
 * @brief Checks frame::decodeAnswer on one synthetic frame.
 *
 * Expected (Interfaces Sec. 4.2; frame.hpp): true exactly when Type is 0x02 and Length is 6; the
 * fields are the little-endian sequence number, the kind byte and the code byte; otherwise the
 * answer is left as it was.
 *
 * @param[in,out] r      the report
 * @param[in]     index  input index
 * @param[in,out] rng    the generator
 * @return nothing
 */
void checkDecodeAnswer(Report & r, uint64_t index, Rng & rng)
{
  std::array<uint8_t, 16> payload{};
  for (auto & x : payload) {
    x = static_cast<uint8_t>(nextU64(rng));
  }
  frame::Frame f{};
  f.type = chance(rng, 60U) ? frame::kTypeAnswer : static_cast<uint8_t>(nextU64(rng));
  f.length = chance(rng, 60U) ? static_cast<uint16_t>(frame::kAnswerBytes)
                              : static_cast<uint16_t>(below(rng, 12U));
  f.payload = payload.data();
  frame::Answer a{0xDEADBEEFU, 0xA5U, 0x5AU};
  const frame::Answer before = a;
  const bool got = frame::decodeAnswer(f, a);
  const bool want = f.type == 0x02U && f.length == 6U;
  bool ok = (got == want);
  if (got && want) {
    ok = ok && a.answered == readLe32(payload.data()) && a.kind == payload[4] &&
         a.code == payload[5];
  }
  if (!got) {
    ok = ok && a.answered == before.answered && a.kind == before.kind && a.code == before.code;
  }
  if (!ok) {
    finding(r, "decodeAnswer disagrees with Sec. 4.2", index, payload.data(), payload.size(),
      "type/length/fields differ from the rule: true iff Type 0x02 and Length 6");
  }
}

}  // namespace

/**
 * @brief Target 1: frame::decode and frame::decodeAnswer against the reference and the spec.
 *
 * For each datagram: the updater's decode, the C reference parser and the spec decoder must agree
 * on accept or refuse and on every field; on a refusal the output frame must stay as it was; on
 * an accept the payload pointer must be byte 16 of the datagram. Frames with Flags bits 1 to 7,
 * Type or Destination outside the lists are counted: decode accepts them and no requirement
 * found says it must refuse.
 *
 * @param[in,out] r  the report (options, log)
 * @return number of findings
 */
int runFrameDecode(Report & r)
{
  Rng seed_rng = rngFor(r.opt.seed, ~0ULL);
  const std::vector<std::vector<uint8_t>> seeds = frameSeeds(seed_rng);
  uint64_t accepted = 0;
  uint64_t odd_flags = 0;
  uint64_t odd_type = 0;
  uint64_t odd_dest = 0;
  const uint64_t lo = r.opt.has_only ? r.opt.only : 0U;
  const uint64_t hi = r.opt.has_only ? r.opt.only + 1U : r.opt.count;
  for (uint64_t i = lo; i < hi; i++) {
    Rng rng = rngFor(r.opt.seed, i);
    const std::vector<uint8_t> d = makeDatagram(rng, seeds);
    frame::Frame f{};
    std::memset(&f, 0xA5, sizeof f);
    frame::Frame untouched = f;
    inputBegin(r, i, d.data(), d.size());
    const bool got = frame::decode(d.data(), d.size(), f);
    inputEnd(r, i);
    SpecFrame sf{};
    frame_t rf{};
    std::vector<uint8_t> rp;
    const bool spec = specDecode(d.data(), d.size(), sf);
    const bool ref = refDatagram(d.data(), d.size(), rf, rp);
    if (got != spec || got != ref) {
      finding(r, "decode, spec decoder and C reference disagree on accept/refuse", i, d.data(),
        d.size(), got ? "updater accepts" : "updater refuses");
    } else if (got && !fieldsEqual(f, d.data(), sf, rf, rp)) {
      finding(r, "decode returns a field that differs from the spec/reference", i, d.data(),
        d.size(), "type, destination, flags, length, sequence, expiry or payload differ");
    } else if (!got && std::memcmp(&f, &untouched, sizeof f) != 0) {
      finding(r, "decode changed the output frame on a refusal", i, d.data(), d.size(),
        "frame.hpp: f is unchanged when false is returned");
    }
    if (got) {
      accepted++;
      odd_flags += ((f.flags & 0xFEU) != 0U) ? 1U : 0U;
      odd_type += (f.type < 1U || f.type > 3U) ? 1U : 0U;
      odd_dest += (f.destination < 1U || f.destination > 3U) ? 1U : 0U;
    }
    checkDecodeAnswer(r, i, rng);
    progress(r, i);
  }
  (void)std::fprintf(r.log, "decode accepted %llu datagrams; of these Flags bits 1-7 set: %llu, "
    "Type outside 1-3: %llu, Destination outside 1-3: %llu\n",
    static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(odd_flags),
    static_cast<unsigned long long>(odd_type), static_cast<unsigned long long>(odd_dest));
  if (odd_flags > 0U || odd_type > 0U || odd_dest > 0U) {
    note(r, "expected behaviour not specified: frame::decode accepts a frame with Flags bits 1-7 "
      "set (Sec. 4.0 says other bits 0), or Type/Destination outside the listed values; the C "
      "reference does the same. Whether the decoder or a later check must refuse them is a "
      "question for Tom.");
  }
  return static_cast<int>(r.findings);
}

namespace
{

/**
 * @brief Makes one byte stream with its arrival times for the parser target.
 *
 * Segments of seed frames (some mutated, some with the CRC-32 fixed), noise, Sync-like prefixes
 * and repeated 0xA5. Gaps between bytes are 0 ms mostly; 1 to 10 ms, exactly 10, exactly 11, 12 to
 * 100 ms and thousands of ms now and then, and a rare step backwards. The start time is 0, near
 * 2^32 (to cross the 32-bit wrap of the reference) or near 2^33.
 *
 * @param[in,out] rng     the generator
 * @param[in]     seeds   the seed frames
 * @param[out]    bytes   the stream
 * @param[out]    times   arrival time of each byte, ms
 * @return nothing
 */
void makeStream(Rng & rng, const std::vector<std::vector<uint8_t>> & seeds,
  std::vector<uint8_t> & bytes, std::vector<uint64_t> & times)
{
  bytes.clear();
  times.clear();
  const uint32_t segments = 1U + below(rng, 6U);
  const std::vector<Field> fields = {{kOffLength, 2}, {kOffSequence, 4}, {2, 1}, {0, 2}};
  for (uint32_t s = 0; s < segments && bytes.size() < kStreamMax; s++) {
    std::vector<uint8_t> seg;
    const uint32_t kind = below(rng, 10U);
    if (kind < 5U) {
      seg = seeds[below(rng, static_cast<uint32_t>(seeds.size()))];
    } else if (kind < 7U) {
      seg = seeds[below(rng, static_cast<uint32_t>(seeds.size()))];
      mutate(rng, seg, fields, kDatagramMax, &seeds[0]);
      if (chance(rng, 50U) && seg.size() >= kHeader + kCheck) {
        writeLe32(&seg[seg.size() - kCheck], refCrc32(seg.data(), seg.size() - kCheck));
      }
    } else if (kind == 7U) {
      seg = randomBytes(rng, 1U + below(rng, 60U));
    } else if (kind == 8U) {
      seg.assign(1U + below(rng, 5U), 0xA5U);
      seg.push_back(0x5AU);
      seg.push_back(0x01U);
    } else {
      seg = {0xA5U, 0x5AU, 0x01U, 0x01U, 0x01U, 0x00U, 0xFFU, 0xFFU};
    }
    bytes.insert(bytes.end(), seg.begin(), seg.end());
  }
  if (bytes.size() > kStreamMax) {
    bytes.resize(kStreamMax);
  }
  const uint32_t start_kind = below(rng, 3U);
  uint64_t t = (start_kind == 0U) ? 0U
             : (start_kind == 1U) ? (0x100000000ULL - 3000U + below(rng, 1000U))
                                  : (0x200000000ULL + below(rng, 1000U));
  for (size_t k = 0; k < bytes.size(); k++) {
    const uint32_t g = below(rng, 1000U);
    if (g < 800U) {
      // same ms
    } else if (g < 900U) {
      t += 1U + below(rng, 10U);
    } else if (g < 950U) {
      t += kGapMaxMs;
    } else if (g < 980U) {
      t += kGapMaxMs + 1U;
    } else if (g < 995U) {
      t += 12U + below(rng, 90U);
    } else if (g < 999U) {
      t += 1000U + below(rng, 4000U);
    } else if (t > 20U) {
      t -= 1U + below(rng, 19U);
    }
    times.push_back(t);
  }
}

/**
 * @brief Checks that a frame the parser returned lies in the stream and obeys the gap rule.
 *
 * The frame is re-encoded; the bytes must equal the stream window that ends at the current byte,
 * and no two neighbouring bytes of that window may be more than 10 ms apart (Interfaces Sec. 6.2).
 *
 * @param[in]     f      the frame returned
 * @param[in]     bytes  the stream
 * @param[in]     times  arrival times
 * @param[in]     at     index of the byte that completed the frame
 * @return true: the frame is in the stream and obeys the gap rule
 */
bool windowOk(const frame::Frame & f, const std::vector<uint8_t> & bytes,
  const std::vector<uint64_t> & times, size_t at)
{
  std::array<uint8_t, frame::kMaxBytes> enc{};
  const size_t n = frame::encode(f, enc.data(), enc.size());
  if (n == 0U || n > at + 1U) {
    return false;
  }
  const size_t from = at + 1U - n;
  if (std::memcmp(&bytes[from], enc.data(), n) != 0) {
    return false;
  }
  for (size_t k = from + 1U; k <= at; k++) {
    if (times[k] - times[k - 1U] > kGapMaxMs) {
      return false;
    }
  }
  return true;
}

}  // namespace

/**
 * @brief Target 2: frame::Parser::feed against the C reference parser, byte by byte.
 *
 * Both parsers get the same bytes and the same times. At every byte they must agree on whether a
 * frame completed and on its fields. A frame the updater's parser returns must also lie in the
 * stream and obey the 10 ms gap rule, and the output frame must stay as it was when none
 * completes.
 *
 * @param[in,out] r  the report (options, log)
 * @return number of findings
 */
int runFrameParser(Report & r)
{
  Rng seed_rng = rngFor(r.opt.seed, ~0ULL);
  const std::vector<std::vector<uint8_t>> seeds = frameSeeds(seed_rng);
  static frame_parser_t ref;
  uint64_t frames = 0;
  uint64_t bytes_fed = 0;
  const uint64_t lo = r.opt.has_only ? r.opt.only : 0U;
  const uint64_t hi = r.opt.has_only ? r.opt.only + 1U : r.opt.count;
  for (uint64_t i = lo; i < hi; i++) {
    Rng rng = rngFor(r.opt.seed, i);
    std::vector<uint8_t> bytes;
    std::vector<uint64_t> times;
    makeStream(rng, seeds, bytes, times);
    frame::Parser parser;
    frame_parser_init(&ref);
    bool bad = false;
    inputBegin(r, i, bytes.data(), bytes.size());
    for (size_t k = 0; k < bytes.size() && !bad; k++) {
      frame::Frame f{};
      std::memset(&f, 0xA5, sizeof f);
      const frame::Frame untouched = f;
      const bool got = parser.feed(bytes[k], times[k], f);
      frame_t rf{};
      const bool want = frame_parser_feed(&ref, bytes[k], static_cast<uint32_t>(times[k]), &rf);
      bytes_fed++;
      if (got != want) {
        finding(r, "Parser and C reference disagree on a frame completing", i, bytes.data(),
          bytes.size(), got ? "updater completes a frame the reference does not"
                            : "reference completes a frame the updater does not");
        bad = true;
      } else if (got) {
        frames++;
        const bool same = f.type == rf.type && f.destination == rf.destination &&
          f.flags == rf.flags && f.length == rf.length && f.sequence == rf.sequence &&
          f.expiry == rf.expiry &&
          (f.length == 0U || std::memcmp(f.payload, rf.payload, f.length) == 0);
        if (!same) {
          finding(r, "Parser returns a field that differs from the C reference", i, bytes.data(),
            bytes.size(), "a decoded field or the payload differs");
          bad = true;
        } else if (!windowOk(f, bytes, times, k)) {
          finding(r, "Parser returned a frame that is not in the stream or breaks the gap rule", i,
            bytes.data(), bytes.size(), "re-encoded frame is not the stream window ending here");
          bad = true;
        }
      } else if (std::memcmp(&f, &untouched, sizeof f) != 0) {
        finding(r, "Parser changed the output frame while returning false", i, bytes.data(),
          bytes.size(), "frame.hpp: f is set only when true is returned");
        bad = true;
      }
    }
    inputEnd(r, i);
    progress(r, i);
  }
  (void)std::fprintf(r.log, "parser: %llu bytes fed to each parser, %llu frames completed\n",
    static_cast<unsigned long long>(bytes_fed), static_cast<unsigned long long>(frames));
  return static_cast<int>(r.findings);
}

}  // namespace fuzz
