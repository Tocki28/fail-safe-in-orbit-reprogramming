/*
 * main.cpp — runs Parts A.2, A.3 and A.4 (report numbers) of the store stress test.
 *
 * Usage: store_stress <workdir> <logdir> <commit> <command text>
 * Each part writes <logdir>/<part>.log (first line: Mac time, commit, command; then the load
 * averages) and <logdir>/<part>.csv. The updater's own log lines go to
 * <workdir>/stderr-<part>.txt.
 */
#include <cstdio>
#include <cstdlib>
#include <string>

#include "a2.hpp"
#include "a3.hpp"
#include "a4.hpp"
#include "util.hpp"

namespace
{

// Number of load averages read: 1, 5 and 15 minutes (getloadavg, man 3 getloadavg).
constexpr int kLoadSamples = 3;
// Exit code for a bad command line or a part that could not be set up.
constexpr int kUsage = 2;
// Exit code when every part ran and at least one reported a finding. Distinct from 1 (a part
// could not run) and 2 (usage); no external source, a local convention of this harness.
constexpr int kFindings = 3;

/**
 * @brief Opens a part's log and writes its first lines: time, commit, command, load averages.
 *
 * @param[in]     logdir   directory of the logs
 * @param[in]     part     "a2", "a3" or "a4"
 * @param[in]     ext      "log" or "csv"
 * @param[in]     commit   short commit hash of the repository
 * @param[in]     command  the command line
 * @return the open file; null when it could not be opened
 */
FILE * openLog(const std::string & logdir, const char * part, const char * ext,
  const std::string & commit, const std::string & command)
{
  FILE * f = std::fopen((logdir + "/" + part + "." + ext).c_str(), "w");
  if (f == nullptr || std::string(ext) == "csv") {
    return f;
  }
  double load[kLoadSamples] = {0, 0, 0};
  (void)getloadavg(load, kLoadSamples);
  (void)std::fprintf(f, "%s | %s | %s\n", util::nowText().c_str(), commit.c_str(), command.c_str());
  (void)std::fprintf(f, "uptime load averages at start: %.2f %.2f %.2f\n", load[0], load[1],
    load[2]);
  return f;
}

}  // namespace

/**
 * @brief Runs the three parts one after the other.
 *
 * @param[in]     argc  argument count, 5 expected
 * @param[in]     argv  workdir, logdir, commit, command text
 * @return 0: all parts ran and none reported a finding; kFindings (3): all parts ran and at least
 *         one finding was reported (A.2 failed cuts, A.3 failed cases, A.4 lines marked FINDING;
 *         the details are in the logs); another non-zero value: a part could not be set up
 */
int main(int argc, char ** argv)
{
  if (argc != 5) {
    (void)std::fprintf(stderr, "usage: store_stress <workdir> <logdir> <commit> <command>\n");
    return kUsage;
  }
  const std::string workdir = argv[1];
  const std::string logdir = argv[2];
  const std::string commit = argv[3];
  const std::string command = argv[4];
  (void)util::resetDir(workdir);
  int status = 0;
  size_t found[3] = {0, 0, 0};   // findings of A.2, A.3, A.4
  const char * only = std::getenv("STRESS_PARTS");   // for a partial run, e.g. "a3 a4"
  for (const char * part : {"a2", "a3", "a4"}) {
    if (only != nullptr && std::string(only).find(part) == std::string::npos) {
      continue;
    }
    FILE * log = openLog(logdir, part, "log", commit, command);
    FILE * csv = openLog(logdir, part, "csv", commit, command);
    if (log == nullptr || csv == nullptr) {
      return kUsage;
    }
    if (std::freopen((workdir + "/stderr-" + part + ".txt").c_str(), "w", stderr) == nullptr) {
      return kUsage;
    }
    std::string p = part;
    int rc = (p == "a2") ? a2::run(workdir, log, csv, found[0])
           : (p == "a3") ? a3::run(workdir, log, csv, found[1])
                         : a4::run(workdir, log, csv, found[2]);
    double load[kLoadSamples] = {0, 0, 0};
    (void)getloadavg(load, kLoadSamples);
    (void)std::fprintf(log, "uptime load averages at end: %.2f %.2f %.2f\n", load[0], load[1],
      load[2]);
    (void)std::fclose(log);
    (void)std::fclose(csv);
    status = (rc != 0) ? rc : status;
  }
  const size_t total = found[0] + found[1] + found[2];
  if (status == 0 && total > 0U) {
    (void)std::printf("store_stress: %zu findings (A.2 failed cuts %zu, A.3 failed cases %zu, "
      "A.4 FINDING lines %zu); exit status %d because a run that reports findings must not look "
      "like a pass\n", total, found[0], found[1], found[2], kFindings);
    return kFindings;
  }
  return status;
}
