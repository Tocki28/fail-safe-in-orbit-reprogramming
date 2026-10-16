/*
 * a3.cpp — Part A.3: for each value of the clock-offset and limits records, with an old record
 * and with none, a child process writes the new value and ends at step N of the write's
 * file-system calls (before it, after it, or after half of a write's bytes); the parent reloads
 * with a fresh DurableRecord and checks the result is the old value or the new value, then
 * checks a following write succeeds. The host file system keeps a rename that the directory
 * fsync has not yet covered, so the lost rename is modelled: the directory as it was before the
 * rename (found by a child cut just before the rename) replaces the directory before the reload.
 */
#include "a3.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <sys/wait.h>
#include <unistd.h>

#include "clock_record.hpp"
#include "config.hpp"
#include "durable_record.hpp"
#include "limits_record.hpp"
#include "shim.hpp"
#include "util.hpp"

namespace
{

// Which record a case writes.
enum class Kind : uint8_t
{
  kClock,
  kLimits,
};

// What a reload found, whichever record it is.
struct Seen
{
  int source;       // 0 stored, 1 no record, 2 damaged, 3 unreadable (the two Source enums' order)
  int64_t offset;   // clock
  uint32_t min_version;   // limits
  uint16_t identity;      // limits
};

// A value to write.
struct Value
{
  int64_t offset;
  uint32_t min_version;
  uint16_t identity;
  const char * label;
};

// Clock offsets of the task brief: 0, -1, 1, INT64_MIN, INT64_MAX.
constexpr std::array<Value, 5> kClockValues = {{
  {0, 0U, 0U, "0"},
  {-1, 0U, 0U, "-1"},
  {1, 0U, 0U, "1"},
  {std::numeric_limits<int64_t>::min(), 0U, 0U, "INT64_MIN"},
  {std::numeric_limits<int64_t>::max(), 0U, 0U, "INT64_MAX"},
}};
// Limits of the task brief: (1, 0x0431), (0xFFFFFFFF, 0xFFFF), (0, 0).
constexpr std::array<Value, 3> kLimitValues = {{
  {0, 1U, 0x0431U, "(1,0x0431)"},
  {0, 0xFFFFFFFFU, 0xFFFFU, "(0xFFFFFFFF,0xFFFF)"},
  {0, 0U, 0U, "(0,0)"},
}};
// The record that exists before the write, and the value of the follow-up write: chosen to differ
// from every test value and from each other (test choice).
constexpr Value kOldClock = {0x1122334455667788LL, 0U, 0U, "old"};
constexpr Value kOldLimits = {0, 7U, 0x0777U, "old"};
constexpr Value kThirdClock = {0x0A0B0C0D0E0F1011LL, 0U, 0U, "third"};
constexpr Value kThirdLimits = {0, 3U, 0x0303U, "third"};

// Positions of a cut relative to the calls: call s "before" is position 2s - 1, "after" 2s.
constexpr size_t kPositionsPerStep = 2;

// One row of the result table.
struct Row
{
  const char * kind;
  const char * value;
  bool old_present;
  size_t step;
  std::string call;
  std::string variant;
  std::string result;
  bool ok;
};

/**
 * @brief Writes a value as a record of the kind.
 *
 * @param[in]     kind   clock or limits
 * @param[in]     v      the value
 * @param[in]     dir    the data directory
 * @param[out]    error  errno value when false is returned
 * @return true: written and flushed; false: not
 */
bool writeValue(Kind kind, const Value & v, const std::string & dir, int & error)
{
  if (kind == Kind::kClock) {
    DurableRecord rec(dir.c_str(), config::kClockOffsetRecordName,
      config::kClockOffsetRecordBytes);
    return clock_record::save(rec, v.offset, error);
  }
  DurableRecord rec(dir.c_str(), config::kLimitsRecordName, config::kLimitsRecordBytes);
  ImageLimits lim{v.min_version, v.identity, true};
  return limits_record::save(rec, lim, error);
}

/**
 * @brief Reloads a record with a fresh DurableRecord.
 *
 * @param[in]     kind  clock or limits
 * @param[in]     dir   the data directory
 * @return what was found
 */
Seen readValue(Kind kind, const std::string & dir)
{
  Seen s{};
  int error = 0;
  if (kind == Kind::kClock) {
    DurableRecord rec(dir.c_str(), config::kClockOffsetRecordName,
      config::kClockOffsetRecordBytes);
    s.source = static_cast<int>(clock_record::load(rec, s.offset, error));
    return s;
  }
  DurableRecord rec(dir.c_str(), config::kLimitsRecordName, config::kLimitsRecordBytes);
  ImageLimits lim{};
  s.source = static_cast<int>(limits_record::load(rec, lim, error));
  s.min_version = lim.minimum_version;
  s.identity = lim.identity;
  return s;
}

/**
 * @brief Tells whether a reload found a stored record holding a value.
 *
 * @param[in]     kind  clock or limits
 * @param[in]     s     what was found
 * @param[in]     v     the value
 * @return true: stored, and equal to v
 */
bool holds(Kind kind, const Seen & s, const Value & v)
{
  if (s.source != 0) {
    return false;
  }
  return kind == Kind::kClock ? s.offset == v.offset
                              : (s.min_version == v.min_version && s.identity == v.identity);
}

/**
 * @brief Text of what a reload found, for the table.
 *
 * @param[in]     kind  clock or limits
 * @param[in]     s     what was found
 * @return for example "stored 5" or "no record"
 */
std::string describe(Kind kind, const Seen & s)
{
  static const std::array<const char *, 4> names = {"stored", "no record", "damaged", "unreadable"};
  std::string t = (s.source >= 0 && s.source < 4) ? names[static_cast<size_t>(s.source)] : "?";
  if (s.source == 0) {
    t += kind == Kind::kClock ? " " + std::to_string(s.offset)
                              : " " + std::to_string(s.min_version) + "/" +
                                std::to_string(s.identity);
  }
  return t;
}

/**
 * @brief Runs the write in a child that ends at the given step.
 *
 * @param[in]     kind  clock or limits
 * @param[in]     v     the value to write
 * @param[in]     dir   the data directory
 * @param[in]     step  call number to cut at
 * @param[in]     when  before, after, or half a write
 * @return true: the child ended at the cut (exit status kCutExit); false: it did not
 */
bool cutChild(Kind kind, const Value & v, const std::string & dir, size_t step, shim::When when)
{
  pid_t pid = fork();
  if (pid == 0) {
    shim::startCut(step, when);
    int error = 0;
    (void)writeValue(kind, v, dir, error);
    _exit(0);   // reached only when the write never made call number "step"
  }
  int st = 0;
  (void)waitpid(pid, &st, 0);
  return WIFEXITED(st) && WEXITSTATUS(st) == shim::kCutExit;
}

// The calls of one good write, noted by the shim.
struct Calls
{
  size_t count;
  size_t rename_step;   // 1-based; 0: none
  size_t dsync_step;    // the first fsync after the rename; 0: none
  std::array<shim::Call, shim::kCallsMax> note;
};

/**
 * @brief Runs one good write with the shim noting its calls.
 *
 * @param[in]     kind  clock or limits
 * @param[in]     v     the value
 * @param[in]     dir   the data directory
 * @param[out]    calls the calls, and where the rename and the directory fsync are
 * @return true: the write succeeded
 */
bool recordCalls(Kind kind, const Value & v, const std::string & dir, Calls & calls)
{
  int error = 0;
  shim::startRecording();
  bool ok = writeValue(kind, v, dir, error);
  calls = Calls{};
  calls.count = shim::callCount();
  shim::off();
  for (size_t i = 0; i < calls.count && i < shim::kCallsMax; i++) {
    calls.note[i] = shim::callAt(i);
    std::string name = calls.note[i].name;
    if (name == "rename" && calls.rename_step == 0U) {
      calls.rename_step = i + 1U;
    } else if (name == "fsync" && calls.rename_step != 0U && calls.dsync_step == 0U) {
      calls.dsync_step = i + 1U;
    }
  }
  return ok;
}

/**
 * @brief Last component of a path.
 *
 * @param[in]     path  a path
 * @return the part after the last '/'
 */
std::string baseName(const char * path)
{
  std::string p = path;
  size_t at = p.rfind('/');
  return at == std::string::npos ? p : p.substr(at + 1U);
}

/**
 * @brief Judges a reload and a follow-up write; fills the row's result and verdict.
 *
 * Allowed: the old value, or the new one; with no old record also "no record" (the code's own
 * statement: durable_record.hpp, "RECORD_ABSENT when there is no file (first start, or never
 * written)"). Then a write of a third value must succeed and read back.
 *
 * @param[in]     kind         clock or limits
 * @param[in]     old_present  a record existed before the write
 * @param[in]     old_value    the old value
 * @param[in]     new_value    the new value
 * @param[in]     third        the follow-up value
 * @param[in]     dir          the data directory after the cut
 * @param[in,out] row          receives result and ok
 * @return nothing
 */
void judge(Kind kind, bool old_present, const Value & old_value, const Value & new_value,
  const Value & third, const std::string & dir, Row & row)
{
  Seen s = readValue(kind, dir);
  if (holds(kind, s, new_value)) {
    row.result = "new";
    row.ok = true;
  } else if (old_present && holds(kind, s, old_value)) {
    row.result = "old";
    row.ok = true;
  } else if (!old_present && s.source == 1) {
    row.result = "old (no record)";
    row.ok = true;
  } else {
    row.result = "BAD: " + describe(kind, s);
    row.ok = false;
  }
  int error = 0;
  bool wrote = writeValue(kind, third, dir, error);
  Seen after = readValue(kind, dir);
  if (!wrote || !holds(kind, after, third)) {
    row.result += "; FOLLOW-UP WRITE FAILED (errno " + std::to_string(error) + ", then " +
                  describe(kind, after) + ")";
    row.ok = false;
  }
}

/**
 * @brief Runs every cut of one write (one kind, value and old state) and adds rows.
 *
 * @param[in]     kind         clock or limits
 * @param[in]     kind_name    "clock" or "limits"
 * @param[in]     v            the value to write
 * @param[in]     old_present  whether an old record exists
 * @param[in]     root         scratch directory
 * @param[in,out] rows         receives the rows
 * @param[out]    calls        the calls of a good write (for the log)
 * @return true: the write could be traced and the cuts ran
 */
bool runValue(Kind kind, const char * kind_name, const Value & v, bool old_present,
  const std::string & root, std::vector<Row> & rows, Calls & calls)
{
  const Value & old_value = kind == Kind::kClock ? kOldClock : kOldLimits;
  const Value & third = kind == Kind::kClock ? kThirdClock : kThirdLimits;
  const std::string base = root + "/base";
  const std::string work = root + "/work";
  const std::string snap = root + "/snap";
  int error = 0;
  if (!util::resetDir(base) || (old_present && !writeValue(kind, old_value, base, error))) {
    return false;
  }
  if (!util::copyDir(base, work) || !recordCalls(kind, v, work, calls) || calls.rename_step == 0U ||
      calls.dsync_step == 0U) {
    return false;
  }
  if (!util::copyDir(base, snap) || !cutChild(kind, v, snap, calls.rename_step,
        shim::When::kBefore)) {
    return false;   // snap now holds the directory as it was just before the rename
  }
  const size_t lost_from = kPositionsPerStep * calls.rename_step;
  // exclusive: after dsync is durable
  const size_t lost_to = kPositionsPerStep * calls.dsync_step;
  for (size_t step = 1; step <= calls.count; step++) {
    const shim::Call & c = calls.note[step - 1U];
    std::vector<shim::When> whens = {shim::When::kBefore, shim::When::kAfter};
    if (std::string(c.name) == "write" && c.bytes > 1U) {
      whens.push_back(shim::When::kHalf);
    }
    for (shim::When when : whens) {
      size_t pos = kPositionsPerStep * step - (when == shim::When::kAfter ? 0U : 1U);
      bool can_lose = pos >= lost_from && pos < lost_to;
      for (int lost = 0; lost <= (can_lose ? 1 : 0); lost++) {
        if (!util::copyDir(base, work) || !cutChild(kind, v, work, step, when)) {
          return false;
        }
        if (lost != 0 && !util::copyDir(snap, work)) {
          return false;
        }
        Row row{kind_name, v.label, old_present, step,
          std::string(c.name) + "(" + baseName(c.path) + ")",
          std::string(when == shim::When::kBefore ? "before" :
                      when == shim::When::kAfter ? "after"
                          : "half") + (lost != 0 ? "+lost-rename" : ""),
          "", false};
        judge(kind, old_present, old_value, v, third, work, row);
        rows.push_back(row);
      }
    }
  }
  return true;
}

}  // namespace

namespace a3
{

/**
 * @brief Runs Part A.3 and writes the call list, the table and every failure.
 *
 * @param[in]     workdir  scratch directory (build/); a3/ inside is emptied
 * @param[in]     log      human log: calls, matrix, failures
 * @param[in]     csv      every row: kind,value,old,step,call,variant,result,verdict
 * @param[out]    findings failed cases (the "failed" count of the total line); 0 when not set up
 * @return 0: the run completed (findings are in the log); 1: it could not be set up
 */
int run(const std::string & workdir, FILE * log, FILE * csv, size_t & findings)
{
  findings = 0;
  const std::string root = workdir + "/a3";
  if (!util::resetDir(root)) {
    return 1;
  }
  (void)std::fprintf(csv, "write,value,old_record,step,call,variant,result,verdict\n");
  std::vector<Row> rows;
  Calls calls{};
  struct Group
  {
    Kind kind;
    const char * name;
    const Value * values;
    size_t count;
  };
  const std::array<Group, 2> groups = {{
    {Kind::kClock, "clock_record::save", kClockValues.data(), kClockValues.size()},
    {Kind::kLimits, "limits_record::save", kLimitValues.data(), kLimitValues.size()},
  }};
  for (const Group & g : groups) {
    bool printed = false;
    for (size_t i = 0; i < g.count; i++) {
      for (int present = 1; present >= 0; present--) {
        if (!runValue(g.kind, g.name, g.values[i], present != 0, root, rows, calls)) {
          (void)std::fprintf(log, "%s %s: could not be traced or cut\n", g.name, g.values[i].label);
          return 1;
        }
        if (!printed) {
          printed = true;
          (void)std::fprintf(log, "%s: file-system calls of one good write:\n", g.name);
          for (size_t s = 0; s < calls.count; s++) {
            (void)std::fprintf(log, "  step %zu  %s(%s)%s\n", s + 1U, calls.note[s].name,
              baseName(calls.note[s].path).c_str(),
              s + 1U == calls.rename_step ? "   <- rename"
                  : (s + 1U == calls.dsync_step ? "   <- directory fsync" : ""));
          }
        }
      }
    }
  }
  size_t bad = 0;
  // "write step call variant" -> result -> n
  std::map<std::string, std::map<std::string, size_t>> matrix;
  for (const Row & r : rows) {
    (void)std::fprintf(csv, "%s,%s,%s,%zu,%s,%s,%s,%s\n", r.kind, r.value,
      r.old_present ? "present" : "absent", r.step, r.call.c_str(), r.variant.c_str(),
      r.result.c_str(), r.ok ? "ok" : "FAIL");
    std::string key = std::string(r.kind) + " | step " + std::to_string(r.step) + " " + r.call +
                      " | " + r.variant;
    matrix[key][r.ok ? r.result : "FAIL"]++;
    if (!r.ok) {
      bad++;
      (void)std::fprintf(log, "FAIL %s value %s old %s step %zu %s %s: %s\n", r.kind, r.value,
        r.old_present ? "present" : "absent", r.step, r.call.c_str(), r.variant.c_str(),
        r.result.c_str());
    }
  }
  (void)std::fprintf(log,
                     "\nwrite | step call | variant : result counts over all values and both old "
                     "states\n");
  for (const auto & kv : matrix) {
    (void)std::fprintf(log, "%s :", kv.first.c_str());
    for (const auto & rc : kv.second) {
      (void)std::fprintf(log, " %s=%zu", rc.first.c_str(), rc.second);
    }
    (void)std::fprintf(log, "\n");
  }
  (void)std::fprintf(log, "A.3 total: %zu cases, %zu failed (full table in the csv)\n", rows.size(),
    bad);
  findings = bad;
  return 0;
}

}  // namespace a3
