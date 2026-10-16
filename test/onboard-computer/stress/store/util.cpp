/*
 * util.cpp — the helpers declared in util.hpp.
 */
#include "util.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <utility>
#include <ctime>
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>

namespace util
{

namespace
{

// CRC-32 polynomial, reflected: 0x04C11DB7 bit-reversed. Interfaces Sec. 4.0, crc32.hpp:2-4.
constexpr uint32_t kCrcPolyReflected = 0xEDB88320U;
constexpr uint32_t kCrcStart = 0xFFFFFFFFU;   // crc32.hpp:3
constexpr unsigned kBitsPerByte = 8U;
// splitmix64 constants: Steele, Lea, Flood, "Fast splittable pseudorandom number generators",
// OOPSLA 2014, Fig. 15 (the published generator; any fixed generator would do).
constexpr uint64_t kSplitStep = 0x9E3779B97F4A7C15ULL;
constexpr uint64_t kSplitMul1 = 0xBF58476D1CE4E5B9ULL;
constexpr uint64_t kSplitMul2 = 0x94D049BB133111EBULL;
constexpr unsigned kSplitShift1 = 30U;
constexpr unsigned kSplitShift2 = 27U;
constexpr unsigned kSplitShift3 = 31U;
// Bytes copied per read when a file is read whole.
constexpr size_t kChunk = 65536;
// Size of the text for a time stamp: "2026-10-16 10:44:00 EEST" plus zero, with room.
constexpr size_t kTimeText = 64;

}  // namespace

/**
 * @brief Reads a whole file.
 *
 * @param[in]     path  the file
 * @param[out]    out   its bytes
 * @return true: read; false: could not be opened or read
 */
bool readFile(const std::string & path, Bytes & out)
{
  out.clear();
  FILE * f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    return false;
  }
  std::array<uint8_t, kChunk> buf{};
  size_t n = 0;
  do {
    n = std::fread(buf.data(), 1, buf.size(), f);
    out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
  } while (n == buf.size());
  bool ok = std::ferror(f) == 0;
  (void)std::fclose(f);
  return ok;
}

/**
 * @brief Writes a file whole (created or replaced), without flushing to the medium.
 *
 * @param[in]     path  the file
 * @param[in]     data  bytes
 * @param[in]     size  number of bytes
 * @return true: written; false: not
 */
bool writeFile(const std::string & path, const uint8_t * data, size_t size)
{
  FILE * f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    return false;
  }
  bool ok = size == 0U || std::fwrite(data, 1, size, f) == size;
  return std::fclose(f) == 0 && ok;
}

/**
 * @brief Makes an empty directory (any earlier one with that path is removed).
 *
 * @param[in]     path  the directory
 * @return true: made; false: not
 */
bool resetDir(const std::string & path)
{
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  ec.clear();
  return std::filesystem::create_directories(path, ec) && !ec;
}

/**
 * @brief Copies a directory's files (no sub-directories) into a new empty directory.
 *
 * @param[in]     from  source directory
 * @param[in]     to    destination; emptied first
 * @return true: copied; false: not
 */
bool copyDir(const std::string & from, const std::string & to)
{
  if (!resetDir(to)) {
    return false;
  }
  std::error_code ec;
  for (const auto & entry : std::filesystem::directory_iterator(from, ec)) {
    std::filesystem::copy_file(entry.path(), std::filesystem::path(to) / entry.path().filename(),
      std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      return false;
    }
  }
  return !ec;
}

/**
 * @brief Next 64-bit value of a seeded generator (splitmix64).
 *
 * @param[in,out] state  generator state; start it with the seed
 * @return the next value
 */
uint64_t splitmix(uint64_t & state)
{
  state += kSplitStep;
  uint64_t z = state;
  z = (z ^ (z >> kSplitShift1)) * kSplitMul1;
  z = (z ^ (z >> kSplitShift2)) * kSplitMul2;
  return z ^ (z >> kSplitShift3);
}

/**
 * @brief Reads a little-endian 32-bit number.
 *
 * @param[in]     p  4 bytes
 * @return the number
 */
uint32_t readLe32(const uint8_t * p)
{
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) |
         (static_cast<uint32_t>(p[2]) << 16U) | (static_cast<uint32_t>(p[3]) << 24U);
}

/**
 * @brief Writes a little-endian 32-bit number.
 *
 * @param[out]    p  4 bytes
 * @param[in]     v  the number
 * @return nothing
 */
void writeLe32(uint8_t * p, uint32_t v)
{
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8U);
  p[2] = static_cast<uint8_t>(v >> 16U);
  p[3] = static_cast<uint8_t>(v >> 24U);
}

/**
 * @brief The CRC-32 of Interfaces Sec. 4.0, written bit by bit (the oracle's own copy).
 *
 * @param[in]     data  bytes
 * @param[in]     size  number of bytes
 * @return the CRC-32
 */
uint32_t crc32(const uint8_t * data, size_t size)
{
  uint32_t crc = kCrcStart;
  for (size_t i = 0; i < size; i++) {
    crc ^= data[i];
    for (unsigned b = 0; b < kBitsPerByte; b++) {
      crc = ((crc & 1U) != 0U) ? ((crc >> 1U) ^ kCrcPolyReflected) : (crc >> 1U);
    }
  }
  return ~crc;
}

/**
 * @brief Builds one record: tag, body length, body, CRC-32 of the three.
 *
 * @param[in]     tag   kTagReport or kTagState
 * @param[in]     body  the body, at most 255 bytes
 * @return the record's bytes
 */
Bytes encodeRecord(uint8_t tag, const Bytes & body)
{
  Bytes r;
  r.push_back(tag);
  r.push_back(static_cast<uint8_t>(body.size()));
  r.insert(r.end(), body.begin(), body.end());
  std::array<uint8_t, kCrcBytes> crc{};
  writeLe32(crc.data(), crc32(r.data(), r.size()));
  r.insert(r.end(), crc.begin(), crc.end());
  return r;
}

/**
 * @brief Builds a state body (report_store.hpp: six numbers of 4 bytes, little-endian).
 *
 * @param[in]     next     next report number
 * @param[in]     deleted  deleted below
 * @param[in]     through  HEALTH dropped through
 * @param[in]     count    dropped count
 * @param[in]     first    onboard time of the first dropped, s
 * @param[in]     last     onboard time of the last dropped, s
 * @return the 24 bytes
 */
Bytes stateBody(uint32_t next, uint32_t deleted, uint32_t through, uint32_t count, uint32_t first,
  uint32_t last)
{
  Bytes b(kStateBytes, 0);
  writeLe32(&b[kStateNextAt], next);
  writeLe32(&b[kStateDeletedAt], deleted);
  writeLe32(&b[kStateThroughAt], through);
  writeLe32(&b[kStateCountAt], count);
  writeLe32(&b[kStateFirstAt], first);
  writeLe32(&b[kStateLastAt], last);
  return b;
}

/**
 * @brief Tells whether a report body has the length and kind of Sec. 5.4.
 *
 * @param[in]     body  the body
 * @return true: a HEALTH of 24 bytes or an EVENT of 15 to 47 bytes
 */
static bool reportFits(const Bytes & body)
{
  if (body.size() < kEventMinLen) {
    return false;
  }
  uint8_t kind = body[kKindAt];
  return (kind == kKindHealth && body.size() == kHealthLen) ||
         (kind == kKindEvent && body.size() <= kEventMaxLen);
}

/**
 * @brief Reads the records of a byte string in order, up to the first that is not good.
 *
 * A record is good when its length fits its tag, all its bytes are there, its CRC-32 matches, and
 * (for a report) its kind and length fit Sec. 5.4 (report_store.hpp "Power cuts").
 *
 * @param[in]     data  the file's bytes
 * @param[in]     size  number of bytes
 * @return the good records and where they end
 */
Parsed parse(const uint8_t * data, size_t size)
{
  Parsed p{};
  size_t at = 0;
  while (at + kHeadBytes <= size) {
    uint8_t tag = data[at];
    size_t n = data[at + 1U];
    bool len_ok = (tag == kTagReport && n >= kEventMinLen && n <= kEventMaxLen) ||
                  (tag == kTagState && n == kStateBytes);
    if (!len_ok || at + kOverhead + n > size) {
      break;
    }
    if (readLe32(&data[at + kHeadBytes + n]) != crc32(&data[at], kHeadBytes + n)) {
      break;
    }
    Rec r{};
    r.offset = at;
    r.size = kOverhead + n;
    r.tag = tag;
    r.body.assign(&data[at + kHeadBytes], &data[at + kHeadBytes + n]);
    if (tag == kTagReport && !reportFits(r.body)) {
      break;
    }
    at += r.size;
    p.recs.push_back(r);
  }
  p.good_end = at;
  return p;
}

/**
 * @brief Works out the state in force and the live reports of parsed records.
 *
 * @param[in]     parsed  the good records
 * @return the view
 */
View evaluate(const Parsed & parsed)
{
  View v{};
  for (const Rec & r : parsed.recs) {
    if (r.tag == kTagState) {
      v.have_state = true;
      v.state_next = readLe32(&r.body[kStateNextAt]);
      v.deleted_below = readLe32(&r.body[kStateDeletedAt]);
      v.through = readLe32(&r.body[kStateThroughAt]);
      v.count = readLe32(&r.body[kStateCountAt]);
      v.first_s = readLe32(&r.body[kStateFirstAt]);
      v.last_s = readLe32(&r.body[kStateLastAt]);
    } else {
      uint32_t number = readLe32(&r.body[kNumberAt]);
      v.highest = (number > v.highest) ? number : v.highest;
    }
  }
  for (const Rec & r : parsed.recs) {
    if (r.tag != kTagReport) {
      continue;
    }
    Rep rep{readLe32(&r.body[kNumberAt]), r.body[kKindAt], r.body};
    bool live = rep.number >= v.deleted_below &&
                (rep.kind != kKindHealth || rep.number > v.through);
    if (!live) {
      continue;
    }
    v.live_bytes += r.size;
    (rep.kind == kKindEvent ? v.events : v.health).push_back(rep);
  }
  return v;
}

/**
 * @brief The Mac's local time as text, with the zone name.
 *
 * @return for example "2026-10-16 10:44:00 EEST"
 */
std::string nowText()
{
  std::array<char, kTimeText> text{};
  time_t t = std::time(nullptr);
  struct tm local{};
  (void)localtime_r(&t, &local);
  (void)std::strftime(text.data(), text.size(), "%F %T %Z", &local);
  return std::string(text.data());
}

/**
 * @brief Runs job(context, 0) to job(context, count - 1), each in its own process, several at once.
 *
 * Why processes: a crash or a sanitizer stop in one case must not end the run, and a case that
 * calls _exit must not end the parent. The status of case i is the raw wait status.
 *
 * @param[in]     count    number of cases
 * @param[in]     workers  most processes running at once, at least 1
 * @param[in]     job      the case; its return value is the child's exit code
 * @param[in]     context  passed to job
 * @param[out]    status   wait status of each case; size set to count
 * @return nothing
 */
void runForked(size_t count, size_t workers, Job job, const void * context,
  std::vector<int> & status)
{
  status.assign(count, -1);
  std::vector<std::pair<pid_t, size_t>> running;
  size_t next = 0;
  while (next < count || !running.empty()) {
    while (next < count && running.size() < workers) {
      pid_t pid = fork();
      if (pid == 0) {
        _exit(job(context, next));
      }
      if (pid > 0) {
        running.emplace_back(pid, next);
      }
      next++;
    }
    int st = 0;
    pid_t done = wait(&st);
    for (size_t i = 0; i < running.size(); i++) {
      if (running[i].first == done) {
        status[running[i].second] = st;
        running.erase(running.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
    }
  }
}

}  // namespace util
