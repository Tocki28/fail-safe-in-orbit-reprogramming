/*
 * fuzz_images.cpp — fuzz target 4 (fuzz_images.hpp).
 *
 * Oracle (task brief): an image whose signed bytes (header and body) differ from those of every
 * valid seed image must never be accepted; an accepted image must also satisfy every structural
 * rule of image_checker.hpp, written again here from image_format.h and Interfaces Sec. 5.5 with
 * an independent CRC-32. A refused image that is byte-for-byte a valid seed, with limits it meets,
 * is a false refusal.
 */
#include "fuzz_images.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

#include "config.hpp"
#include "image_checker.hpp"
#include "image_file_check.hpp"
#include "keys.h"
#include "protocol.hpp"

namespace fuzz
{

namespace
{

// Image layout (image_format.h, Interfaces Sec. 5.5 Figure 6).
constexpr size_t kHeaderBytes = 64;
constexpr size_t kSigBytes = 64;
constexpr size_t kOverhead = kHeaderBytes + 2 * kSigBytes;
// The largest image the checker takes: one slot, 0xA000 (layout.h SLOT_BYTES).
constexpr size_t kImageMax = 0xA000;
// Header field offsets (image_format.h).
constexpr size_t kOffFormat = 4;
constexpr size_t kOffKind = 5;
constexpr size_t kOffTarget = 6;
constexpr size_t kOffVersion = 8;
constexpr size_t kOffLength = 12;
constexpr size_t kOffBodyCrc = 16;
// Region sizes for --check-image (layout.h): bootloader, last-resort. The slot is kImageMax.
constexpr size_t kBootloaderRegion = 0x6000;
constexpr size_t kLastResortRegion = 0x4000;
// The group order L of Ed25519, little-endian [RFC 8032, Sec. 5.1]:
// 2^252 + 27742317777372353535851937790883648493.
constexpr uint8_t kGroupOrderLe[32] = {
  0xEDU, 0xD3U, 0xF5U, 0x5CU, 0x1AU, 0x63U, 0x12U, 0x58U, 0xD6U, 0x9CU, 0xF7U, 0xA2U, 0xDEU, 0xF9U,
  0xDEU, 0x14U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  0x00U, 0x00U, 0x00U, 0x10U};
// Longest --kind word buffer the word fuzzer builds.
constexpr size_t kWordMax = 24;

// One seed image and what the checker says about it with the default limits.
struct Seed
{
  std::string name;
  std::vector<uint8_t> bytes;
  bool valid;
};

// The set of valid signed regions and their signatures.
struct ValidSet
{
  std::vector<Seed> seeds;
  std::map<std::vector<uint8_t>, std::pair<std::vector<uint8_t>,
      std::vector<uint8_t>>> signed_to_sigs;
};

/**
 * @brief Reads a whole file.
 *
 * @param[in]     path  file name
 * @param[out]    out   the bytes
 * @return true: read; false: could not be read
 */
bool readFile(const std::string & path, std::vector<uint8_t> & out)
{
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  out.clear();
  std::array<uint8_t, 4096> buf{};
  for (size_t i = 0; i < 100000U; i++) {   // bounded: 400 MB at most
    ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n <= 0) {
      break;
    }
    out.insert(out.end(), buf.begin(), buf.begin() + n);
  }
  (void)::close(fd);
  return true;
}

/**
 * @brief Loads every .img file of a directory as a seed and classifies it.
 *
 * @param[in]     dir      directory
 * @param[in]     checker  the checker (default limits decide valid or not)
 * @param[in,out] set      the seed set to add to
 * @return nothing
 */
void loadSeeds(const std::string & dir, const ImageChecker & checker, ValidSet & set)
{
  DIR * d = ::opendir(dir.c_str());
  if (d == nullptr) {
    return;
  }
  std::vector<std::string> names;
  for (size_t i = 0; i < 1000U; i++) {   // bounded
    struct dirent * e = ::readdir(d);
    if (e == nullptr) {
      break;
    }
    std::string n = e->d_name;
    if (n.size() > 4U && n.compare(n.size() - 4U, 4U, ".img") == 0) {
      names.push_back(n);
    }
  }
  (void)::closedir(d);
  for (const std::string & n : names) {
    Seed s;
    s.name = dir + "/" + n;
    if (!readFile(s.name, s.bytes) || s.bytes.size() <= kOverhead) {
      continue;
    }
    s.valid = checker.check(s.bytes.data(), s.bytes.size()).code == 0U;
    if (s.valid) {
      const long cut = static_cast<long>(s.bytes.size() - 2U * kSigBytes);
      const long sig = static_cast<long>(kSigBytes);
      set.signed_to_sigs[std::vector<uint8_t>(s.bytes.begin(), s.bytes.begin() + cut)] =
        {std::vector<uint8_t>(s.bytes.begin() + cut, s.bytes.begin() + cut + sig),
         std::vector<uint8_t>(s.bytes.begin() + cut + sig, s.bytes.end())};
    }
    set.seeds.push_back(s);
  }
}

/**
 * @brief Limits chosen for one input: the defaults most of the time.
 *
 * @param[in,out] rng  the generator
 * @return the limits
 */
ImageLimits makeLimits(Rng & rng)
{
  ImageLimits l = defaultImageLimits();
  if (chance(rng, 30U)) {
    l.minimum_version = static_cast<uint32_t>(boundaryValue(rng, 4U));
    l.identity = chance(rng, 50U) ? static_cast<uint16_t>(boundaryValue(rng, 2U))
                                  : l.identity;
    l.usable = !chance(rng, 15U);
  }
  return l;
}

/**
 * @brief Makes one image input.
 *
 * @param[in,out] rng   the generator
 * @param[in]     set   the seeds
 * @return the bytes
 */
std::vector<uint8_t> makeImage(Rng & rng, const ValidSet & set)
{
  const Seed & s = set.seeds[below(rng, static_cast<uint32_t>(set.seeds.size()))];
  const Seed & other = set.seeds[below(rng, static_cast<uint32_t>(set.seeds.size()))];
  std::vector<uint8_t> v = s.bytes;
  const uint32_t mode = below(rng, 100U);
  if (mode < 5U) {
    return randomBytes(rng, below(rng, 3000U));
  }
  const size_t sig_at = v.size() - 2U * kSigBytes;
  if (mode < 10U) {
    // Signature malleability: S + L in place of S (RFC 8032, Sec. 5.1.7 wants S < L).
    const size_t at = sig_at + (chance(rng, 50U) ? 0U : kSigBytes) + 32U;
    unsigned carry = 0;
    for (size_t k = 0; k < 32U; k++) {
      unsigned sum = static_cast<unsigned>(v[at + k]) + kGroupOrderLe[k] + carry;
      v[at + k] = static_cast<uint8_t>(sum);
      carry = sum >> 8;
    }
    return v;
  }
  if (mode < 15U) {
    // Signatures taken from another image.
    const size_t o_at = other.bytes.size() - 2U * kSigBytes;
    v.resize(sig_at);
    v.insert(v.end(), other.bytes.begin() + static_cast<long>(o_at), other.bytes.end());
    return v;
  }
  std::vector<Field> fields = {{0, 4}, {kOffFormat, 1}, {kOffKind, 1}, {kOffTarget, 2},
    {kOffVersion, 4}, {kOffLength, 4}, {kOffBodyCrc, 4}, {20, 1}, {52, 4}, {56, 8},
    {sig_at, 8}, {sig_at + 32U, 8}, {sig_at + kSigBytes, 8}, {sig_at + kSigBytes + 32U, 8}};
  mutate(rng, v, fields, kImageMax + 64U, &other.bytes);
  if (mode >= 40U && v.size() > kOverhead) {
    // Fix-up: make Length and the body check value right, so the signatures decide.
    writeLe32(&v[kOffLength], static_cast<uint32_t>(v.size() - kOverhead));
    writeLe32(&v[kOffBodyCrc], refCrc32(&v[kHeaderBytes], v.size() - kOverhead));
  }
  return v;
}

/**
 * @brief Judges one checker verdict against the independent rules.
 *
 * @param[in,out] r       the report
 * @param[in]     index   input index
 * @param[in]     img     the image
 * @param[in]     limits  the limits applied
 * @param[in]     set     the valid seeds
 * @param[in]     v       the checker's verdict
 * @return nothing
 */
void judge(Report & r, uint64_t index, const std::vector<uint8_t> & img, const ImageLimits & limits,
  const ValidSet & set, const ImageVerdict & v)
{
  const uint8_t code = v.code;
  const bool known_code = code == 0x00U || code == 0x06U || code == 0x08U || code == 0x09U ||
                          code == 0x0AU || code == 0x0BU;
  if (!known_code) {
    finding(r, "verdict code outside the image refusal list", index, img.data(), img.size(),
      "code is not 0x00, 0x06, 0x08, 0x09, 0x0A or 0x0B (Interfaces Sec. 4.2)");
  }
  const size_t size = img.size();
  const bool exact_seed = [&]() {
    for (const Seed & s : set.seeds) {
      if (s.valid && s.bytes == img) {
        return true;
      }
    }
    return false;
  }();
  if (code != 0U) {
    if (exact_seed) {
      uint8_t want = 0x00U;
      const uint32_t version = readLe32(&img[kOffVersion]);
      if (!limits.usable) {
        want = 0x0AU;
      } else if (readLe16(&img[kOffTarget]) != limits.identity) {
        want = 0x0BU;
      } else if (version < limits.minimum_version) {
        want = 0x0AU;
      }
      if (want == 0x00U) {
        finding(r, "a valid signed image is refused", index, img.data(), img.size(),
          "the image is byte for byte a valid seed and meets the limits");
      } else if (want != code) {
        finding(r, "refusal code differs from the rule order", index, img.data(), img.size(),
          "usable, target, then version decide the code (image_checker.cpp)");
      }
    }
    return;
  }
  // Accepted: every rule must hold.
  bool rules = limits.usable && size > kOverhead && size <= kImageMax && img[0] == 'F' &&
               img[1] == 'S' && img[2] == 'I' && img[3] == 'R' && img[kOffFormat] == 1U &&
               img[kOffKind] >= 1U && img[kOffKind] <= 3U &&
               readLe16(&img[kOffTarget]) == limits.identity &&
               readLe32(&img[kOffVersion]) >= limits.minimum_version;
  if (rules) {
    rules = readLe32(&img[kOffLength]) == size - kOverhead &&
            readLe32(&img[kOffBodyCrc]) == refCrc32(&img[kHeaderBytes], size - kOverhead);
  }
  if (!rules) {
    finding(r, "image accepted although a header rule is broken", index, img.data(), size,
      "magic, format, kind, target, version, length, body check value or size");
    return;
  }
  const size_t cut = size - 2U * kSigBytes;
  auto it = set.signed_to_sigs.find(std::vector<uint8_t>(img.begin(),
                                                         img.begin() + static_cast<long>(cut)));
  if (it == set.signed_to_sigs.end()) {
    finding(r, "image accepted whose signed bytes differ from every valid image", index,
      img.data(), size, "header and body are in no valid seed; both signatures verified");
  } else if (std::memcmp(&img[cut], it->second.first.data(), kSigBytes) != 0 ||
             std::memcmp(&img[cut + kSigBytes], it->second.second.data(), kSigBytes) != 0) {
    finding(r, "image accepted with signature bytes that differ from the valid ones", index,
      img.data(), size, "same signed bytes as a valid seed, other signature bytes (malleable)");
  }
}

/**
 * @brief The exhaustive pass: every bit flip, truncation and extension of every valid seed.
 *
 * Also reports the byte ranges that no single-bit flip makes refused (none expected: header and
 * body are signed, each signature is checked, and nothing else is in the image).
 *
 * @param[in,out] r        the report
 * @param[in]     checker  the checker
 * @param[in]     set      the seeds
 * @return nothing
 */
void exhaustive(Report & r, const ImageChecker & checker, const ValidSet & set)
{
  uint64_t tried = 0;
  for (const Seed & s : set.seeds) {
    if (!s.valid) {
      continue;
    }
    std::vector<uint8_t> v = s.bytes;
    std::vector<size_t> insensitive;
    for (size_t at = 0; at < v.size(); at++) {
      bool any_accepted = false;
      for (unsigned bit = 0; bit < 8U; bit++) {
        v[at] ^= static_cast<uint8_t>(1U << bit);
        inputBegin(r, tried, v.data(), v.size());
        const ImageVerdict res = checker.check(v.data(), v.size());
        inputEnd(r, tried);
        v[at] ^= static_cast<uint8_t>(1U << bit);
        tried++;
        if (res.code == 0U) {
          any_accepted = true;
          finding(r, "single-bit flip of a valid image is accepted", tried, s.bytes.data(),
            s.bytes.size(), (s.name + " byte " + std::to_string(at) + " bit " +
            std::to_string(bit)).c_str());
        }
      }
      if (any_accepted) {
        insensitive.push_back(at);
      }
    }
    for (size_t len = 0; len < v.size(); len++) {
      inputBegin(r, tried, v.data(), len);
      const ImageVerdict res = checker.check(v.data(), len);
      inputEnd(r, tried);
      tried++;
      if (res.code == 0U) {
        finding(r, "truncated valid image is accepted", tried, v.data(), len, s.name.c_str());
      }
    }
    const uint8_t extras[2] = {0x00U, 0xFFU};
    for (uint8_t extra : extras) {
      std::vector<uint8_t> longer = s.bytes;
      longer.push_back(extra);
      if (checker.check(longer.data(), longer.size()).code == 0U) {
        finding(r, "valid image with one extra byte is accepted", tried, longer.data(),
          longer.size(), s.name.c_str());
      }
      tried++;
    }
    (void)std::fprintf(r.log, "exhaustive %s: %zu bytes, bytes where a bit flip stays accepted: "
      "%zu\n", s.name.c_str(), s.bytes.size(), insensitive.size());
  }
  (void)std::fprintf(r.log, "exhaustive pass: %llu inputs; header (0-63) and body are covered by "
    "both signatures, so no byte range lies outside both; the signatures cover each other's bytes "
    "only through their own verification\n", static_cast<unsigned long long>(tried));
}

}  // namespace

/**
 * @brief Target 4: ImageChecker::check on mutated images.
 *
 * @param[in,out] r  the report (options, log)
 * @return number of findings
 */
int runImage(Report & r)
{
  ImageChecker checker(PUBLIC_KEY_A, PUBLIC_KEY_B);
  ValidSet set;
  loadSeeds(r.opt.ctrl_images, checker, set);
  loadSeeds(r.opt.test_images, checker, set);
  if (!checker.ready() || set.seeds.empty() || set.signed_to_sigs.empty()) {
    (void)std::fprintf(r.log, "FATAL: checker not ready or no seed images found\n");
    r.findings++;
    return 1;
  }
  size_t valid = 0;
  for (const Seed & s : set.seeds) {
    valid += s.valid ? 1U : 0U;
  }
  (void)std::fprintf(r.log, "seeds: %zu images, %zu valid with the default limits\n",
    set.seeds.size(), valid);
  if (!r.opt.has_only && std::getenv("FUZZ_SKIP_EXHAUSTIVE") == nullptr) {
    exhaustive(r, checker, set);
  }
  uint64_t accepted = 0;
  const uint64_t lo = r.opt.has_only ? r.opt.only : 0U;
  const uint64_t hi = r.opt.has_only ? r.opt.only + 1U : r.opt.count;
  for (uint64_t i = lo; i < hi; i++) {
    Rng rng = rngFor(r.opt.seed, i);
    const std::vector<uint8_t> img = makeImage(rng, set);
    const ImageLimits limits = makeLimits(rng);
    inputBegin(r, i, img.data(), img.size());
    const ImageVerdict v = checker.check(img.data(), img.size(), limits);
    inputEnd(r, i);
    accepted += (v.code == 0U) ? 1U : 0U;
    judge(r, i, img, limits, set, v);
    progress(r, i);
  }
  (void)std::fprintf(r.log, "mutated inputs accepted: %llu (each equal to a valid seed or a "
    "finding above)\n", static_cast<unsigned long long>(accepted));
  return static_cast<int>(r.findings);
}

namespace
{

/**
 * @brief Checks parseKindName on one word.
 *
 * @param[in,out] r      the report
 * @param[in]     index  input index
 * @param[in,out] rng    the generator
 * @return nothing
 */
void checkKindWord(Report & r, uint64_t index, Rng & rng)
{
  static const char * const words[3] = {"bootloader", "main", "last-resort"};
  std::array<char, kWordMax> word{};
  const uint32_t pick = below(rng, 3U);
  size_t n = 0;
  if (chance(rng, 50U)) {
    n = std::strlen(words[pick]);
    std::memcpy(word.data(), words[pick], n);
    const uint32_t op = below(rng, 5U);
    if (op == 0U) {
      word[below(rng, static_cast<uint32_t>(n))] ^= static_cast<char>(1U << below(rng, 7U));
    } else if (op == 1U && n + 1U < kWordMax) {
      word[n++] = static_cast<char>(' ' + below(rng, 90U));
    } else if (op == 2U) {
      n = below(rng, static_cast<uint32_t>(n));
    } else if (op == 3U) {
      word[below(rng, static_cast<uint32_t>(n))] = 0;   // an early end
    }
  } else {
    n = below(rng, 16U);
    for (size_t k = 0; k < n; k++) {
      word[k] = static_cast<char>(1U + below(rng, 255U));
    }
  }
  word[n] = '\0';
  uint8_t kind = 0xEEU;
  const bool got = parseKindName(word.data(), kind);
  uint8_t want = 0;
  if (std::strcmp(word.data(), "bootloader") == 0) {
    want = 1;
  } else if (std::strcmp(word.data(), "main") == 0) {
    want = 2;
  } else if (std::strcmp(word.data(), "last-resort") == 0) {
    want = 3;
  }
  if (got != (want != 0U) || (got && kind != want) || (!got && kind != 0xEEU)) {
    finding(r, "parseKindName disagrees with the three words", index,
      reinterpret_cast<const uint8_t *>(word.data()), std::strlen(word.data()),
      "true exactly for bootloader, main, last-resort; kind untouched on false");
  }
  if (index % 1000U == 0U && parseKindName(nullptr, kind)) {
    finding(r, "parseKindName accepts a null word", index, nullptr, 0, "null must be refused");
  }
}

/**
 * @brief Writes bytes to a file, replacing it.
 *
 * @param[in]     path  file name
 * @param[in]     data  the bytes
 * @param[in]     size  number of bytes
 * @return true: written
 */
bool writeFile(const std::string & path, const uint8_t * data, size_t size)
{
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return false;
  }
  size_t done = 0;
  for (size_t i = 0; i < 4096U && done < size; i++) {   // bounded
    ssize_t n = ::write(fd, data + done, size - done);
    if (n <= 0) {
      break;
    }
    done += static_cast<size_t>(n);
  }
  (void)::close(fd);
  return done == size;
}

}  // namespace

/**
 * @brief Target 4: parseKindName, checkImageFile and the files behind them.
 *
 * For each input: an image (or a special file), a kind byte, limits and a line buffer of random
 * size with a canary behind it. checkImageFile may return true only for an image the independent
 * rules accept, of the kind asked, that fits its region; the line must be NUL-terminated inside
 * the buffer and the canary must be intact.
 *
 * @param[in,out] r  the report (options, log)
 * @return number of findings
 */
int runImageFile(Report & r)
{
  ImageChecker checker(PUBLIC_KEY_A, PUBLIC_KEY_B);
  ValidSet set;
  loadSeeds(r.opt.ctrl_images, checker, set);
  loadSeeds(r.opt.test_images, checker, set);
  if (!checker.ready() || set.seeds.empty()) {
    (void)std::fprintf(r.log, "FATAL: checker not ready or no seed images found\n");
    r.findings++;
    return 1;
  }
  const std::string path = r.opt.work_dir + "/imagefile.bin";
  const std::string dir_path = r.opt.work_dir;
  uint64_t passed = 0;
  const uint64_t lo = r.opt.has_only ? r.opt.only : 0U;
  const uint64_t hi = r.opt.has_only ? r.opt.only + 1U : r.opt.count;
  for (uint64_t i = lo; i < hi; i++) {
    Rng rng = rngFor(r.opt.seed, i);
    checkKindWord(r, i, rng);
    std::vector<uint8_t> img;
    std::string use_path = path;
    const uint32_t special = below(rng, 100U);
    if (special == 0U) {
      use_path = r.opt.work_dir + "/no-such-file.img";
    } else if (special == 1U) {
      use_path = dir_path;
    } else if (special == 2U) {
      img.clear();
    } else if (special == 3U) {
      img = randomBytes(rng, kImageMax + 1U + below(rng, 64U));
    } else {
      img = makeImage(rng, set);
    }
    const bool wrote = (use_path != path) || writeFile(path, img.data(), img.size());
    const uint8_t kind = chance(rng, 85U) ? static_cast<uint8_t>(1U + below(rng, 3U))
                                          : static_cast<uint8_t>(nextU64(rng));
    const ImageLimits limits = makeLimits(rng);
    constexpr size_t kGuard = 16;
    std::array<char, 512 + kGuard> buf{};
    const size_t line_bytes = chance(rng, 70U) ? 320U + below(rng, 190U) : below(rng, 330U);
    std::memset(buf.data(), 0x5C, buf.size());
    inputBegin(r, i, img.data(), img.size());
    const bool got = checkImageFile(use_path.c_str(), kind, checker, limits, "fuzz limits",
      buf.data(), line_bytes);
    inputEnd(r, i);
    bool guard_ok = true;
    for (size_t k = line_bytes; k < buf.size(); k++) {
      guard_ok = guard_ok && buf[k] == 0x5C;
    }
    const size_t len = (line_bytes > 0U) ? strnlen(buf.data(), line_bytes) : 0U;
    if (!guard_ok || (line_bytes > 0U && len == line_bytes)) {
      finding(r, "checkImageFile wrote past the line buffer or did not end the line", i,
        img.data(), img.size(), "canary after line_bytes changed, or no NUL inside the buffer");
    }
    if (!wrote) {
      continue;
    }
    if (got) {
      passed++;
      const ImageVerdict v = checker.check(img.data(), img.size(), limits);
      const size_t body = (img.size() > kOverhead) ? img.size() - kOverhead : 0U;
      size_t region = kImageMax;
      size_t written = img.size();
      if (kind == 1U) {
        region = kBootloaderRegion;
        written = body;
      } else if (kind == 3U) {
        region = kLastResortRegion;
      }
      if (use_path != path || v.code != 0U || v.kind != kind || written > region) {
        finding(r, "checkImageFile accepted a file the rules refuse", i, img.data(), img.size(),
          "not a regular image file, checker refuses, wrong kind, or too big for its region");
      } else {
        judge(r, i, img, limits, set, v);
      }
      if (line_bytes >= 24U && std::strncmp(buf.data(), "image check passed", 18) != 0) {
        finding(r, "checkImageFile returns true with a line that does not say passed", i,
          img.data(), img.size(), buf.data());
      }
    } else if (line_bytes >= 24U && std::strncmp(buf.data(), "image check FAILED", 18) != 0) {
      finding(r, "checkImageFile returns false with a line that does not say FAILED", i,
        img.data(), img.size(), buf.data());
    }
    progress(r, i);
  }
  (void)std::fprintf(r.log, "imagefile: %llu files passed the file check\n",
    static_cast<unsigned long long>(passed));
  return static_cast<int>(r.findings);
}

}  // namespace fuzz
