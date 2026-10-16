/*
 * boundaries.cpp — boundary and wrap cases (Part A.4) and repeat-memory eviction cases (Part A.6)
 * of the stress audit of the onboard computer's updater.
 *
 * Every case drives the real objects through their public interface (Updater::onGroundReadable,
 * onLinkReadable, tick; CommandGate over loopback UDP; DataLink over a fake transport; the
 * public classes TrialWatch, ControllerWatch and ForwardTable directly) with a controlled clock.
 * Each case runs in its own child process, so an abort by the undefined-behaviour sanitizer
 * (E5) ends that case only and is reported as a result.
 *
 * Usage: boundaries SCRATCH_DIR LOG_DIR OUT_DIR [case ...]
 */
#include <csignal>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <string>
#include <vector>

#include "cases_a4.hpp"
#include "cases_a6.hpp"
#include "common.hpp"
#include "controller_watch.hpp"
#include "frame.hpp"
#include "trial_watch.hpp"

namespace stress
{

namespace
{

// A case that does not end in this many seconds is killed by SIGALRM (code rule B2). Test
// choice: the longest case (the 1,000-pair eviction runs) takes well under two minutes.
constexpr unsigned kCaseSecondsMax = 900;

// Exit status of a case that ran to its end.
constexpr int kCaseEnded = 0;

// One case: its name, the function, and whether it needs the wrap build of data link A.
struct CaseEntry
{
  const char * name;
  void (*run)();
  bool wrap_build;
};

const std::array<CaseEntry, 28> kCases = {{
  {"seq-used-wrap", caseSeqUsedWrap, false},
  {"seq-used-ring-wrap", caseSeqUsedRingWrap, false},
  {"seq-repeat-wrap", caseSeqRepeatWrap, false},
  {"seq-forward-table", caseSeqForwardTable, false},
  {"seq-link-wrap", caseSeqLinkWrap, true},
  {"clock-offset-max", caseClockOffsetMax, false},
  {"clock-offset-min", caseClockOffsetMin, false},
  {"clock-offset-min-overflow", caseClockOffsetMinOverflow, false},
  {"clock-set-time-0", caseClockSetTimeZero, false},
  {"clock-set-time-max", caseClockSetTimeMax, false},
  {"expiry-boundary", caseExpiryBoundary, false},
  {"expiry-boundary-offset", caseExpiryBoundaryOffset, false},
  {"expiry-exempt", caseExpiryExempt, false},
  {"expiry-resend", caseExpiryResend, false},
  {"arm-onboard-30s", caseArmOnboard, false},
  {"arm-controller-arrival-30s", caseArmControllerArrival, false},
  {"arm-controller-queue-30s", caseArmControllerQueue, false},
  {"trial-watch-150s", caseTrialWatch, false},
  {"silence-5s", caseSilence, false},
  {"reset-gap-60s", caseResetGap, false},
  {"a6-arm-fillers", caseA6ArmFillers, false},
  {"a6-pair-fillers", caseA6PairFillers, false},
  {"a6-other-fillers", caseA6OtherFillers, false},
  {"a6-restart", caseA6Restart, false},
  {"a6-onboard-subjects", caseA6OnboardSubjects, false},
  {"a6-unanswered", caseA6Unanswered, false},
  {"a6-interplay", caseA6Interplay, false},
  {"a6-repeat-eviction", caseA6RepeatEviction, false},
}};

/**
 * @brief Runs one case in a child process and reports how it ended.
 *
 * The child sends its stderr (the updater's log) to LOG_DIR/<name>.log, sets an alarm of
 * kCaseSecondsMax, runs the case and exits. The parent waits. A child that dies on a signal
 * (a sanitizer abort, SIGALRM) or exits with a status other than 0 gets one line, with the
 * first sanitizer line of its log, and its log is copied to OUT_DIR.
 *
 * @param[in]     entry    the case
 * @param[in]     log_dir  where the child's log goes
 * @param[in]     out_dir  where the log of a case that died is kept
 * @return true: the case ran to its end
 */
bool runIsolated(const CaseEntry & entry, const std::string & log_dir, const std::string & out_dir)
{
  const std::string log_path = log_dir + "/" + entry.name + ".log";
  (void)std::fflush(stdout);
  const pid_t pid = ::fork();
  if (pid < 0) {
    std::printf("SETUP    fork failed for %s\n", entry.name);
    return false;
  }
  if (pid == 0) {
    g_log_path = log_path;
    if (std::freopen(log_path.c_str(), "w", stderr) == nullptr) {
      std::_Exit(2);
    }
    // stderr is unbuffered on a terminal but not after freopen to a file: the log lines must
    // reach the file at once, for logCount() and for a case that is killed.
    (void)std::setvbuf(stderr, nullptr, _IONBF, 0);
    (void)::alarm(kCaseSecondsMax);
    entry.run();
    (void)std::fflush(stdout);
    std::_Exit(kCaseEnded);
  }
  int status = 0;
  if (::waitpid(pid, &status, 0) != pid) {
    std::printf("SETUP    waitpid failed for %s\n", entry.name);
    return false;
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == kCaseEnded) {
    return true;
  }
  std::string first_ubsan = "(none in the log)";
  std::FILE * f = std::fopen(log_path.c_str(), "r");
  if (f != nullptr) {
    std::array<char, 600> line{};
    while (std::fgets(line.data(), static_cast<int>(line.size()), f) != nullptr) {
      if (std::strstr(line.data(), "runtime error") != nullptr) {
        first_ubsan = line.data();
        while (!first_ubsan.empty() && first_ubsan.back() == '\n') {
          first_ubsan.pop_back();
        }
        break;
      }
    }
    (void)std::fclose(f);
  }
  const std::string how = WIFSIGNALED(status) ? fmt("killed by signal %d", WTERMSIG(status))
                                              : fmt("exit status %d", WEXITSTATUS(status));
  std::printf("DIED     %s | the case ended abnormally: %s | first sanitizer line: %s\n",
    entry.name, how.c_str(), first_ubsan.c_str());
  (void)std::fflush(stdout);
  std::error_code ec;
  std::filesystem::copy_file(log_path, out_dir + "/" + entry.name + ".updater.log",
    std::filesystem::copy_options::overwrite_existing, ec);
  return false;
}

}  // namespace

}  // namespace stress

/**
 * @brief Runs the named cases, or all cases that fit this build.
 *
 * This binary is built twice: with the real data_link.cpp (STRESS_WRAP_BUILD undefined) and with
 * a copy whose first sequence number is set at build time (STRESS_WRAP_BUILD defined). Each
 * build runs only the cases made for it.
 *
 * @param[in]     argc  argument count
 * @param[in]     argv  SCRATCH_DIR LOG_DIR OUT_DIR [case ...]
 * @return 0 when every case ran to its end; 1 otherwise; 2 for a bad command line
 */
int main(int argc, char ** argv)
{
  constexpr int kFixedArguments = 4;
  if (argc < kFixedArguments) {
    std::fprintf(stderr, "usage: %s SCRATCH_DIR LOG_DIR OUT_DIR [case ...]\n", argv[0]);
    return 2;
  }
#ifdef STRESS_LINK_START
  const bool wrap_build = true;
#else
  const bool wrap_build = false;
#endif
  stress::g_scratch = argv[1];
  const std::string log_dir = argv[2];
  const std::string out_dir = argv[3];
  std::error_code ec;
  std::filesystem::create_directories(stress::g_scratch, ec);
  std::filesystem::create_directories(log_dir, ec);
  std::vector<std::string> wanted;
  for (int i = kFixedArguments; i < argc; i++) {
    wanted.emplace_back(argv[i]);
  }
  bool all_ended = true;
  for (const stress::CaseEntry & entry : stress::kCases) {
    if (entry.wrap_build != wrap_build) {
      continue;
    }
    bool chosen = wanted.empty();
    for (const std::string & w : wanted) {
      chosen = chosen || (w == entry.name);
    }
    if (chosen) {
      all_ended = stress::runIsolated(entry, log_dir, out_dir) && all_ended;
    }
  }
  return all_ended ? 0 : 1;
}
