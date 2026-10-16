/*
 * a2.cpp — Part A.2: fill the report store past 1 MB through its public interface, then cut a
 * copy of the file at every byte of each of the last 3 records, reload it with a fresh store and
 * check what the reload holds against an oracle that reads the file by its documented layout
 * (util.hpp) and against a model of every report accepted.
 *
 * Three files are cut: S1 (after the fill, which includes at least one compaction),
 * S2 (a compaction forced by deleteBelow(), then more reports appended after it) and S3 (right
 * after that compaction, the rewritten file's own last records).
 */
#include "a2.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <map>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.hpp"
#include "protocol.hpp"
#include "report_store.hpp"
#include "util.hpp"

namespace
{

using util::Bytes;

// Reports accepted in the fill. With 30 % EVENT (average 31 bytes + 6 framing) and 70 % HEALTH
// (24 + 6), a report averages about 32 bytes: 1 MB (config.hpp:190) is full after about 33,000;
// the file passes live + kReportDeadMaxBytes (config.hpp:200) and is rewritten at about 65,000.
// 100,000 therefore passes the capacity three times over and compacts at least once. Test choice.
constexpr size_t kFillReports = 100000;
// Fixed seed of the report mix: the run is the same every time. Test choice.
constexpr uint64_t kSeed = 0x5EED20261016ULL;
// Share of EVENT reports in the first mix, per cent. "Many events" (task brief); test choice.
// 100,000 reports x 30 % x 37 bytes is 1.1 MB: events alone pass the 1 MB capacity, so this fill
// ends in the "events alone fill the store" state (report_store.cpp:951-962).
constexpr uint64_t kManyEventsPercent = 30;
// Share of EVENT reports in the second mix, per cent: few enough that the oldest-HEALTH-first
// drop (report_store.cpp:977-1030) runs on every report after the store is full. Test choice.
constexpr uint64_t kFewEventsPercent = 10;
// Reports in the second fill: 1 MB is full after about 33,000 (31 bytes each); 60,000 passes
// the capacity and the rewrite threshold (config.hpp:200). Test choice.
constexpr size_t kSecondFillReports = 60000;
// Records at the end of the file whose every byte is cut (task brief: the last 3).
constexpr size_t kTailRecords = 3;
// Cases run at once. The Mac runs other agents' work too; test choice.
constexpr size_t kWorkers = 4;
// Room for one report out of nextToSend and accept (protocol.hpp:248: at most 47 bytes).
constexpr size_t kRoom = 64;
// Reports appended after the forced compaction in S2. Test choice.
constexpr size_t kExtraReports = 10;
// Onboard time of the first report, seconds; each next report is one second later. Test choice.
constexpr uint32_t kTimeStart = 1000U;
// Upper bound on reports read back in one enumeration (B2): a full store is about 35,000
// (config.hpp:208); twice the fill is far more than any file holds.
constexpr size_t kEnumBound = 2U * kFillReports;
// Largest record: framing + EVENT of 47 bytes (report_store.cpp:421).
constexpr size_t kRecordMax = util::kOverhead + util::kEventMaxLen;
// Smallest HEALTH record, framing included: dropOldestHealth() overshoots by less than this.
constexpr size_t kHealthRecord = util::kOverhead + util::kHealthLen;
// Seed offset of the report stored after a reload, so it differs from the fill's reports.
constexpr uint64_t kAfterSeed = 0xAF7E4ULL;
// Exit code of a case whose files could not be set up (not a finding).
constexpr int kSetupFailed = 2;
// Lines of a failed case's result kept in the log, and failed cases shown in full per scenario.
constexpr size_t kStderrKeep = 6;
constexpr size_t kFailShow = 3;
// Longest line of a result file read back.
constexpr size_t kLineMax = 400;

// One report as accepted: the model of what the store was given and gave back.
struct Item
{
  uint32_t number;
  uint8_t kind;
  uint32_t time_s;
  Bytes bytes;
};

using Model = std::vector<Item>;

// A file to cut, with the model of what it should hold.
struct Scenario
{
  std::string name;
  Bytes file;
  Model model;
  std::string dir;       // where the cut copies go
  bool check_overdrop;   // true when only drops (no deletion) have taken space out of the store
  bool append_tail;      // false: the file is a rewrite, its state record comes first and covers
                         // reports whose records follow it (a cut there is damage, not a power cut)
};

// One cut: which record, where.
struct Cut
{
  size_t from_end;       // 1: the last record
  size_t rec_offset;
  size_t rec_size;
  uint8_t rec_tag;
  uint32_t rec_number;   // for a report record; 0 for a state
  size_t at;             // the cut, in bytes from the file start
};

// The scenario and its cuts: the context of runCut.
struct Plan
{
  const Scenario * scn;
  std::vector<Cut> cuts;
};

// What the fill did.
struct FillStats
{
  size_t accepted;
  size_t compactions;
  size_t max_file;
  size_t events;
  size_t health;
};

/**
 * @brief Makes one report as the controller sends it (Sec. 5.4 layout), from the seeded generator.
 *
 * Number and onboard time are 0, as the controller sends them (report_store.hpp). HEALTH fields
 * that log lines depend on stay fixed, so the log stays short.
 *
 * @param[in,out] rng            generator state
 * @param[in]     index          report index, kept in the controller-time field
 * @param[in]     event_percent  share of EVENT reports, per cent
 * @return the report's bytes
 */
Bytes makeReport(uint64_t & rng, uint32_t index, uint64_t event_percent)
{
  Bytes p;
  if (util::splitmix(rng) % 100U < event_percent) {
    size_t span = util::kEventMaxLen - util::kEventMinLen + 1U;
    size_t len = util::kEventMinLen + static_cast<size_t>(util::splitmix(rng) % span);
    p.assign(len, 0);
    for (size_t i = util::kKindAt + 1U; i < len; i++) {
      p[i] = static_cast<uint8_t>(util::splitmix(rng));
    }
    p[util::kKindAt] = util::kKindEvent;
  } else {
    p.assign(util::kHealthLen, 0);
    util::writeLe32(&p[report::kOffsetControllerTime], index);
    p[util::kKindAt] = util::kKindHealth;
    p[report::kOffsetMode] = report::kModeOperate;
    p[report::kOffsetFirmwareKind] = 1U;
    p[report::kOffsetSlot] = 1U;
    p[report::kOffsetTrialLeft] = static_cast<uint8_t>(util::splitmix(rng));
  }
  return p;
}

/**
 * @brief Gives the store one report and notes it in the model.
 *
 * @param[in,out] store  the store
 * @param[in,out] rng    generator state
 * @param[in]     index  report index (also gives the onboard time)
 * @param[in]     event_percent  share of EVENT reports, per cent
 * @param[in,out] model  receives the report as the store numbered it
 * @return true: the store accepted it
 */
bool feed(ReportStore & store, uint64_t & rng, uint32_t index, uint64_t event_percent,
  Model & model)
{
  Bytes p = makeReport(rng, index, event_percent);
  std::array<uint8_t, kRoom> out{};
  uint32_t t = kTimeStart + index;
  if (!store.accept(p.data(), p.size(), t, index, out.data(), out.size())) {
    return false;
  }
  Item it{};
  it.number = util::readLe32(&out[util::kNumberAt]);
  it.kind = out[util::kKindAt];
  it.time_s = t;
  it.bytes.assign(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(p.size()));
  model.push_back(it);
  return true;
}

/**
 * @brief Size of a file in bytes.
 *
 * @param[in]     path  the file
 * @return its size; 0 when it cannot be read
 */
size_t sizeOf(const std::string & path)
{
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0 || st.st_size < 0) {
    return 0;
  }
  return static_cast<size_t>(st.st_size);
}

/**
 * @brief Enumerates what SEND REPORTS from first to end would send, through the public interface.
 *
 * @param[in,out] store  a loaded store
 * @param[in]     first  first report number
 * @param[in]     end    one past the last
 * @param[out]    sent   the reports in the order given
 * @return true: every EVENT came before every HEALTH; false: an EVENT came after a HEALTH
 */
bool enumerate(ReportStore & store, uint32_t first, uint32_t end, std::vector<util::Rep> & sent)
{
  sent.clear();
  store.startSend(first, end);
  bool order_ok = true;
  bool seen_health = false;
  std::array<uint8_t, kRoom> out{};
  for (size_t i = 0; i < kEnumBound && store.sending(); i++) {
    size_t len = 0;
    if (!store.nextToSend(out.data(), out.size(), len)) {
      break;
    }
    util::Rep r{util::readLe32(&out[util::kNumberAt]), out[util::kKindAt],
      Bytes(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(len))};
    seen_health = seen_health || r.kind == util::kKindHealth;
    order_ok = order_ok && !(seen_health && r.kind == util::kKindEvent);
    sent.push_back(r);
    store.markSent();
  }
  return order_ok;
}

/**
 * @brief Text of a pair of numbers for the failure list.
 *
 * @param[in]     what  a short name
 * @param[in]     a     first value
 * @param[in]     b     second value
 * @return "what (a vs b)"
 */
std::string pair(const char * what, unsigned long long a, unsigned long long b)
{
  return std::string(what) + " (" + std::to_string(a) + " vs " + std::to_string(b) + ")";
}

/**
 * @brief Compares the sent reports with the oracle's live reports (same reports, same order).
 *
 * @param[in]     sent    what the store sent, in order
 * @param[in]     view    the oracle's view of the surviving file
 * @param[in,out] fails   receives a line for each difference
 * @return nothing
 */
void compareSent(const std::vector<util::Rep> & sent, const util::View & view,
  std::vector<std::string> & fails)
{
  std::vector<util::Rep> want = view.events;
  want.insert(want.end(), view.health.begin(), view.health.end());
  if (sent.size() != want.size()) {
    fails.push_back(pair("sent count differs from the file's live reports", sent.size(),
      want.size()));
  }
  for (size_t i = 0; i < sent.size() && i < want.size(); i++) {
    if (sent[i].number != want[i].number || sent[i].bytes != want[i].bytes) {
      fails.push_back(pair("report differs from the file at send index", i, sent[i].number));
      break;
    }
  }
}

/**
 * @brief Bytes the live EVENT reports take in the file, framing included.
 *
 * @param[in]     view  the oracle's view of the surviving file
 * @return the bytes
 */
size_t eventBytes(const util::View & view)
{
  size_t total = 0;
  for (const util::Rep & r : view.events) {
    total += util::kOverhead + r.bytes.size();
  }
  return total;
}

/**
 * @brief Checks the surviving reports against the model of everything accepted.
 *
 * Events numbered at or above deleted_below must be live with the bytes accepted; HEALTH must be
 * dropped exactly when numbered up to "through" (oldest first, contiguous); the dropped count and
 * span in the state must be what the model gives; HEALTH may be dropped only while the store is
 * full (no over-dropping).
 *
 * @param[in]     scn     the scenario (model, over-drop check flag)
 * @param[in]     view    the oracle's view of the surviving file
 * @param[in,out] fails   receives a line for each difference
 * @return nothing
 */
void compareModel(const Scenario & scn, const util::View & view, std::vector<std::string> & fails)
{
  std::map<uint32_t, const util::Rep *> live;
  for (const util::Rep & r : view.events) {
    live[r.number] = &r;
  }
  for (const util::Rep & r : view.health) {
    live[r.number] = &r;
  }
  size_t dropped = 0;
  uint32_t first_t = 0;
  uint32_t last_t = 0;
  const size_t room_slack = kTailRecords * kRecordMax + kHealthRecord;
  const bool events_full = eventBytes(view) + kHealthRecord
      + room_slack > config::kReportStoreBytes;
  // Reports numbered past the highest stored report can still be counted: a HEALTH that was not
  // stored is counted in a state record whose next number already covers it.
  const uint32_t state_last = (view.state_next > 0U) ? view.state_next - 1U : 0U;
  const uint32_t bound = std::max(view.highest, state_last);
  const Item * extra = nullptr;   // the one HEALTH that may or may not be in the state's count
  size_t optional = 0;            // HEALTH after the highest record of a rewritten file
  for (const Item & it : scn.model) {
    const bool is_health = it.kind == util::kKindHealth;
    if (it.number > bound) {
      continue;
    }
    if (it.number > view.highest && !scn.append_tail) {
      optional += is_health ? 1U : 0U;   // lost to damage of a rewritten file: count is a range
      continue;
    }
    if (it.number > view.highest) {
      // No record of it is in the file. A HEALTH numbered below the state's last number was not
      // stored and was counted; the one numbered state_last may instead be the report whose own
      // record was cut. An EVENT is always stored, so only state_last can be lost to the cut.
      if (is_health && it.number < state_last) {
        dropped++;
        first_t = (dropped == 1U) ? it.time_s : first_t;
        last_t = it.time_s;
      } else if (is_health) {
        extra = &it;
      } else if (it.number < state_last) {
        fails.push_back(pair("EVENT missing", it.number, view.highest));
      }
      continue;
    }
    auto found = live.find(it.number);
    const bool by_rule = is_health && it.number <= view.through;
    if (it.number < view.deleted_below) {
      if (by_rule) {   // deleted by SEND REPORTS, but it was dropped (counted) before
        dropped++;
        first_t = (dropped == 1U) ? it.time_s : first_t;
        last_t = it.time_s;
      }
      continue;
    }
    // A HEALTH above "through" that is not in the file was not stored because events fill the
    // store: it counts as dropped (report_store.hpp, "If only events are left ...").
    const bool unstored = is_health && !by_rule && found == live.end();
    if (by_rule || unstored) {
      dropped++;
      first_t = (dropped == 1U) ? it.time_s : first_t;
      last_t = it.time_s;
      if (by_rule && found != live.end()) {
        fails.push_back(pair("HEALTH numbered up to 'through' is still live", it.number,
          view.through));
      }
      if (unstored && scn.check_overdrop && !events_full) {
        fails.push_back(pair("HEALTH not stored while the store had room", it.number,
          view.live_bytes));
      }
    } else if (found == live.end()) {
      fails.push_back(pair("EVENT missing", it.number, view.highest));
    } else if (found->second->bytes != it.bytes) {
      fails.push_back(pair("report bytes differ from the accepted report", it.number, 0));
    }
  }
  if (extra != nullptr && view.count == dropped + 1U) {
    dropped++;
    first_t = (dropped == 1U) ? extra->time_s : first_t;
    last_t = extra->time_s;
  }
  bool span_bad = dropped > 0U && (first_t != view.first_s || last_t != view.last_s);
  if (!scn.append_tail) {
    span_bad = false;   // a range check only
    dropped = (view.count >= dropped && view.count <= dropped + optional) ? view.count : dropped;
  }
  if (dropped != view.count || span_bad) {
    fails.push_back(pair("dropped count/span in the state differs from the model", view.count,
      dropped));
  }
  if (scn.check_overdrop && view.through > 0U && !view.health.empty() &&
      view.live_bytes + room_slack <= config::kReportStoreBytes) {
    fails.push_back(pair("HEALTH dropped while the store had room", view.live_bytes,
      config::kReportStoreBytes));
  }
}

/**
 * @brief Stores one more report after a reload and says how its number follows the file's.
 *
 * Classes: "contig" (highest + 1), "gap" (past highest + 1; "gap-explained" when the state record
 * in force already names a next number above highest + 1, i.e. the cut report's own number),
 * "REUSE" (a number the surviving file already holds).
 *
 * @param[in,out] store  a loaded store
 * @param[in]     view   the oracle's view of the surviving file
 * @param[in]     seed   generator seed for the report
 * @param[out]    got    receives the report as the store gave it back
 * @param[out]    cls    the class
 * @return the number given
 */
uint32_t storeAfter(ReportStore & store, const util::View & view, uint64_t seed, Item & got,
  std::string & cls)
{
  Model one;
  uint64_t rng = seed;
  (void)feed(store, rng, 0, kManyEventsPercent, one);
  got = one.empty() ? Item{} : one[0];
  if (got.number <= view.highest) {
    cls = "REUSE";
  } else if (got.number == view.highest + 1U) {
    cls = "contig";
  } else {
    cls = (view.state_next > view.highest + 1U) ? "gap-explained" : "gap";
  }
  return got.number;
}

/**
 * @brief Writes a case's result file: the class line, then one line per failed check.
 *
 * @param[in]     path   the file
 * @param[in]     line1  first line (key=value pairs)
 * @param[in]     fails  failed checks
 * @return nothing
 */
void writeResult(const std::string & path, const std::string & line1,
  const std::vector<std::string> & fails)
{
  std::string text = line1 + "\n";
  for (const std::string & f : fails) {
    text += f + "\n";
  }
  (void)util::writeFile(path, reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

/**
 * @brief Runs one cut: truncate a copy, reload it, check, store a report, reload again.
 *
 * @param[in]     context  the Plan
 * @param[in]     index    index into plan.cuts
 * @return 0: all checks held; 1: a check failed (lines in result.txt of the case's directory)
 */
int runCut(const void * context, size_t index)
{
  const Plan & plan = *static_cast<const Plan *>(context);
  const Scenario & scn = *plan.scn;
  const Cut & cut = plan.cuts[index];
  std::string dir = scn.dir + "/c" + std::to_string(index);
  std::vector<std::string> fails;
  if (!util::resetDir(dir) ||
      std::freopen((dir + "/stderr.txt").c_str(), "w", stderr) == nullptr) {
    return kSetupFailed;
  }
  const std::string store_file = dir + "/" + config::kReportStoreName;
  (void)util::writeFile(store_file, scn.file.data(), cut.at);
  util::Parsed parsed = util::parse(scn.file.data(), cut.at);
  util::View view = util::evaluate(parsed);
  ReportStore store(dir.c_str(), config::kReportStoreBytes);
  if (!store.load()) {
    fails.push_back("load() returned false");
  }
  if (sizeOf(store_file) != parsed.good_end) {
    fails.push_back(pair("file size after load differs from the good end", sizeOf(store_file),
      parsed.good_end));
  }
  uint32_t want_next = std::max(view.highest + 1U, view.state_next);
  want_next = (view.have_state || view.highest > 0U) ? want_next : 1U;
  if (store.nextNumber() != want_next) {
    fails.push_back(pair("next number after reload", store.nextNumber(), want_next));
  }
  DroppedHealth dropped = store.takeDropped();
  if (dropped.count != view.count || dropped.first_time_s != view.first_s ||
      dropped.last_time_s != view.last_s) {
    fails.push_back(pair("dropped counters after reload", dropped.count, view.count));
  }
  std::vector<util::Rep> sent;
  if (!enumerate(store, 1U, store.nextNumber(), sent)) {
    fails.push_back("an EVENT was sent after a HEALTH (REQ-078)");
  }
  compareSent(sent, view, fails);
  compareModel(scn, view, fails);
  Item got{};
  std::string cls;
  uint32_t n = storeAfter(store, view, kSeed ^ kAfterSeed ^ index, got, cls);
  if (cls == "REUSE") {
    fails.push_back(pair("next report reused a number the file holds", n, view.highest));
  }
  // A HEALTH report is not stored when the live EVENT reports alone leave no room for it
  // (report_store.hpp: "a new HEALTH is not stored and counts as dropped").
  const bool expect_unstored = got.kind == util::kKindHealth &&
                               eventBytes(view) + kHealthRecord > config::kReportStoreBytes;
  ReportStore again(dir.c_str(), config::kReportStoreBytes);
  std::vector<util::Rep> sent2;
  bool loaded = again.load();
  bool order2 = loaded && enumerate(again, n, n + 1U, sent2);
  bool found = sent2.size() == 1U && sent2[0].number == n && sent2[0].bytes == got.bytes;
  DroppedHealth counted = again.takeDropped();
  bool after_ok = expect_unstored ? (loaded && !found && counted.count == 1U)
                                  : (loaded && order2 && found);
  if (!after_ok || again.nextNumber() != n + 1U) {
    fails.push_back(pair("report stored after the reload is not found by a second reload", n,
      again.nextNumber()));
  }
  std::string line1 = "cls=" + cls + " new=" + std::to_string(n) + " highest=" +
                      std::to_string(view.highest) + " state_next=" +
                      std::to_string(view.state_next) + " cut_number=" +
                      std::to_string(cut.rec_number);
  writeResult(dir + "/result.txt", line1, fails);
  return fails.empty() ? 0 : 1;
}

/**
 * @brief Fills a loaded store through accept() and notes what happened.
 *
 * @param[in]     dir    the store's data directory
 * @param[in]     count  reports to give
 * @param[in]     event_percent  share of EVENT reports, per cent
 * @param[in,out] rng    generator state
 * @param[in,out] store  a loaded store on disk
 * @param[in,out] model  receives every report, as numbered
 * @param[in,out] stats  receives what the fill did
 * @return true: all accepted
 */
bool fill(const std::string & dir, size_t count, uint64_t event_percent, uint64_t & rng,
  ReportStore & store, Model & model, FillStats & stats)
{
  const std::string file = dir + "/" + config::kReportStoreName;
  size_t last = sizeOf(file);
  for (size_t i = 0; i < count; i++) {
    if (!feed(store, rng, static_cast<uint32_t>(i), event_percent, model)) {
      return false;
    }
    size_t now = sizeOf(file);
    stats.compactions += (now < last) ? 1U : 0U;
    stats.max_file = std::max(stats.max_file, now);
    last = now;
    stats.events += (model.back().kind == util::kKindEvent) ? 1U : 0U;
    stats.health += (model.back().kind == util::kKindHealth) ? 1U : 0U;
  }
  stats.accepted = count;
  return true;
}

/**
 * @brief Lists the cuts of the last kTailRecords records of a file: every byte from a record's
 *        first byte to its end.
 *
 * @param[in]     scn    the scenario (its file must parse whole)
 * @param[out]    cuts   the cuts
 * @return true: the file parses whole; false: it does not
 */
bool listCuts(const Scenario & scn, std::vector<Cut> & cuts)
{
  util::Parsed p = util::parse(scn.file.data(), scn.file.size());
  if (p.good_end != scn.file.size() || p.recs.size() < kTailRecords) {
    return false;
  }
  for (size_t k = 1; k <= kTailRecords; k++) {
    const util::Rec & r = p.recs[p.recs.size() - k];
    uint32_t number = (r.tag == util::kTagReport) ? util::readLe32(&r.body[util::kNumberAt]) : 0U;
    for (size_t at = r.offset; at <= r.offset + r.size; at++) {
      cuts.push_back(Cut{k, r.offset, r.size, r.tag, number, at});
    }
  }
  return true;
}

/**
 * @brief Reads a short text file.
 *
 * @param[in]     path  the file
 * @return its lines (empty when it cannot be read)
 */
std::vector<std::string> readLines(const std::string & path)
{
  Bytes b;
  std::vector<std::string> lines;
  if (!util::readFile(path, b)) {
    return lines;
  }
  std::string cur;
  for (uint8_t c : b) {
    if (c == '\n') {
      lines.push_back(cur.substr(0, kLineMax));
      cur.clear();
    } else {
      cur.push_back(static_cast<char>(c));
    }
  }
  return lines;
}

/**
 * @brief Cuts one scenario at every byte of its last records and logs the table.
 *
 * @param[in]     scn      the scenario
 * @param[in]     log      human log
 * @param[in]     csv      one row per cut
 * @param[in,out] total    cases run, added to
 * @param[in,out] failed   cases that failed, added to
 * @return nothing
 */
void cutScenario(const Scenario & scn, FILE * log, FILE * csv, size_t & total, size_t & failed)
{
  Plan plan{&scn, {}};
  if (!listCuts(scn, plan.cuts)) {
    (void)std::fprintf(log, "%s: the file does not parse whole; not cut\n", scn.name.c_str());
    failed++;
    return;
  }
  std::vector<int> status;
  util::runForked(plan.cuts.size(), kWorkers, runCut, &plan, status);
  std::map<std::string, size_t> classes;
  std::map<std::string, size_t> reasons;
  size_t bad = 0;
  size_t shown = 0;
  for (size_t i = 0; i < plan.cuts.size(); i++) {
    const Cut & c = plan.cuts[i];
    std::string dir = scn.dir + "/c" + std::to_string(i);
    std::vector<std::string> lines = readLines(dir + "/result.txt");
    bool clean = WIFEXITED(status[i]) && WEXITSTATUS(status[i]) == 0;
    std::string cls = lines.empty() ? "none" : lines[0];
    size_t sp = cls.find(' ');
    std::string cls_word = cls.substr(4, sp == std::string::npos ? std::string::npos : sp - 4U);
    classes[clean ? cls_word : "FAIL"]++;
    (void)std::fprintf(csv, "%s,%zu,%c,%u,%zu,%zu,%s,%s\n", scn.name.c_str(), c.from_end,
      static_cast<char>(c.rec_tag), c.rec_number, c.rec_offset, c.at - c.rec_offset,
      clean ? "ok" : "FAIL", cls.c_str());
    if (!clean) {
      bad++;
      for (size_t k = 1; k < lines.size(); k++) {
        std::string kind = lines[k].substr(0, lines[k].find(" ("));
        reasons[kind]++;
      }
      if (!WIFEXITED(status[i])) {
        reasons["STOPPED (crash or sanitizer), status " + std::to_string(status[i])]++;
      }
      if (shown < kFailShow) {
        shown++;
        (void)std::fprintf(log, "FAIL %s record %zu (tag %c, number %u) cut at +%zu: status 0x%x; "
          "first lines of %s/result.txt:\n", scn.name.c_str(), c.from_end,
          static_cast<char>(c.rec_tag), c.rec_number, c.at - c.rec_offset,
          static_cast<unsigned>(status[i]), dir.c_str());
        for (size_t k = 0; k < lines.size() && k < kStderrKeep; k++) {
          (void)std::fprintf(log, "    %s\n", lines[k].c_str());
        }
      }
    }
  }
  total += plan.cuts.size();
  failed += bad;
  (void)std::fprintf(log, "%s: %zu cuts (records from the end: 3,2,1), %zu failed; next-number "
    "classes:", scn.name.c_str(), plan.cuts.size(), bad);
  for (const auto & kv : classes) {
    (void)std::fprintf(log, " %s=%zu", kv.first.c_str(), kv.second);
  }
  (void)std::fprintf(log, "\n");
  for (const auto & kv : reasons) {
    (void)std::fprintf(log, "    failed check: %s: in %zu cases\n", kv.first.c_str(), kv.second);
  }
}

/**
 * @brief Prints what a tail looks like: for each of the last records, its tag, number and size.
 *
 * @param[in]     scn  the scenario
 * @param[in]     log  human log
 * @return nothing
 */
void describeTail(const Scenario & scn, FILE * log)
{
  util::Parsed p = util::parse(scn.file.data(), scn.file.size());
  (void)std::fprintf(log, "%s: file %zu bytes, %zu records; last records:", scn.name.c_str(),
    scn.file.size(), p.recs.size());
  for (size_t k = kTailRecords; k >= 1U && k <= p.recs.size(); k--) {
    const util::Rec & r = p.recs[p.recs.size() - k];
    uint32_t number = (r.tag == util::kTagReport) ? util::readLe32(&r.body[util::kNumberAt]) : 0U;
    (void)std::fprintf(log, " [%c n=%u %zu B]", static_cast<char>(r.tag), number, r.size);
  }
  (void)std::fprintf(log, "\n");
}

/**
 * @brief Fills a store, then cuts the scenarios made from it.
 *
 * With extra set, the fill is followed by a forced rewrite (deleteBelow) and more reports, giving
 * the scenarios S3 (the rewritten file) and S2 (reports appended after the rewrite).
 *
 * @param[in]     label          scenario name prefix and directory name
 * @param[in]     root           scratch directory
 * @param[in]     count          reports in the fill
 * @param[in]     event_percent  share of EVENT reports, per cent
 * @param[in]     extra          also make S3 and S2
 * @param[in]     log            human log
 * @param[in]     csv            one row per cut
 * @param[in,out] total          cases run, added to
 * @param[in,out] failed         cases that failed, added to
 * @return true: the fill and the cuts ran
 */
bool fillAndCut(const std::string & label, const std::string & root, size_t count,
  uint64_t event_percent, bool extra, FILE * log, FILE * csv, size_t & total, size_t & failed)
{
  auto t0 = std::chrono::steady_clock::now();
  const std::string fill_dir = root + "/" + label + "-fill";
  if (!util::resetDir(fill_dir)) {
    return false;
  }
  uint64_t rng = kSeed;
  Model model;
  FillStats stats{};
  Scenario s1{label + "-S1-after-fill", {}, {}, root + "/" + label + "-s1", true, true};
  Scenario s3{label + "-S3-compacted", {}, {}, root + "/" + label + "-s3", false, false};
  Scenario s2{label + "-S2-after-compaction", {}, {}, root + "/" + label + "-s2", false, true};
  ReportStore store(fill_dir.c_str(), config::kReportStoreBytes);
  if (!store.load() || !fill(fill_dir, count, event_percent, rng, store, model, stats)) {
    (void)std::fprintf(log, "%s: the fill failed\n", label.c_str());
    return false;
  }
  const std::string file = fill_dir + "/" + config::kReportStoreName;
  (void)util::readFile(file, s1.file);
  s1.model = model;
  double fill_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  (void)std::fprintf(log, "\n%s fill: %zu reports (%zu EVENT, %zu HEALTH) in %.1f s; the file "
    "reached %zu bytes at most, was %zu at the end of the fill; compactions seen during the fill: "
    "%zu; capacity %zu bytes\n", label.c_str(), stats.accepted, stats.events, stats.health, fill_s,
    stats.max_file, s1.file.size(), stats.compactions, config::kReportStoreBytes);
  std::vector<const Scenario *> list = {&s1};
  if (extra) {
    uint32_t cutoff = model[count / 2U].number;
    store.deleteBelow(cutoff);
    (void)util::readFile(file, s3.file);
    s3.model = model;
    bool ok = true;
    for (size_t i = 0; i < kExtraReports && ok; i++) {
      ok = feed(store, rng, static_cast<uint32_t>(count + i), event_percent, model);
    }
    (void)util::readFile(file, s2.file);
    s2.model = model;
    (void)std::fprintf(log, "%s: S3/S2 use deleteBelow(%u): a forced rewrite of the file\n",
      label.c_str(), cutoff);
    list.push_back(&s3);
    list.push_back(&s2);
  }
  for (const Scenario * scn : list) {
    describeTail(*scn, log);
    cutScenario(*scn, log, csv, total, failed);
  }
  return true;
}

}  // namespace

namespace a2
{

/**
 * @brief Runs Part A.2.
 *
 * Two fills with seeded report mixes (30 % EVENT, then 10 % EVENT); the first is also cut after a
 * forced rewrite. Every scenario is cut at every byte of its last three records; the log has a
 * summary line per scenario and every failure.
 *
 * @param[in]     workdir  scratch directory (build/); a2/ inside is emptied
 * @param[in]     log      human log
 * @param[in]     csv      one row per cut
 * @param[out]    findings failed cuts (the "failed" count of the total line); 0 when not set up
 * @return 0: the run completed (findings are in the log); 1: it could not be set up
 */
int run(const std::string & workdir, FILE * log, FILE * csv, size_t & findings)
{
  auto t0 = std::chrono::steady_clock::now();
  findings = 0;
  const std::string root = workdir + "/a2";
  if (!util::resetDir(root)) {
    return 1;
  }
  (void)std::fprintf(csv, "scenario,record_from_end,tag,number,record_offset,cut_in_record,"
    "result,class\n");
  (void)std::fprintf(log, "Next-number classes after a reload (new = number the first report "
    "stored after the reload got; highest = highest number in a surviving report record):\n"
    "  contig = new is highest + 1; gap-explained = new is past highest + 1 and the state record "
    "in force already names it (the cut report's own number is lost: its record was cut after "
    "its state record); gap = past highest + 1 with no state record naming it; REUSE = new is a "
    "number a surviving record holds (a failure).\n");
  size_t total = 0;
  size_t failed = 0;
  bool ok = fillAndCut("A", root, kFillReports, kManyEventsPercent, true, log, csv, total, failed);
  ok = ok && fillAndCut("B", root, kSecondFillReports, kFewEventsPercent, false, log, csv, total,
    failed);
  double all_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  (void)std::fprintf(log, "A.2 total: %zu cuts, %zu failed, %.1f s\n", total, failed, all_s);
  findings = failed;
  return ok ? 0 : 1;
}

}  // namespace a2
