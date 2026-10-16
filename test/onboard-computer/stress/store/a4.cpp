/*
 * a4.cpp — Part A.4, report numbers near 2^32.
 *
 * The public interface cannot start a store near 0xFFFFFFFF in reasonable time (4.29e9 reports),
 * so the store's file is written first in its own on-disk format: one state record whose next
 * number is 0xFFFFFFF0 (layout: report_store.hpp "File layout"; tags and framing
 * report_store.cpp:403-408; state body report_store.cpp:412-418; record encoding
 * report_store.cpp:646-653). load() reads that file as it would a file it wrote itself. Reports
 * are then given through accept() across 0xFFFFFFFF, and SEND REPORTS is run the way
 * updater.cpp:2063-2075 runs it: end = nextNumber(); deleteBelow(first); takeDropped();
 * startSend(first, end).
 */
#include "a4.hpp"

#include <array>
#include <cstdint>
#include <vector>

#include "config.hpp"
#include "protocol.hpp"
#include "report_store.hpp"
#include "util.hpp"

namespace
{

using util::Bytes;

// First report number of the seeded store: 16 numbers below the wrap. Test choice: small enough
// to cross the wrap in a short run.
constexpr uint32_t kStart = 0xFFFFFFF0U;
// Reports given in a crossing: 16 numbers up to 0xFFFFFFFF, then 8 past the wrap. Test choice.
constexpr size_t kCrossing = 24;
// "The ground has fetched up to here" before the crossing, for the second scenario. Test choice:
// 16 below kStart. SEND REPORTS from N stores deleted_below = N (report_store.cpp:1377-1394).
constexpr uint32_t kFetchedBelow = 0xFFFFFFE0U;
// Capacity of the small store, bytes: room for about 13 HEALTH records (30 bytes each).
// Test choice: small enough that the crossing overflows it.
constexpr size_t kSmallCapacity = 400;
// Reports given in the small-store crossing. Test choice.
constexpr size_t kSmallCrossing = 40;
// Every kEventEvery-th report is an EVENT of the shortest length, the rest HEALTH. Test choice.
constexpr size_t kEventEvery = 3;
// Room for one report.
constexpr size_t kRoom = 64;
// Bound on reports read back in one enumeration (B2).
constexpr size_t kEnumBound = 1000;
// Event code of the generated EVENT reports: 0x0105, "clock differs" (protocol.hpp:212).
constexpr uint16_t kEventCode = 0x0105U;
// Bits in a byte, for splitting the event code.
constexpr unsigned kByteBits = 8U;
// SEND REPORTS "first" values probed across the wrap. Test choice: the start, inside the old
// side, the last old number, the first new numbers, and 0.
constexpr std::array<uint32_t, 6> kFirsts = {kStart, 0xFFFFFFF8U, 0xFFFFFFFFU, 1U, 3U, 0U};

// What a SEND REPORTS probe saw.
struct Probe
{
  uint32_t first;
  uint32_t end;
  std::vector<uint32_t> events;
  std::vector<uint32_t> health;
  DroppedHealth dropped;
  util::View after;                // the file after the sequence
  std::vector<uint32_t> in_file;   // report numbers in the file after the sequence
};

/**
 * @brief Text of numbers as hexadecimal runs, "FFFFFFF0..FFFFFFF3,00000001".
 *
 * @param[in]     v  numbers in the order given
 * @return runs of consecutive numbers joined with "..", separated by ","; "-" when empty
 */
std::string runs(const std::vector<uint32_t> & v)
{
  if (v.empty()) {
    return "-";
  }
  std::string out;
  size_t i = 0;
  std::array<char, 16> buf{};
  while (i < v.size()) {
    size_t j = i;
    while (j + 1U < v.size() && v[j + 1U] == v[j] + 1U) {
      j++;
    }
    (void)std::snprintf(buf.data(), buf.size(), "%08X", v[i]);
    out += (out.empty() ? "" : ",") + std::string(buf.data());
    if (j > i) {
      (void)std::snprintf(buf.data(), buf.size(), "%08X", v[j]);
      out += ".." + std::string(buf.data());
    }
    i = j + 1U;
  }
  return out;
}

/**
 * @brief Writes the seeded store file: one state record with the given next number and deletion.
 *
 * @param[in]     dir      data directory (emptied first)
 * @param[in]     next     next report number
 * @param[in]     deleted  deleted below
 * @return true: written
 */
bool seedStore(const std::string & dir, uint32_t next, uint32_t deleted)
{
  if (!util::resetDir(dir)) {
    return false;
  }
  Bytes rec = util::encodeRecord(util::kTagState, util::stateBody(next, deleted, 0, 0, 0, 0));
  return util::writeFile(dir + "/" + config::kReportStoreName, rec.data(), rec.size());
}

/**
 * @brief Gives the store a report: the shortest EVENT every kEventEvery-th, else HEALTH.
 *
 * @param[in,out] store  the store
 * @param[in]     index  report index
 * @param[out]    kind   the kind given
 * @return the number the store gave; 0 when it refused the report
 */
uint32_t give(ReportStore & store, size_t index, uint8_t & kind)
{
  bool event = index % kEventEvery == 0U;
  Bytes p(event ? util::kEventMinLen : util::kHealthLen, 0);
  p[util::kKindAt] = event ? util::kKindEvent : util::kKindHealth;
  if (event) {
    p[report::kOffsetEventCode] = static_cast<uint8_t>(kEventCode & 0xFFU);
    p[report::kOffsetEventCode + 1U] = static_cast<uint8_t>(kEventCode >> kByteBits);
  } else {
    p[report::kOffsetMode] = report::kModeOperate;
  }
  std::array<uint8_t, kRoom> out{};
  kind = p[util::kKindAt];
  if (!store.accept(p.data(), p.size(), static_cast<uint32_t>(index), index, out.data(),
        out.size())) {
    return 0;
  }
  return util::readLe32(&out[util::kNumberAt]);
}

/**
 * @brief Report numbers of the report records in the file of a directory, and the state in force.
 *
 * @param[in]     dir      data directory
 * @param[out]    numbers  report numbers in file order
 * @param[out]    view     the oracle's view of the file
 * @return nothing
 */
void readFileNumbers(const std::string & dir, std::vector<uint32_t> & numbers, util::View & view)
{
  util::Bytes file;
  (void)util::readFile(dir + "/" + config::kReportStoreName, file);
  util::Parsed parsed = util::parse(file.data(), file.size());
  view = util::evaluate(parsed);
  numbers.clear();
  for (const util::Rec & r : parsed.recs) {
    if (r.tag == util::kTagReport) {
      numbers.push_back(util::readLe32(&r.body[util::kNumberAt]));
    }
  }
}

/**
 * @brief Runs SEND REPORTS on a loaded store the way updater.cpp:2063-2075 does.
 *
 * @param[in,out] s     a loaded store
 * @param[in]     work  the store's data directory
 * @param[in]     first the first report number of the command
 * @param[out]    out   what was seen
 * @return nothing
 */
void sendReports(ReportStore & s, const std::string & work, uint32_t first, Probe & out)
{
  out = Probe{};
  out.first = first;
  out.end = s.nextNumber();
  s.deleteBelow(first);
  out.dropped = s.takeDropped();
  s.startSend(first, out.end);
  std::array<uint8_t, kRoom> buf{};
  for (size_t i = 0; i < kEnumBound && s.sending(); i++) {
    size_t len = 0;
    if (!s.nextToSend(buf.data(), buf.size(), len)) {
      break;
    }
    uint32_t n = util::readLe32(&buf[util::kNumberAt]);
    (buf[util::kKindAt] == util::kKindEvent ? out.events : out.health).push_back(n);
    s.markSent();
  }
  readFileNumbers(work, out.in_file, out.after);
}

/**
 * @brief Runs SEND REPORTS after a restart: a copy of the file, loaded by a fresh store.
 *
 * @param[in]     base      the directory to copy
 * @param[in]     work      where the copy goes
 * @param[in]     capacity  the store's capacity in bytes
 * @param[in]     first     the first report number of the command
 * @param[out]    out       what was seen
 * @return true: the probe ran
 */
bool probeRestart(const std::string & base, const std::string & work, size_t capacity,
  uint32_t first, Probe & out)
{
  if (!util::copyDir(base, work)) {
    return false;
  }
  ReportStore s(work.c_str(), capacity);
  if (!s.load()) {
    return false;
  }
  sendReports(s, work, first, out);
  return true;
}

/**
 * @brief Runs SEND REPORTS without a restart: a seeded store, the crossing, then the command.
 *
 * @param[in]     work      the data directory (seeded here)
 * @param[in]     deleted   deleted-below in the seeded state
 * @param[in]     capacity  the store's capacity in bytes
 * @param[in]     count     reports to give before the command
 * @param[in]     first     the first report number of the command
 * @param[out]    out       what was seen
 * @return true: the probe ran
 */
bool probeLive(const std::string & work, uint32_t deleted, size_t capacity, size_t count,
  uint32_t first, Probe & out)
{
  if (!seedStore(work, kStart, deleted)) {
    return false;
  }
  ReportStore s(work.c_str(), capacity);
  if (!s.load()) {
    return false;
  }
  for (size_t i = 0; i < count; i++) {
    uint8_t kind = 0;
    (void)give(s, i, kind);
  }
  sendReports(s, work, first, out);
  return true;
}

/**
 * @brief Gives a seeded store a crossing and logs the numbers, the file and a reload.
 *
 * @param[in]     base      the seeded directory
 * @param[in]     capacity  store capacity in bytes
 * @param[in]     count     reports to give
 * @param[in]     log       human log
 * @param[in,out] findings  incremented when the reload does not give the in-RAM next number
 * @return true: the store loaded
 *
 * A finding is a reload whose nextNumber() differs from the nextNumber() the store had in RAM
 * after its last report, or a reload that fails. The log line of a finding starts with
 * "FINDING " (the same marker the boundaries log uses) so that main can count it.
 */
bool cross(const std::string & base, size_t capacity, size_t count, FILE * log, size_t & findings)
{
  std::vector<uint32_t> given;
  std::vector<uint32_t> given_events;
  uint32_t last_number = 0;
  uint32_t next_number = 0;
  {
    ReportStore s(base.c_str(), capacity);
    if (!s.load()) {
      return false;
    }
    (void)std::fprintf(log, "load() of the seeded file gave nextNumber()=%08X\n", s.nextNumber());
    for (size_t i = 0; i < count; i++) {
      uint8_t kind = 0;
      uint32_t n = give(s, i, kind);
      given.push_back(n);
      if (kind == util::kKindEvent) {
        given_events.push_back(n);
      }
    }
    last_number = s.lastNumber();
    next_number = s.nextNumber();
  }
  (void)std::fprintf(log, "accept() gave numbers: %s\n", runs(given).c_str());
  (void)std::fprintf(log, "  of which EVENT: %s\n", runs(given_events).c_str());
  (void)std::fprintf(log, "  lastNumber()=%08X nextNumber()=%08X after the last report\n",
    last_number, next_number);
  std::vector<uint32_t> in_file;
  util::View v{};
  readFileNumbers(base, in_file, v);
  (void)std::fprintf(log, "file on disk: %s; last state record: next=%08X deleted_below=%08X "
    "through=%08X dropped=%u\n", runs(in_file).c_str(), v.state_next, v.deleted_below, v.through,
    v.count);
  ReportStore again(base.c_str(), capacity);
  bool ok = again.load();
  const bool reload_differs = !ok || again.nextNumber() != next_number;
  if (reload_differs) {
    findings++;
  }
  (void)std::fprintf(log, "%sreload: load()=%d nextNumber()=%08X (the last number given was "
    "%08X)\n", reload_differs ? "FINDING " : "", ok ? 1 : 0, again.nextNumber(),
    given.empty() ? 0U : given.back());
  return true;
}

/**
 * @brief Runs one crossing scenario and logs it.
 *
 * @param[in]     name      scenario name
 * @param[in]     root      scratch directory
 * @param[in]     deleted   deleted-below in the seeded state
 * @param[in]     capacity  store capacity in bytes
 * @param[in]     count     reports to give
 * @param[in]     log       human log
 * @param[in]     csv       one row per probe
 * @param[in,out] findings  incremented per finding of the crossing
 * @return true: the scenario ran
 */
bool scenario(const char * name, const std::string & root, uint32_t deleted, size_t capacity,
  size_t count, FILE * log, FILE * csv, size_t & findings)
{
  const std::string base = root + "/base";
  if (!seedStore(base, kStart, deleted)) {
    return false;
  }
  (void)std::fprintf(log, "\n== %s: file seeded with next=%08X deleted_below=%08X; capacity %zu "
    "bytes\n", name, kStart, deleted, capacity);
  if (!cross(base, capacity, count, log, findings)) {
    return false;
  }
  for (int live = 1; live >= 0; live--) {
    (void)std::fprintf(log, live != 0 ? "-- SEND REPORTS without a restart (end = in-RAM next "
      "number)\n" : "-- SEND REPORTS after a restart (fresh load() of the file above)\n");
    for (uint32_t first : kFirsts) {
      Probe p{};
      bool ran = (live != 0) ? probeLive(root + "/probe", deleted, capacity, count, first, p)
                             : probeRestart(base, root + "/probe", capacity, first, p);
      if (!ran) {
        return false;
      }
      (void)std::fprintf(log, "first=%08X end=%08X: sent EVENT %s | HEALTH %s | dropped count %u; "
        "file afterwards: %s (deleted_below=%08X)\n", p.first, p.end, runs(p.events).c_str(),
        runs(p.health).c_str(), p.dropped.count, runs(p.in_file).c_str(), p.after.deleted_below);
      (void)std::fprintf(csv, "%s,%s,%08X,%08X,%zu,%zu,%zu\n", name, live != 0 ? "live" : "restart",
        p.first, p.end, p.events.size(), p.health.size(), p.in_file.size());
    }
  }
  return true;
}

}  // namespace

namespace a4
{

/**
 * @brief Runs Part A.4 (report numbers).
 *
 * Three crossings: the ground has fetched nothing (deleted_below 0); the ground has fetched up to
 * 0xFFFFFFE0 (deleted_below high); and a small store that overflows during the crossing.
 *
 * @param[in]     workdir  scratch directory (build/); a4/ inside is emptied
 * @param[in]     log      human log
 * @param[in]     csv      one row per probe: scenario,mode,first,end,events sent,health sent,file
 *     reports
 * @param[out]    findings log lines that start with "FINDING " (see cross())
 * @return 0: the run completed (observations are in the log); 1: it could not be set up
 */
int run(const std::string & workdir, FILE * log, FILE * csv, size_t & findings)
{
  findings = 0;
  const std::string root = workdir + "/a4";
  if (!util::resetDir(root)) {
    return 1;
  }
  (void)std::fprintf(csv, "scenario,mode,first,end,events_sent,health_sent,reports_in_file\n");
  bool ok = scenario("W0 nothing fetched", root + "/w0", 0U, config::kReportStoreBytes, kCrossing,
    log, csv, findings);
  ok = ok && scenario("W1 fetched below FFFFFFE0", root + "/w1", kFetchedBelow,
    config::kReportStoreBytes, kCrossing, log, csv, findings);
  ok = ok && scenario("W2 small store overflows", root + "/w2", 0U, kSmallCapacity,
    kSmallCrossing, log, csv, findings);
  return ok ? 0 : 1;
}

}  // namespace a4
