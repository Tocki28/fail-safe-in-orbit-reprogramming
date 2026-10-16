/*
 * fuzz_records.cpp — fuzz targets 5 and 6 (fuzz_records.hpp).
 *
 * Oracles, written from the file layouts in durable_record.hpp and report_store.hpp (not from
 * the code under test), with an independent CRC-32:
 *   record file = record bytes then CRC-32 (4, little-endian); any other size is damage;
 *   store file  = records of  tag (1), n (1), body (n), CRC-32 (4) of tag..body;  tag 'R' with n 15
 *                 to 47 and a HEALTH (24) or EVENT report body, or tag 'S' with n 24; the file
 *                 is cut at the first record that fails.
 */
#include "fuzz_records.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

#include "clock_record.hpp"
#include "config.hpp"
#include "durable_record.hpp"
#include "limits_record.hpp"
#include "protocol.hpp"
#include "report_store.hpp"

namespace fuzz
{

namespace
{

// Store record framing (report_store.hpp): tag, length, CRC-32.
constexpr size_t kHead = 2;
constexpr size_t kCrc = 4;
constexpr uint8_t kTagR = 0x52U;
constexpr uint8_t kTagS = 0x53U;
constexpr size_t kStateLen = 24;
constexpr size_t kReportMin = 15;
constexpr size_t kReportMax = 47;
constexpr size_t kHealthLen = 24;
constexpr size_t kKindAt = 12;
// Record sizes the record fuzzer uses: the limits record (6), the clock offset (8), the smallest
// and the largest a DurableRecord keeps (1 and 64), and 0 and 65 which it must refuse.
constexpr size_t kRecordSizes[6] = {6, 8, 1, 64, 0, 65};
// Seed store files built through the public interface.
constexpr size_t kStoreSeeds = 48;
// Store capacity for most inputs: config::kReportStoreBytes.
constexpr size_t kBigStore = 1048576;
// Bound on the read-back loop: more than the reports a seed holds.
constexpr size_t kReadBackMax = 4096;

/**
 * @brief Makes a directory (and ignores "exists").
 *
 * @param[in]     path  directory name
 * @return nothing
 */
void makeDir(const std::string & path)
{
  (void)::mkdir(path.c_str(), 0755);
}

/**
 * @brief Writes a file, replacing it, without a flush (the fuzzer does not test power cuts).
 *
 * @param[in]     path  file name
 * @param[in]     v     the bytes
 * @return true: written
 */
bool putFile(const std::string & path, const std::vector<uint8_t> & v)
{
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    return false;
  }
  size_t done = 0;
  for (size_t i = 0; i < 4096U && done < v.size(); i++) {   // bounded
    ssize_t n = ::write(fd, v.data() + done, v.size() - done);
    if (n <= 0) {
      break;
    }
    done += static_cast<size_t>(n);
  }
  (void)::close(fd);
  return done == v.size();
}

/**
 * @brief Reads a whole file.
 *
 * @param[in]     path  file name
 * @param[out]    v     the bytes
 * @return true: read
 */
bool getFile(const std::string & path, std::vector<uint8_t> & v)
{
  v.clear();
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  std::array<uint8_t, 4096> buf{};
  for (size_t i = 0; i < 4096U; i++) {   // bounded: 16 MB
    ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n <= 0) {
      break;
    }
    v.insert(v.end(), buf.begin(), buf.begin() + n);
  }
  (void)::close(fd);
  return true;
}

/**
 * @brief The record file the layout asks for: bytes then their CRC-32.
 *
 * @param[in]     rec  the record's bytes
 * @return the file
 */
std::vector<uint8_t> recordFile(const std::vector<uint8_t> & rec)
{
  std::vector<uint8_t> f = rec;
  f.resize(rec.size() + 4U);
  writeLe32(&f[rec.size()], refCrc32(rec.data(), rec.size()));
  return f;
}

}  // namespace

/**
 * @brief Target 5: DurableRecord::read on fuzzed files, and the two loaders on top.
 *
 * Expected: RECORD_OK exactly when the file is record_bytes + 4 long and its CRC-32 matches, with
 * the bytes returned; RECORD_ABSENT when no file; RECORD_DAMAGED otherwise; RECORD_ERROR for a
 * handle that is not configured or a wrong buffer size. A record read back from a file that no
 * write produced and that was not given a recomputed CRC-32 is a finding. The write() output is
 * also compared with the layout, once at start. A valid record in "<name>.tmp" must not be read.
 *
 * @param[in,out] r  the report (options, log)
 * @return number of findings
 */
int runRecords(Report & r)
{
  const std::string dir = r.opt.work_dir + "/records";
  makeDir(dir);
  // Layout check of write(), through the public interface.
  for (size_t n : {size_t{1}, size_t{6}, size_t{8}, size_t{64}}) {
    Rng rng = rngFor(r.opt.seed, 1000000000ULL + n);
    DurableRecord w(dir.c_str(), "layout", n);
    std::vector<uint8_t> rec = randomBytes(rng, n);
    int err = 0;
    std::vector<uint8_t> on_disk;
    if (!w.write(rec.data(), rec.size(), err) || !getFile(dir + "/layout", on_disk) ||
        on_disk != recordFile(rec)) {
      finding(r, "DurableRecord::write file differs from the documented layout", n, rec.data(),
        rec.size(), "expected record bytes then CRC-32 little-endian, file size n + 4");
    }
  }
  uint64_t ok_count = 0;
  uint64_t forged_ok = 0;
  const uint64_t lo = r.opt.has_only ? r.opt.only : 0U;
  const uint64_t hi = r.opt.has_only ? r.opt.only + 1U : r.opt.count;
  for (uint64_t i = lo; i < hi; i++) {
    Rng rng = rngFor(r.opt.seed, i);
    const size_t n = kRecordSizes[chance(rng, 70U) ? below(rng, 2U) : below(rng, 6U)];
    const bool use_limits = (n == 6U) && chance(rng, 50U);
    const char * name = use_limits ? "limits" : (n == 8U ? "clock-offset" : "rec");
    const std::string path = dir + "/" + name;
    std::vector<uint8_t> rec = randomBytes(rng, n == 0U ? 6U : (n > 64U ? 64U : n));
    std::vector<uint8_t> file = recordFile(rec);
    const std::vector<uint8_t> seed_file = file;
    bool forged = false;
    bool absent = false;
    bool special = false;
    const uint32_t mode = below(rng, 100U);
    if (mode < 5U) {
      file = randomBytes(rng, below(rng, 90U));
    } else if (mode < 8U) {
      absent = true;
    } else if (mode < 10U) {
      special = true;
    } else if (mode >= 25U) {
      const std::vector<Field> fields = {{0, 1}, {0, 4}, {n, 4}, {n > 0U ? n - 1U : 0U, 1}};
      mutate(rng, file, fields, 80U, &seed_file);
      if (chance(rng, 40U) && file.size() > 4U) {
        writeLe32(&file[file.size() - 4U], refCrc32(file.data(), file.size() - 4U));
        forged = true;   // sizes: the CRC-32 is right for whatever bytes precede it
      }
    }
    (void)::unlink(path.c_str());
    (void)::rmdir(path.c_str());
    if (special) {
      (void)::mkdir(path.c_str(), 0755);
    } else if (!absent) {
      (void)putFile(path, file);
    }
    const bool tmp_trap = chance(rng, 3U);
    if (tmp_trap) {
      (void)putFile(path + ".tmp", recordFile(randomBytes(rng, n == 0U ? 6U
                                                          : (n > 64U ? 64U : n))));
    } else {
      (void)::unlink((path + ".tmp").c_str());
    }
    DurableRecord handle(dir.c_str(), name, n);
    std::array<uint8_t, 80> out{};
    out.fill(0xA5U);
    int error = 0;
    inputBegin(r, i, file.data(), file.size());
    const DurableRecord::Status got = handle.read(out.data(), (n == 0U) ? 1U : n, error);
    inputEnd(r, i);
    DurableRecord::Status want = DurableRecord::RECORD_DAMAGED;
    if (n == 0U || n > 64U) {
      want = DurableRecord::RECORD_ERROR;
    } else if (absent) {
      want = DurableRecord::RECORD_ABSENT;
    } else if (special) {
      want = DurableRecord::RECORD_ERROR;   // read() on a directory: read fails (EISDIR)
    } else if (file.size() == n + 4U && readLe32(&file[n]) == refCrc32(file.data(), n)) {
      want = DurableRecord::RECORD_OK;
    }
    const bool equal_status = (got == want) ||
      (special && (got == DurableRecord::RECORD_ERROR || got == DurableRecord::RECORD_DAMAGED));
    if (!equal_status) {
      finding(r, "DurableRecord::read status differs from the layout rule", i, file.data(),
        file.size(), (std::string("got ") + std::to_string(got) + " want " +
        std::to_string(want)).c_str());
    }
    if (got == DurableRecord::RECORD_OK) {
      ok_count++;
      if (std::memcmp(out.data(), file.data(), n) != 0) {
        finding(r, "RECORD_OK returned bytes that are not the file's record", i, file.data(),
          file.size(), "out differs from the first n bytes of the file");
      }
      if (file != seed_file) {
        if (forged || mode >= 25U) {
          forged_ok++;   // a CRC-32 made valid on purpose: integrity, not authenticity
        } else {
          finding(r, "a record was read back that no write produced", i, file.data(),
            file.size(), "random bytes passed the size and CRC-32 test");
        }
      }
    } else if (got != DurableRecord::RECORD_ERROR && out[0] != 0xA5U) {
      finding(r, "read() wrote the caller's buffer without returning RECORD_OK", i, file.data(),
        file.size(), "durable_record.hpp: out is written only when RECORD_OK is returned");
    }
    // The loaders on the same file.
    if (use_limits && n == 6U) {
      ImageLimits l{};
      int e = 0;
      const limits_record::Source src = limits_record::load(handle, l, e);
      const bool is_ok = (want == DurableRecord::RECORD_OK);
      const bool good = is_ok ? (src == limits_record::Source::kStored && l.usable &&
                                 l.minimum_version == readLe32(&file[0]) &&
                                 l.identity == readLe16(&file[4]))
                              : (src != limits_record::Source::kStored && !l.usable) ||
                                (src == limits_record::Source::kNoRecord && l.usable);
      if (!good && !special) {
        finding(r, "limits_record::load differs from the rule", i, file.data(), file.size(),
          "stored iff the record is good; damaged and unreadable are not usable");
      }
    } else if (n == 8U && std::strcmp(name, "clock-offset") == 0) {
      int64_t off = 12345;
      int e = 0;
      const clock_record::Source src = clock_record::load(handle, off, e);
      if (want == DurableRecord::RECORD_OK) {
        const uint64_t v = static_cast<uint64_t>(readLe32(&file[0])) |
                           (static_cast<uint64_t>(readLe32(&file[4])) << 32);
        if (src != clock_record::Source::kStored || off != static_cast<int64_t>(v)) {
          finding(r, "clock_record::load returned another offset than the record's", i,
            file.data(), file.size(), "offset is the little-endian signed 64-bit number");
        }
      } else if (src == clock_record::Source::kStored || off != 0) {
        finding(r, "clock_record::load accepted a record it should not", i, file.data(),
          file.size(), "damaged, absent or unreadable gives offset 0");
      }
    }
    // The same file read through handles of the wrong size: an error, never a value.
    if (n > 1U && n <= 64U && chance(rng, 5U)) {
      DurableRecord wrong(dir.c_str(), name, n - 1U);
      std::array<uint8_t, 80> sink{};
      int e = 0;
      DurableRecord::Status s = wrong.read(sink.data(), n - 1U, e);
      if (s == DurableRecord::RECORD_OK && !(file.size() == n + 3U)) {
        finding(r, "a handle of another record size read a record", i, file.data(), file.size(),
          "file size is not n - 1 + 4");
      }
    }
    progress(r, i);
  }
  (void)std::fprintf(r.log, "records: %llu reads gave RECORD_OK (%llu of them with a CRC-32 made "
    "valid on purpose). There is one copy only: <name>, plus <name>.tmp that write() uses and "
    "read() never opens (checked: a good record in .tmp is not read).\n",
    static_cast<unsigned long long>(ok_count), static_cast<unsigned long long>(forged_ok));
  return static_cast<int>(r.findings);
}

namespace
{

// One record of a store file as the reference scanner reads it.
struct Rec
{
  uint8_t tag;
  size_t offset;
  std::vector<uint8_t> body;
};

// What the reference scanner finds in a store file.
struct Scan
{
  size_t good_end;
  std::vector<Rec> recs;
  uint32_t next_number;
  uint32_t deleted_below;
  uint32_t dropped_through;
  bool ascending;   // report numbers rise along the file
};

/**
 * @brief Reference scan of a store file from the layout in report_store.hpp.
 *
 * Reads records in order and stops at the first one that is cut short, has a tag or a length
 * the tag cannot have, a wrong CRC-32, or (tag 'R') a body that is no HEALTH of 24 bytes or EVENT
 * of 15 to 47. The last state record is in force. The next number is the state's, or one past the
 * highest stored report when higher; 0 becomes 1.
 *
 * @param[in]     f  the file's bytes
 * @return the scan
 */
Scan refScan(const std::vector<uint8_t> & f)
{
  Scan s{};
  s.next_number = 1U;
  s.ascending = true;
  uint32_t highest = 0;
  uint32_t state_next = 1U;
  uint32_t last_number = 0;
  size_t at = 0;
  for (size_t guard = 0; guard < f.size() + 1U; guard++) {   // bounded
    if (at + kHead > f.size()) {
      break;
    }
    const uint8_t tag = f[at];
    const size_t n = f[at + 1U];
    const bool len_ok = (tag == kTagR && n >= kReportMin && n <= kReportMax) ||
                        (tag == kTagS && n == kStateLen);
    if (!len_ok || at + kHead + n + kCrc > f.size()) {
      break;
    }
    if (readLe32(&f[at + kHead + n]) != refCrc32(&f[at], kHead + n)) {
      break;
    }
    if (tag == kTagR) {
      const uint8_t kind = f[at + kHead + kKindAt];
      const bool shape = (kind == 1U && n == kHealthLen) || (kind == 2U);
      if (!shape) {
        break;
      }
    }
    Rec rec{tag, at, std::vector<uint8_t>(f.begin() + static_cast<long>(at + kHead),
      f.begin() + static_cast<long>(at + kHead + n))};
    if (tag == kTagS) {
      state_next = readLe32(&rec.body[0]);
      s.deleted_below = readLe32(&rec.body[4]);
      s.dropped_through = readLe32(&rec.body[8]);
    } else {
      const uint32_t number = readLe32(&rec.body[0]);
      highest = (number > highest) ? number : highest;
      s.ascending = s.ascending && number > last_number;
      last_number = number;
    }
    s.recs.push_back(rec);
    at += kHead + n + kCrc;
  }
  s.good_end = at;
  s.next_number = (highest >= state_next) ? highest + 1U : state_next;
  if (s.next_number == 0U) {
    s.next_number = 1U;
  }
  return s;
}

/**
 * @brief Counts record-shaped, CRC-valid pieces after the point where the scan stopped.
 *
 * Measures what a cut at the first damage loses: records that are whole and good, but lie after
 * the cut. Tries every byte offset after the cut.
 *
 * @param[in]     f     the file's bytes
 * @param[in]     from  the scan's good end
 * @return number of such records
 */
size_t validAfter(const std::vector<uint8_t> & f, size_t from)
{
  size_t found = 0;
  for (size_t at = from + 1U; at + kHead + kCrc <= f.size(); at++) {
    const uint8_t tag = f[at];
    const size_t n = f[at + 1U];
    const bool len_ok = (tag == kTagR && n >= kReportMin && n <= kReportMax) ||
                        (tag == kTagS && n == kStateLen);
    if (len_ok && at + kHead + n + kCrc <= f.size() &&
        readLe32(&f[at + kHead + n]) == refCrc32(&f[at], kHead + n)) {
      found++;
    }
  }
  return found;
}

/**
 * @brief A random report from the controller (Interfaces Sec. 5.4): HEALTH or EVENT.
 *
 * @param[in,out] rng  the generator
 * @return the payload, number and onboard time fields 0
 */
std::vector<uint8_t> makeReport(Rng & rng)
{
  const bool health = chance(rng, 55U);
  std::vector<uint8_t> p = randomBytes(rng, health ? kHealthLen
                                                   : kReportMin + below(rng, 33U));
  std::memset(p.data(), 0, 8);   // report number and onboard time are filled by the store
  p[kKindAt] = health ? 1U : 2U;
  return p;
}

/**
 * @brief Builds the seed store files through the public interface of ReportStore.
 *
 * Each seed: a store of random capacity (small ones force HEALTH drops), 3 to 30 reports,
 * sometimes a SEND REPORTS sequence (deleteBelow, takeDropped, startSend) that adds state
 * records.
 *
 * @param[in]     dir    scratch data directory
 * @param[in]     seed   run seed
 * @param[out]    seeds  the files' bytes
 * @return nothing
 */
void buildStoreSeeds(const std::string & dir, uint64_t seed,
                     std::vector<std::vector<uint8_t>> & seeds)
{
  for (size_t k = 0; k < kStoreSeeds; k++) {
    Rng rng = rngFor(seed, 2000000000ULL + k);
    (void)::unlink((dir + "/" + config::kReportStoreName).c_str());
    const size_t cap = chance(rng, 30U) ? 150U + below(rng, 500U) : kBigStore;
    ReportStore store(dir.c_str(), cap);
    if (!store.load()) {
      continue;
    }
    const uint32_t count = 3U + below(rng, 28U);
    std::array<uint8_t, report::kEventMaxBytes> out{};
    for (uint32_t j = 0; j < count; j++) {
      std::vector<uint8_t> p = makeReport(rng);
      (void)store.accept(p.data(), p.size(), static_cast<uint32_t>(nextU64(rng)), j, out.data(),
        out.size());
    }
    if (chance(rng, 40U)) {
      store.deleteBelow(1U + below(rng, count));
      (void)store.takeDropped();
      store.startSend(1U, store.nextNumber());
    }
    std::vector<uint8_t> bytes;
    if (getFile(dir + "/" + config::kReportStoreName, bytes)) {
      seeds.push_back(bytes);
    }
  }
}

/**
 * @brief Re-computes the CRC-32 of every record-shaped piece, walking by the length bytes.
 *
 * Makes a mutated file CRC-valid where its framing allows, so that the tag, length and body
 * checks decide instead of the CRC-32. Stops where a record would run past the end.
 *
 * @param[in,out] f  the file's bytes
 * @return nothing
 */
void fixCrcs(std::vector<uint8_t> & f)
{
  size_t at = 0;
  for (size_t guard = 0; guard < f.size() + 1U; guard++) {   // bounded
    if (at + kHead > f.size()) {
      return;
    }
    const size_t n = f[at + 1U];
    if (at + kHead + n + kCrc > f.size()) {
      return;
    }
    writeLe32(&f[at + kHead + n], refCrc32(&f[at], kHead + n));
    at += kHead + n + kCrc;
  }
}

}  // namespace

/**
 * @brief Target 6: ReportStore::load on fuzzed store files.
 *
 * After load(): the file must have been cut to the reference scan's good end; nextNumber() must
 * equal the reference's; onDisk() must be true. Then, for one input in eight with stored reports,
 * a SEND REPORTS read-back (startSend, nextToSend) must give only reports the reference scan holds
 * and that are live (not deleted, not dropped); when the numbers rise along the file it must give
 * all of them, EVENT reports first. A report read back from a file that is neither a seed nor
 * given a recomputed CRC-32 is a finding. Statistics of what damage costs are logged.
 *
 * @param[in,out] r  the report (options, log)
 * @return number of findings
 */
int runReports(Report & r)
{
  const std::string dir = r.opt.work_dir + "/reports";
  makeDir(dir);
  const std::string path = dir + "/" + config::kReportStoreName;
  std::vector<std::vector<uint8_t>> seeds;
  buildStoreSeeds(dir, r.opt.seed, seeds);
  if (seeds.empty()) {
    (void)std::fprintf(r.log, "FATAL: no seed store files could be built\n");
    r.findings++;
    return 1;
  }
  std::set<std::vector<uint8_t>> seed_reports;
  for (const auto & sf : seeds) {
    for (const Rec & rec : refScan(sf).recs) {
      if (rec.tag == kTagR) {
        seed_reports.insert(rec.body);
      }
    }
  }
  (void)std::fprintf(r.log, "reports: %zu seed store files, %zu distinct seed reports\n",
    seeds.size(), seed_reports.size());
  uint64_t damaged = 0;
  uint64_t lost_inputs = 0;
  uint64_t lost_records = 0;
  uint64_t readbacks = 0;
  uint64_t reports_read = 0;
  uint64_t order_skips = 0;
  const uint64_t lo = r.opt.has_only ? r.opt.only : 0U;
  const uint64_t hi = r.opt.has_only ? r.opt.only + 1U : r.opt.count;
  for (uint64_t i = lo; i < hi; i++) {
    Rng rng = rngFor(r.opt.seed, i);
    std::vector<uint8_t> f = seeds[below(rng, static_cast<uint32_t>(seeds.size()))];
    const std::vector<uint8_t> & other = seeds[below(rng, static_cast<uint32_t>(seeds.size()))];
    bool forged = false;
    const uint32_t mode = below(rng, 100U);
    if (mode < 8U) {
      f = randomBytes(rng, below(rng, 400U));
    } else if (mode >= 20U) {
      Scan sc = refScan(f);
      std::vector<Field> fields;
      for (const Rec & rec : sc.recs) {
        fields.push_back({rec.offset, 1});
        fields.push_back({rec.offset + 1U, 1});
        fields.push_back({rec.offset + kHead, 4});
        fields.push_back({rec.offset + kHead + 4U, 4});
        fields.push_back({rec.offset + kHead + kKindAt, 1});
      }
      mutate(rng, f, fields, 4000U, &other);
      if (chance(rng, 40U)) {
        fixCrcs(f);
        forged = true;
      }
    }
    const Scan ref = refScan(f);
    const size_t before_size = f.size();
    (void)putFile(path, f);
    (void)::unlink((path + ".tmp").c_str());
    const size_t cap = chance(rng, 85U) ? kBigStore : 100U + below(rng, 800U);
    inputBegin(r, i, f.data(), f.size());
    ReportStore store(dir.c_str(), cap);
    const bool loaded = store.load();
    inputEnd(r, i);
    std::vector<uint8_t> after;
    (void)getFile(path, after);
    if (!loaded || !store.onDisk()) {
      finding(r, "load() refused a store file the layout allows", i, f.data(), f.size(),
        "a file of any content is cut to its good records, never refused");
      continue;
    }
    if (after.size() != ref.good_end || !std::equal(after.begin(), after.end(), f.begin())) {
      finding(r, "load() left another file than the good records", i, f.data(), f.size(),
        (std::string("file size after ") + std::to_string(after.size()) + ", reference good end " +
        std::to_string(ref.good_end)).c_str());
    }
    if (store.nextNumber() != ref.next_number) {
      finding(r, "load() next report number differs from the reference", i, f.data(), f.size(),
        (std::string("got ") + std::to_string(store.nextNumber()) + " want " +
        std::to_string(ref.next_number)).c_str());
    }
    if (ref.good_end < before_size) {
      damaged++;
      const size_t later = validAfter(f, ref.good_end);
      if (later > 0U) {
        lost_inputs++;
        lost_records += later;
      }
    }
    bool has_report = false;
    for (const Rec & rec : ref.recs) {
      has_report = has_report || rec.tag == kTagR;
    }
    if (has_report && chance(rng, 12U)) {
      readbacks++;
      store.startSend(0U, store.nextNumber());
      std::array<uint8_t, report::kEventMaxBytes> out{};
      std::vector<std::vector<uint8_t>> got;
      for (size_t k = 0; k < kReadBackMax; k++) {
        size_t len = 0;
        if (!store.nextToSend(out.data(), out.size(), len)) {
          break;
        }
        got.emplace_back(out.begin(), out.begin() + static_cast<long>(len));
        store.markSent();
      }
      std::vector<std::vector<uint8_t>> want;
      for (int pass = 0; pass < 2; pass++) {
        const uint8_t kind = (pass == 0) ? 2U : 1U;
        uint32_t last = 0;
        for (const Rec & rec : ref.recs) {
          if (rec.tag != kTagR || rec.body[kKindAt] != kind) {
            continue;
          }
          const uint32_t number = readLe32(&rec.body[0]);
          const bool live = number >= ref.deleted_below &&
                            (kind != 1U || number > ref.dropped_through);
          if (live && number < store.nextNumber() && number > last) {
            want.push_back(rec.body);
            last = number;
          } else if (live && number <= last) {
            order_skips++;
          }
        }
      }
      reports_read += got.size();
      bool subset = true;
      for (const auto & g : got) {
        bool found = false;
        for (const Rec & rec : ref.recs) {
          found = found || (rec.tag == kTagR && rec.body == g);
        }
        subset = subset && found;
        if (found && !forged && seed_reports.count(g) == 0U) {
          finding(r, "a report was read back that no write produced", i, f.data(), f.size(),
            "not in any seed store and the CRC-32 was not recomputed");
        }
      }
      if (!subset || (ref.ascending && got != want)) {
        finding(r, "read-back after load() differs from the reference scan", i, f.data(),
          f.size(), (std::string("read ") + std::to_string(got.size()) + " reports, reference " +
          std::to_string(want.size())).c_str());
      }
    }
    progress(r, i);
  }
  (void)std::fprintf(r.log, "reports: damaged files (good records end before the file's end): "
    "%llu; of these %llu had %llu whole, CRC-valid records after the cut that load() dropped "
    "(cut at the first damage, as report_store.hpp says: TBR). Read-backs: %llu, reports read "
    "%llu; valid reports skipped by the number-order rule: %llu\n",
    static_cast<unsigned long long>(damaged), static_cast<unsigned long long>(lost_inputs),
    static_cast<unsigned long long>(lost_records), static_cast<unsigned long long>(readbacks),
    static_cast<unsigned long long>(reports_read), static_cast<unsigned long long>(order_skips));
  return static_cast<int>(r.findings);
}

}  // namespace fuzz
