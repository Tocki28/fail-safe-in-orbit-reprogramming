/*
 * fuzz_common.cpp — the shared parts of the fuzz targets (fuzz_common.hpp).
 */
#include "fuzz_common.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace fuzz
{

namespace
{

// The longest an input may run before the watchdog calls it a hang. Bench value: the slowest
// legitimate input here is a store rewrite or an image check, a few ms; 10 s is far above that.
constexpr unsigned kHangSeconds = 10;
// Saved files per run. Bench value: keeps a run with a systematic finding from filling the disk.
constexpr uint64_t kSavedMax = 40;
// A progress line every this many inputs.
constexpr uint64_t kProgressEvery = 100000;
// The stderr file is emptied every this many inputs.
constexpr uint64_t kTrimEvery = 256;
// Bytes in the largest input the watchdog can save: an image (40 KB) or a store file.
constexpr size_t kSaveMaxBytes = 1U << 20;
// Special byte values a mutation writes: all zero, all one, the largest signed, the sign bit.
constexpr uint8_t kSpecialBytes[4] = {0x00U, 0xFFU, 0x7FU, 0x80U};
// Reflected CRC-32 polynomial of Interfaces Sec. 4.0 (0x04C11DB7 bit-reversed).
constexpr uint32_t kCrcPolynomialReflected = 0xEDB88320U;
// The multiplier and the two shifts/xors of splitmix64 [Steele, Lea, Flood, OOPSLA 2014, Fig. 5].
constexpr uint64_t kSplitGamma = 0x9E3779B97F4A7C15ULL;
constexpr uint64_t kSplitMul1 = 0xBF58476D1CE4E5B9ULL;
constexpr uint64_t kSplitMul2 = 0x94D049BB133111EBULL;

// The watchdog's view of the input now running; read by the signal handlers.
const uint8_t * volatile g_data = nullptr;
volatile size_t g_size = 0;
volatile uint64_t g_index = 0;
char g_target[32] = "";
char g_out_dir[512] = "";
uint64_t g_seed = 0;
double g_start_ms = 0.0;
int g_log_fd = -1;
uint64_t g_saved = 0;

/**
 * @brief Saves the running input to a file; used by the signal handlers and by finding().
 *
 * @param[in]     what   word in the file name: crash, hang or finding
 * @param[in]     index  input index
 * @param[in]     data   the bytes
 * @param[in]     size   number of bytes
 * @return nothing
 */
void saveFile(const char * what, uint64_t index, const uint8_t * data, size_t size)
{
  char path[1024];
  (void)std::snprintf(path, sizeof path, "%s/%s-%s-seed%llu-idx%llu.bin", g_out_dir, g_target,
    what, static_cast<unsigned long long>(g_seed), static_cast<unsigned long long>(index));
  int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return;
  }
  size_t done = 0;
  size_t bound = std::min(size, kSaveMaxBytes);
  for (size_t i = 0; i < 4096U && done < bound; i++) {   // bounded: B2
    ssize_t n = ::write(fd, data + done, bound - done);
    if (n <= 0) {
      break;
    }
    done += static_cast<size_t>(n);
  }
  (void)::close(fd);
}

/**
 * @brief Signal handler for a crash or an undefined-behaviour abort: saves the input, dies.
 *
 * snprintf is not formally async-signal-safe; it works on this Mac and the process is about to
 * end anyway.
 *
 * @param[in]     sig  the signal number
 * @return nothing
 */
void onCrash(int sig)
{
  char line[256];
  int n = std::snprintf(line, sizeof line, "CRASH signal %d target %s seed %llu index %llu\n", sig,
    g_target, static_cast<unsigned long long>(g_seed), static_cast<unsigned long long>(g_index));
  if (n > 0 && g_log_fd >= 0) {
    (void)::write(g_log_fd, line, static_cast<size_t>(n));
  }
  saveFile("crash", g_index, g_data, g_size);
  (void)std::signal(sig, SIG_DFL);
  (void)::raise(sig);
}

/**
 * @brief Signal handler for the watchdog's alarm: saves the input and ends the run.
 *
 * @param[in]     sig  the signal number (SIGALRM)
 * @return nothing
 */
void onHang(int sig)
{
  (void)sig;
  char line[256];
  int n = std::snprintf(line, sizeof line, "HANG over %u s target %s seed %llu index %llu\n",
    kHangSeconds, g_target, static_cast<unsigned long long>(g_seed),
    static_cast<unsigned long long>(g_index));
  if (n > 0 && g_log_fd >= 0) {
    (void)::write(g_log_fd, line, static_cast<size_t>(n));
  }
  saveFile("hang", g_index, g_data, g_size);
  ::_exit(124);   // 124: the exit code of timeout(1) for a time-out
}

}  // namespace

/**
 * @brief Next 64-bit value of the generator (splitmix64).
 *
 * @param[in,out] rng  the generator
 * @return a 64-bit pseudo-random value
 */
uint64_t nextU64(Rng & rng)
{
  rng.state += kSplitGamma;
  uint64_t z = rng.state;
  z = (z ^ (z >> 30)) * kSplitMul1;
  z = (z ^ (z >> 27)) * kSplitMul2;
  return z ^ (z >> 31);
}

/**
 * @brief A value in [0, bound).
 *
 * The modulo bias is below 2^-32 for the bounds used here and does not matter for fuzzing.
 *
 * @param[in,out] rng    the generator
 * @param[in]     bound  exclusive upper limit; 0 gives 0
 * @return a pseudo-random value below bound
 */
uint32_t below(Rng & rng, uint32_t bound)
{
  if (bound == 0U) {
    return 0U;
  }
  return static_cast<uint32_t>(nextU64(rng) % bound);
}

/**
 * @brief True with the given probability.
 *
 * @param[in,out] rng      the generator
 * @param[in]     percent  0 to 100
 * @return true with probability percent/100
 */
bool chance(Rng & rng, uint32_t percent)
{
  return below(rng, 100U) < percent;
}

/**
 * @brief The generator of one input: a function of the seed and the index only.
 *
 * @param[in]     seed   run seed
 * @param[in]     index  input index
 * @return a generator whose first values depend on both
 */
Rng rngFor(uint64_t seed, uint64_t index)
{
  Rng rng{seed ^ (index * kSplitGamma)};
  (void)nextU64(rng);   // mix the first state once
  return rng;
}

/**
 * @brief count random bytes.
 *
 * @param[in,out] rng    the generator
 * @param[in]     count  number of bytes
 * @return the bytes
 */
std::vector<uint8_t> randomBytes(Rng & rng, size_t count)
{
  std::vector<uint8_t> v(count);
  for (size_t i = 0; i < count; i++) {
    v[i] = static_cast<uint8_t>(nextU64(rng));
  }
  return v;
}

/**
 * @brief Reads a little-endian 32-bit number.
 *
 * @param[in]     b  4 bytes
 * @return the number
 */
uint32_t readLe32(const uint8_t * b)
{
  return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

/**
 * @brief Reads a little-endian 16-bit number.
 *
 * @param[in]     b  2 bytes
 * @return the number
 */
uint16_t readLe16(const uint8_t * b)
{
  return static_cast<uint16_t>(static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8));
}

/**
 * @brief Writes a little-endian 32-bit number.
 *
 * @param[out]    b      4 bytes
 * @param[in]     value  the number
 * @return nothing
 */
void writeLe32(uint8_t * b, uint32_t value)
{
  b[0] = static_cast<uint8_t>(value);
  b[1] = static_cast<uint8_t>(value >> 8);
  b[2] = static_cast<uint8_t>(value >> 16);
  b[3] = static_cast<uint8_t>(value >> 24);
}

/**
 * @brief Writes a little-endian 16-bit number.
 *
 * @param[out]    b      2 bytes
 * @param[in]     value  the number
 * @return nothing
 */
void writeLe16(uint8_t * b, uint16_t value)
{
  b[0] = static_cast<uint8_t>(value);
  b[1] = static_cast<uint8_t>(value >> 8);
}

/**
 * @brief CRC-32 of Interfaces Sec. 4.0, bit by bit.
 *
 * Polynomial 0x04C11DB7, start 0xFFFFFFFF, least significant bit first, result inverted
 * [src/controller/common/crc32.h]. Written again here, without a table, so that the oracle shares
 * no code with the updater's crc32.cpp or the controller's crc32.c.
 *
 * @param[in]     data  the bytes
 * @param[in]     size  number of bytes
 * @return the CRC-32
 */
uint32_t refCrc32(const uint8_t * data, size_t size)
{
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < size; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc & 1U) != 0U ? (crc >> 1) ^ kCrcPolynomialReflected : crc >> 1;
    }
  }
  return ~crc;
}

/**
 * @brief A boundary value for a field of the given width.
 *
 * Zero, one, the largest signed, the sign bit, all ones, and the neighbours of each: the values
 * where a length or an offset check is most likely to be off by one.
 *
 * @param[in,out] rng    the generator
 * @param[in]     width  field width in bytes: 1, 2, 4 or 8
 * @return the value, cut to the width
 */
uint64_t boundaryValue(Rng & rng, size_t width)
{
  const unsigned bits = static_cast<unsigned>(width) * 8U;
  const uint64_t top = (bits >= 64U) ? ~0ULL : ((1ULL << bits) - 1ULL);
  const uint64_t sign = 1ULL << (bits - 1U);
  const uint64_t table[8] = {0ULL, 1ULL, sign - 1ULL, sign, sign + 1ULL, top - 1ULL, top, 2ULL};
  uint64_t v = table[below(rng, 8U)];
  const uint32_t jitter = below(rng, 4U);
  if (jitter == 1U) {
    v = v + 1ULL;
  } else if (jitter == 2U) {
    v = v - 1ULL;
  }
  return v & top;
}

/**
 * @brief Mutates a byte vector in place.
 *
 * One to four operations: flip a bit, set a byte to 0x00/0xFF/0x7F/0x80, write a boundary value
 * at a known field, truncate, extend with random bytes, delete a range, duplicate a range, and
 * splice in a piece of another seed. The size never exceeds max_size.
 *
 * @param[in,out] rng       the generator
 * @param[in,out] v         the bytes to change
 * @param[in]     fields    fields to aim boundary values at (may be empty)
 * @param[in]     max_size  the largest size the result may have
 * @param[in]     other     another seed to splice from; may be null
 * @return nothing
 */
void mutate(Rng & rng, std::vector<uint8_t> & v, const std::vector<Field> & fields,
  size_t max_size, const std::vector<uint8_t> * other)
{
  const uint32_t rounds = 1U + below(rng, 4U);
  for (uint32_t round = 0; round < rounds; round++) {
    const uint32_t op = below(rng, 9U);
    const size_t size = v.size();
    if (op == 0U && size > 0U) {
      v[below(rng, static_cast<uint32_t>(size))] ^= static_cast<uint8_t>(1U << below(rng, 8U));
    } else if (op == 1U && size > 0U) {
      v[below(rng, static_cast<uint32_t>(size))] = kSpecialBytes[below(rng, 4U)];
    } else if (op == 2U && !fields.empty()) {
      const Field & f = fields[below(rng, static_cast<uint32_t>(fields.size()))];
      uint64_t value = boundaryValue(rng, f.width);
      for (size_t k = 0; k < f.width && f.offset + k < size; k++) {
        v[f.offset + k] = static_cast<uint8_t>(value >> (8U * k));
      }
    } else if (op == 3U && size > 0U) {
      v.resize(below(rng, static_cast<uint32_t>(size)));
    } else if (op == 4U) {
      const size_t more = 1U + below(rng, 64U);
      if (size + more <= max_size) {
        std::vector<uint8_t> extra = randomBytes(rng, more);
        v.insert(v.end(), extra.begin(), extra.end());
      }
    } else if (op == 5U && size > 1U) {
      const size_t at = below(rng, static_cast<uint32_t>(size));
      const size_t len = 1U + below(rng, static_cast<uint32_t>(std::min<size_t>(size - at, 32U)));
      v.erase(v.begin() + static_cast<long>(at), v.begin() + static_cast<long>(at + len));
    } else if (op == 6U && size > 1U) {
      const size_t at = below(rng, static_cast<uint32_t>(size));
      const size_t len = 1U + below(rng, static_cast<uint32_t>(std::min<size_t>(size - at, 32U)));
      if (size + len <= max_size) {
        std::vector<uint8_t> piece(v.begin() + static_cast<long>(at),
          v.begin() + static_cast<long>(at + len));
        v.insert(v.begin() + static_cast<long>(at), piece.begin(), piece.end());
      }
    } else if (op == 7U && other != nullptr && !other->empty()) {
      const size_t from = below(rng, static_cast<uint32_t>(other->size()));
      const size_t len = 1U + below(rng, static_cast<uint32_t>(other->size() - from));
      const size_t at = (size == 0U) ? 0U : below(rng, static_cast<uint32_t>(size));
      const size_t cut = std::min<size_t>(len, (size > at) ? size - at : 0U);
      if (size - cut + len <= max_size) {
        v.erase(v.begin() + static_cast<long>(at), v.begin() + static_cast<long>(at + cut));
        v.insert(v.begin() + static_cast<long>(at), other->begin() + static_cast<long>(from),
          other->begin() + static_cast<long>(from + len));
      }
    } else if (size > 0U) {
      v[below(rng, static_cast<uint32_t>(size))] = static_cast<uint8_t>(nextU64(rng));
    }
  }
  if (v.size() > max_size) {
    v.resize(max_size);
  }
}

/**
 * @brief Reads the command line.
 *
 * Usage: fuzz TARGET --seed S --n N --out DIR --work DIR --ctrl-images DIR --test-images DIR
 * [--only INDEX]
 *
 * @param[in]     argc  argument count
 * @param[in]     argv  arguments
 * @param[out]    opt   the options
 * @return true: all required options present; false: usage error
 */
bool parseOptions(int argc, char ** argv, Options & opt)
{
  opt = Options{};
  opt.seed = 1U;
  opt.count = 1000000U;
  if (argc < 2) {
    return false;
  }
  opt.target = argv[1];
  for (int i = 2; i + 1 < argc; i += 2) {
    const std::string key = argv[i];
    const char * val = argv[i + 1];
    if (key == "--seed") {
      opt.seed = std::strtoull(val, nullptr, 0);
    } else if (key == "--n") {
      opt.count = std::strtoull(val, nullptr, 0);
    } else if (key == "--only") {
      opt.has_only = true;
      opt.only = std::strtoull(val, nullptr, 0);
    } else if (key == "--out") {
      opt.out_dir = val;
    } else if (key == "--work") {
      opt.work_dir = val;
    } else if (key == "--ctrl-images") {
      opt.ctrl_images = val;
    } else if (key == "--test-images") {
      opt.test_images = val;
    } else {
      return false;
    }
  }
  return !opt.out_dir.empty() && !opt.work_dir.empty();
}

/**
 * @brief Opens the log and starts the watchdog and the crash handlers.
 *
 * The first log line is the start time, the commit and the exact command, as the task asks. The
 * commit is passed in the environment variable FUZZ_COMMIT and the start time in FUZZ_START by
 * the Makefile, which also gives the command line in command_line.
 *
 * @param[out]    r             the report to fill
 * @param[in]     opt           the options
 * @param[in]     command_line  the exact command, for the first log line
 * @return true: the log is open; false: it could not be opened
 */
bool reportOpen(Report & r, const Options & opt, const char * command_line)
{
  r = Report{};
  r.opt = opt;
  std::string path = opt.out_dir + "/" + opt.target + ".log";
  r.log = std::fopen(path.c_str(), "w");
  if (r.log == nullptr) {
    return false;
  }
  (void)std::setvbuf(r.log, nullptr, _IOLBF, 0);
  g_log_fd = fileno(r.log);
  (void)std::snprintf(g_target, sizeof g_target, "%s", opt.target.c_str());
  (void)std::snprintf(g_out_dir, sizeof g_out_dir, "%s", opt.out_dir.c_str());
  g_seed = opt.seed;
  const char * start = std::getenv("FUZZ_START");
  const char * commit = std::getenv("FUZZ_COMMIT");
  (void)std::fprintf(r.log, "%s EEST, commit %s, %s\n", (start != nullptr) ? start : "?",
    (commit != nullptr) ? commit : "?", command_line);
  const char * load = std::getenv("FUZZ_UPTIME");
  (void)std::fprintf(r.log, "uptime at start: %s\n", (load != nullptr) ? load : "?");
  (void)std::fprintf(r.log, "mutation-based fuzzer without coverage feedback (no libFuzzer, AFL "
    "or honggfuzz here); seed %llu; %llu inputs; UBSan on, no recovery\n",
    static_cast<unsigned long long>(opt.seed), static_cast<unsigned long long>(opt.count));
  const int crash_signals[6] = {SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGTRAP, SIGFPE};
  for (int sig : crash_signals) {
    (void)std::signal(sig, onCrash);
  }
  (void)std::signal(SIGALRM, onHang);
  return true;
}

/**
 * @brief Writes the summary and closes the log.
 *
 * @param[in,out] r  the report
 * @return nothing
 */
void reportClose(Report & r)
{
  (void)std::fprintf(r.log, "SUMMARY target %s: %llu inputs, %llu findings, %llu notes, "
    "%.1f s in the code under test\n", r.opt.target.c_str(),
    static_cast<unsigned long long>(r.inputs), static_cast<unsigned long long>(r.findings),
    static_cast<unsigned long long>(r.notes), r.total_ms / 1000.0);
  (void)std::fprintf(r.log, "slowest inputs (ms, index):");
  for (int i = 0; i < 10; i++) {
    (void)std::fprintf(r.log, " %.3f@%llu", r.slow_ms[i],
      static_cast<unsigned long long>(r.slow_index[i]));
  }
  (void)std::fprintf(r.log, "\n");
  (void)std::fclose(r.log);
  r.log = nullptr;
  g_log_fd = -1;
}

/**
 * @brief Milliseconds on the monotonic clock.
 *
 * @return milliseconds since an arbitrary start
 */
double nowMs()
{
  struct timespec ts{};
  (void)::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1.0e6;
}

/**
 * @brief Arms the watchdog on an input and starts its timer.
 *
 * @param[in,out] r      the report
 * @param[in]     index  input index
 * @param[in]     data   the input's bytes (kept for the signal handlers)
 * @param[in]     size   number of bytes
 * @return nothing
 */
void inputBegin(Report & r, uint64_t index, const uint8_t * data, size_t size)
{
  (void)r;
  g_data = data;
  g_size = size;
  g_index = index;
  (void)::alarm(kHangSeconds);
  g_start_ms = nowMs();
}

/**
 * @brief Disarms the watchdog, adds the input's time to the totals and the slowest list.
 *
 * @param[in,out] r      the report
 * @param[in]     index  input index
 * @return nothing
 */
void inputEnd(Report & r, uint64_t index)
{
  const double ms = nowMs() - g_start_ms;
  (void)::alarm(0);
  r.inputs++;
  r.total_ms += ms;
  int worst = 0;
  for (int i = 1; i < 10; i++) {
    if (r.slow_ms[i] < r.slow_ms[worst]) {
      worst = i;
    }
  }
  if (ms > r.slow_ms[worst]) {
    r.slow_ms[worst] = ms;
    r.slow_index[worst] = index;
  }
  if (r.inputs % kTrimEvery == 0U) {
    trimStderr();
  }
}

/**
 * @brief Records a finding: log line, counter and a saved input.
 *
 * @param[in,out] r      the report
 * @param[in]     title  one-line name of the finding
 * @param[in]     index  input index that reproduces it (with the seed)
 * @param[in]     data   the input (may be null with size 0)
 * @param[in]     size   number of bytes
 * @param[in]     detail what was expected and what happened
 * @return nothing
 */
void finding(Report & r, const char * title, uint64_t index, const uint8_t * data, size_t size,
  const char * detail)
{
  r.findings++;
  (void)std::fprintf(r.log, "FINDING %llu: %s (seed %llu, index %llu, %zu bytes): %s\n",
    static_cast<unsigned long long>(r.findings), title,
    static_cast<unsigned long long>(r.opt.seed), static_cast<unsigned long long>(index), size,
    detail);
  if (g_saved < kSavedMax && data != nullptr) {
    g_saved++;
    saveFile("finding", index, data, size);
  }
}

/**
 * @brief Records an observation that is not a failure.
 *
 * @param[in,out] r     the report
 * @param[in]     text  the observation
 * @return nothing
 */
void note(Report & r, const char * text)
{
  r.notes++;
  (void)std::fprintf(r.log, "NOTE: %s\n", text);
}

/**
 * @brief Writes a progress line now and then.
 *
 * @param[in,out] r      the report
 * @param[in]     index  the input just finished
 * @return nothing
 */
void progress(Report & r, uint64_t index)
{
  if ((index + 1U) % kProgressEvery == 0U) {
    (void)std::fprintf(r.log, "progress: %llu inputs, %llu findings, %.1f s under test\n",
      static_cast<unsigned long long>(index + 1U), static_cast<unsigned long long>(r.findings),
      r.total_ms / 1000.0);
  }
}

/**
 * @brief Tells whether input index is to run.
 *
 * @param[in]     r      the report (holds the --only option)
 * @param[in]     index  input index
 * @return true: run it; false: skip it
 */
bool wanted(const Report & r, uint64_t index)
{
  return !r.opt.has_only || r.opt.only == index;
}

/**
 * @brief Empties the standard error file (the updater's log lines go there).
 *
 * The Makefile points stderr at a file opened for appending, so after the cut the next line is
 * written at the new end. The last cut-off lines are lost; the tail up to the next cut stays, and
 * with it the sanitizer message of a crash.
 *
 * @return nothing
 */
void trimStderr()
{
  struct stat info{};
  if (::fstat(STDERR_FILENO, &info) == 0 && S_ISREG(info.st_mode)) {
    (void)::ftruncate(STDERR_FILENO, 0);
  }
}

}  // namespace fuzz
