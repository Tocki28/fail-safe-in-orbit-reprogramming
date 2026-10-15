/*
 * restart_runner.hpp — runs the controller restart script as a child process without ever
 * blocking the updater's loop (decision H, accepted by the owner, 13 Oct 2026; Interfaces
 * Sec. 5.4 event 0x0106; REQ-013, REQ-061, REQ-124).
 *
 * What is run is a request: the script's argument list and the result line prefixes that count
 * as "done" and "failed after the reset step". The decision-H request is resetRequest(), the
 * argument "reset" with the lines below; start(now_ms, error) without a request runs it, so
 * the overrun path is unchanged. Another mode (restore.sh full ...) is another request, run by
 * the same runner with the same time limits, output handling and process-group kill.
 *
 * The script is restore.sh in mode "reset" (src/onboard-computer/restore/restore.sh): OpenOCD
 * "init; reset run; shutdown" over SWD under the restore script's own reset configuration and
 * time limit. It prints one result line on stdout:
 *   RESTORE DONE: reset                 the controller was reset and runs
 *   RESTORE FAILED: after reset (...)   the reset ran, then OpenOCD did not end cleanly
 *   RESTORE FAILED: <step> (...)        the reset is not shown to have happened
 * The result line is the last non-empty line on stdout, whatever its text; it is judged only
 * against the request's prefixes. stderr is not captured (it stays the updater's journal).
 * "The reset happened" is therefore true for the first two lines only. A run that ended with no
 * line (killed, crashed, script not found) is "not shown to have happened": the updater sends
 * nothing to the ground that it cannot back.
 *
 * Why a child process and a poll, not a call that waits: one loop serves the ground link and
 * data link A (main.cpp), and a reset can take as long as OpenOCD's time limit. start() forks
 * and returns at once; poll() is called every loop pass, reads what the child printed without
 * waiting, reaps it with WNOHANG, and enforces the deadline: after the deadline TERM to the
 * script (it stops OpenOCD and prints its own line), after the grace KILL to the script's whole
 * process group (the script is started as its own group leader, so OpenOCD dies with it), after
 * another grace the child is given up (a zombie is left; nothing waits for it).
 */
#ifndef FSIR_UPDATER_RESTART_RUNNER_HPP
#define FSIR_UPDATER_RESTART_RUNNER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>

class RestartRunner
{
public:
  enum class Result : uint8_t
  {
    kDone,               // last line starts with done_prefix, exit status 0
    kFailedAfterReset,   // last line starts with after_prefix: the step ran, then failed
    kFailed,             // anything else: the reset is not shown to have happened
  };

  // The most arguments after the script's own name, and the longest one with its zero. Bench
  // choices (TBR, awaiting the owner's acceptance): "reset" is one argument of 6 bytes; a full
  // run names a mode, an image and a few options; 8 x 128 bytes is room for that and for no more
  // than the fixed buffers hold.
  static constexpr size_t kArgsMax = 8;
  static constexpr size_t kArgBytes = 128;
  // The longest result prefix with its zero. "RESTORE FAILED: after reset" is 27 bytes; the
  // output line is cut at 160 (Outcome::line), so a prefix of 64 leaves room for details.
  static constexpr size_t kPrefixBytes = 64;

  // What to run: the script's arguments and the result lines it prints. Copied by start().
  struct Request
  {
    const char * const * args;   // arguments after the script's name, ended by a null pointer;
                                 // null: none. At most kArgsMax, each under kArgBytes
    const char * done_prefix;    // result line meaning "done" (with exit status 0); not empty
    const char * after_prefix;   // result line meaning "failed after the step ran"; null: none
  };

  struct Outcome
  {
    Result result;
    bool deadline_hit;   // the updater's deadline passed and TERM was sent
    bool killed;         // and KILL was sent after the grace
    bool abandoned;      // and the child still was not reaped: given up
    int exit_code;       // exit status of the script; -1 if it did not exit (signal, abandoned)
    int term_signal;     // signal that ended the script; 0 if it exited
    std::array<char, 160> line;   // the result line, cut to size; empty if there was none
  };

  RestartRunner(const char * script, uint64_t deadline_ms, uint64_t kill_grace_ms);
  ~RestartRunner();
  RestartRunner(const RestartRunner &) = delete;
  RestartRunner & operator=(const RestartRunner &) = delete;

  // A script path is set (not null, not empty).
  bool configured() const;

  // A child is running (started, not yet reaped or given up).
  bool running() const;

  // The script path; "" when none is configured.
  const char * script() const;

  // The decision-H request: argument "reset", result lines "RESTORE DONE: reset" and
  // "RESTORE FAILED: after reset".
  static Request resetRequest();

  // Starts "script reset" (resetRequest()). False if not configured, already running, or the
  // child could not be started; error then holds the errno value.
  [[nodiscard]] bool start(uint64_t now_ms, int & error);

  // Starts "script <request.args>" with the request's result lines. As above, and EINVAL when
  // the request does not fit the fixed buffers or has no done_prefix.
  [[nodiscard]] bool start(const Request & request, uint64_t now_ms, int & error);

  // Every loop pass while running(): reads output, reaps, enforces the deadline. True once, when
  // the child has ended or been given up; outcome is then filled and running() is false.
  [[nodiscard]] bool poll(uint64_t now_ms, Outcome & outcome);

private:
  void drain();
  void finish(int status, bool reaped, Outcome & outcome);
  void closePipe();

  std::string script_;   // a copy: the caller's text need not outlive the runner
  uint64_t deadline_ms_;
  uint64_t kill_grace_ms_;
  pid_t pid_;
  int out_fd_;
  uint64_t started_ms_;
  uint64_t term_ms_;
  uint64_t kill_ms_;
  bool term_sent_;
  bool kill_sent_;
  std::array<char, kPrefixBytes> done_prefix_;    // of the request now running
  std::array<char, kPrefixBytes> after_prefix_;   // empty: the request has none
  std::array<char, 512> output_;   // the last bytes the child printed
  size_t output_size_;
  bool output_cut_;   // older bytes were dropped: output_[0] may be the middle of a line
};

#endif  // FSIR_UPDATER_RESTART_RUNNER_HPP
