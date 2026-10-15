#include "restart_runner.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" char ** environ;

namespace
{

// The reset request's contract with restore.sh (restore.sh in mode "reset" prints these lines).
constexpr const char * kResetArgument = "reset";
constexpr const char * kResultDone = "RESTORE DONE: reset";
constexpr const char * kResultAfterReset = "RESTORE FAILED: after reset";
constexpr size_t kMaxReadsPerPoll = 64;   // bounds drain(): 64 x 512 bytes is far more than one line

/**
 * @brief Tells whether text starts with a prefix and the prefix ends the word there.
 *
 * "RESTORE DONE: reset" matches "RESTORE DONE: reset" and "RESTORE DONE: reset (x)" but not
 * "RESTORE DONE: resetting": the result line is a contract with restore.sh, not a guess.
 *
 * @param[in]     text    a result line
 * @param[in]     prefix  the contract text
 * @return true: text begins with prefix, followed by the end or a space; false: otherwise
 */
bool startsWord(const char * text, const char * prefix)
{
  size_t n = std::strlen(prefix);
  if (n == 0) {
    return false;   // no prefix: no match (a request without an "after" line)
  }
  return std::strncmp(text, prefix, n) == 0 && (text[n] == '\0' || text[n] == ' ');
}

/**
 * @brief Copies a text into a fixed buffer if it fits with its zero.
 *
 * @param[out]    dst       the buffer
 * @param[in]     dst_size  its size
 * @param[in]     src       the text; null counts as empty
 * @return true: copied; false: it does not fit (dst is then empty)
 */
bool copyText(char * dst, size_t dst_size, const char * src)
{
  dst[0] = '\0';
  if (src == nullptr) {
    return true;
  }
  size_t n = ::strnlen(src, dst_size);
  if (n >= dst_size) {
    return false;
  }
  std::memcpy(dst, src, n + 1);
  return true;
}

}  // namespace

/**
 * @brief Makes a runner with no child.
 *
 * @param[in]     script         path of the restart script (restore.sh), copied; null or empty: none configured
 * @param[in]     deadline_ms    run time after which the script gets TERM, ms (config::kRestartScriptMs)
 * @param[in]     kill_grace_ms  wait after TERM, and again after KILL, ms (config::kRestartKillGraceMs)
 * @return nothing (constructor)
 */
RestartRunner::RestartRunner(const char * script, uint64_t deadline_ms, uint64_t kill_grace_ms)
: script_((script != nullptr) ? script : ""), deadline_ms_(deadline_ms), kill_grace_ms_(kill_grace_ms),
  pid_(-1), out_fd_(-1), started_ms_(0), term_ms_(0), kill_ms_(0), term_sent_(false),
  kill_sent_(false), done_prefix_{}, after_prefix_{}, output_{}, output_size_(0), output_cut_(false)
{
}

/**
 * @brief Ends a child that is still running (KILL to its group, one reap without waiting) and closes the pipe.
 *
 * A destructor must not wait; KILL cannot be ignored, and a child not yet reaped at this point
 * stays a zombie until the program ends.
 *
 * @return nothing (destructor)
 */
RestartRunner::~RestartRunner()
{
  if (pid_ > 0) {
    if (::kill(-pid_, SIGKILL) != 0) {
      (void)::kill(pid_, SIGKILL);   // the group is gone or never formed; the child itself, if any
    }
    int status = 0;
    (void)::waitpid(pid_, &status, WNOHANG);   // a non-blocking reap; the result is not needed
  }
  closePipe();
}

/**
 * @brief Tells whether a restart script is configured.
 *
 * @return true: the path is set; false: it is empty
 */
bool RestartRunner::configured() const
{
  return !script_.empty();
}

/**
 * @brief Tells whether a child has been started and not yet reaped or given up.
 *
 * @return true: a child is running; false: none
 */
bool RestartRunner::running() const
{
  return pid_ > 0;
}

/**
 * @brief Gives the configured script path.
 *
 * @return the path; "" when none is configured
 */
const char * RestartRunner::script() const
{
  return script_.c_str();
}

/**
 * @brief Closes the read end of the child's output pipe, if open.
 *
 * @return nothing
 */
void RestartRunner::closePipe()
{
  if (out_fd_ >= 0) {
    (void)::close(out_fd_);   // read end only; a failed close leaves nothing to recover
    out_fd_ = -1;
  }
}

/**
 * @brief Gives the decision-H request: "script reset" and its two result lines.
 *
 * The strings are constants that live for the whole run; the request holds pointers to them.
 *
 * @return the request for restore.sh in mode reset
 */
RestartRunner::Request RestartRunner::resetRequest()
{
  static const char * const kArgs[] = {kResetArgument, nullptr};
  return Request{kArgs, kResultDone, kResultAfterReset};
}

/**
 * @brief Starts "script reset"; the request of the trial overrun path.
 *
 * @param[in]     now_ms  current time, ms; the deadline counts from here
 * @param[out]    error   errno value of the failure when false is returned; 0 otherwise
 * @return true: the child runs; false: not configured, already running, or it could not be started
 */
bool RestartRunner::start(uint64_t now_ms, int & error)
{
  return start(resetRequest(), now_ms, error);
}

/**
 * @brief Starts "script <arguments>" as a child in its own process group; returns at once.
 *
 * posix_spawn, not fork: it reports a missing or non-executable script as an error here, and
 * leaves no half-started child. The child gets stdin from /dev/null and stdout into a pipe the
 * updater reads without waiting; stderr stays the updater's (the journal). Signal dispositions
 * go back to default (the updater ignores SIGPIPE, and an ignored signal would survive exec).
 * Its own process group lets the deadline's KILL reach OpenOCD too (decision H, accepted 13 Oct
 * 2026).
 *
 * @param[in]     request  the arguments and result lines (copied here; the caller's strings need
 *                         not outlive the call); invalid: EINVAL, nothing is started
 * @param[in]     now_ms   current time, ms; the deadline counts from here
 * @param[out]    error    errno value of the failure when false is returned; 0 otherwise
 * @return true: the child runs; false: not configured, already running, a bad request, or it
 *         could not be started
 */
bool RestartRunner::start(const Request & request, uint64_t now_ms, int & error)
{
  error = 0;
  if (!configured()) {
    error = ENOENT;
    return false;
  }
  if (running()) {
    error = EBUSY;
    return false;
  }
  // The request is validated and copied before anything is started (code rules B4). argv holds
  // the script's name, the arguments, and the end marker.
  std::array<std::array<char, kArgBytes>, kArgsMax> arg_text{};
  std::array<char *, kArgsMax + 2> argv{};
  size_t arg_count = 0;
  bool request_ok = (request.done_prefix != nullptr);
  while (request_ok && request.args != nullptr && request.args[arg_count] != nullptr) {
    request_ok = arg_count < kArgsMax &&
                 copyText(arg_text[arg_count].data(), kArgBytes, request.args[arg_count]);
    if (request_ok) {
      arg_count++;
    }
  }
  std::array<char, kPrefixBytes> done_copy{};
  std::array<char, kPrefixBytes> after_copy{};
  request_ok = request_ok && copyText(done_copy.data(), done_copy.size(), request.done_prefix) &&
               done_copy[0] != '\0' &&
               copyText(after_copy.data(), after_copy.size(), request.after_prefix);
  if (!request_ok) {
    error = EINVAL;
    return false;
  }
  argv[0] = const_cast<char *>(script_.c_str());
  for (size_t i = 0; i < arg_count; i++) {
    argv[i + 1] = arg_text[i].data();
  }
  argv[arg_count + 1] = nullptr;
  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) {
    error = errno;
    return false;
  }
  // Both ends close on exec; the child's copy of the write end is made by dup2 below, which
  // clears the flag on the new descriptor.
  bool ok = ::fcntl(fds[0], F_SETFD, FD_CLOEXEC) == 0 && ::fcntl(fds[1], F_SETFD, FD_CLOEXEC) == 0 &&
            ::fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0;
  int rc = ok ? 0 : errno;

  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attr;
  bool have_actions = false;
  bool have_attr = false;
  if (rc == 0) {
    rc = ::posix_spawn_file_actions_init(&actions);
    have_actions = (rc == 0);
  }
  if (rc == 0) {
    rc = ::posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  }
  if (rc == 0) {
    rc = ::posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
  }
  if (rc == 0) {
    rc = ::posix_spawnattr_init(&attr);
    have_attr = (rc == 0);
  }
  if (rc == 0) {
    sigset_t defaults;
    sigset_t mask;
    rc = (sigemptyset(&defaults) == 0 && sigaddset(&defaults, SIGPIPE) == 0 &&
          sigaddset(&defaults, SIGINT) == 0 && sigaddset(&defaults, SIGTERM) == 0 &&
          sigaddset(&defaults, SIGHUP) == 0 && sigemptyset(&mask) == 0) ? 0 : EINVAL;
    if (rc == 0) {
      rc = ::posix_spawnattr_setsigdefault(&attr, &defaults);
    }
    if (rc == 0) {
      rc = ::posix_spawnattr_setsigmask(&attr, &mask);
    }
    if (rc == 0) {
      rc = ::posix_spawnattr_setpgroup(&attr, 0);
    }
    if (rc == 0) {
      rc = ::posix_spawnattr_setflags(&attr,
        static_cast<short>(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK));
    }
  }
  pid_t pid = -1;
  if (rc == 0) {
    rc = ::posix_spawn(&pid, script_.c_str(), &actions, &attr, argv.data(), environ);
  }
  if (have_actions) {
    (void)::posix_spawn_file_actions_destroy(&actions);   // frees memory only; nothing to recover
  }
  if (have_attr) {
    (void)::posix_spawnattr_destroy(&attr);   // frees memory only; nothing to recover
  }
  (void)::close(fds[1]);   // the parent does not write; a failed close leaves nothing to recover
  if (rc != 0) {
    (void)::close(fds[0]);   // the child never started; nothing was read
    error = rc;
    return false;
  }
  pid_ = pid;
  done_prefix_ = done_copy;
  after_prefix_ = after_copy;
  out_fd_ = fds[0];
  started_ms_ = now_ms;
  term_ms_ = 0;
  kill_ms_ = 0;
  term_sent_ = false;
  kill_sent_ = false;
  output_size_ = 0;
  output_cut_ = false;
  output_.fill('\0');
  return true;
}

/**
 * @brief Reads what the child has printed, without waiting.
 *
 * The pipe is non-blocking; at most kMaxReadsPerPoll reads per call. When the buffer is full the
 * oldest bytes are dropped and the newest kept, so a chatty child cannot fill the pipe and stall,
 * and the last line (the result) is never the one lost.
 *
 * @return nothing
 */
void RestartRunner::drain()
{
  if (out_fd_ < 0) {
    return;
  }
  std::array<char, 256> scratch{};
  for (size_t i = 0; i < kMaxReadsPerPoll; i++) {
    ssize_t n = ::read(out_fd_, scratch.data(), scratch.size());
    if (n <= 0) {
      return;   // 0: the child closed its end; -1: nothing to read now (EAGAIN), or an error
    }
    size_t take = static_cast<size_t>(n);   // at most scratch.size(), which is below the buffer size
    size_t capacity = output_.size() - 1;   // keep a terminating zero
    if (output_size_ + take > capacity) {
      // Keep the newest bytes: the result line is the last thing the script prints.
      size_t drop = output_size_ + take - capacity;
      std::memmove(output_.data(), &output_[drop], output_size_ - drop);
      output_size_ -= drop;
      std::memset(&output_[output_size_], 0, output_.size() - output_size_);   // no stale tail
      output_cut_ = true;
    }
    std::memcpy(&output_[output_size_], scratch.data(), take);
    output_size_ += take;
  }
}

/**
 * @brief Turns the end of the child into an Outcome and returns to the idle state.
 *
 * The result line is the last non-empty line the script printed on stdout, the only stream the
 * runner captures (stderr stays the updater's journal). The line is kept in outcome.line in
 * every case. kDone needs that line to start with the request's done prefix and exit status 0;
 * kFailedAfterReset needs the request's after prefix (when it has one); every other case,
 * including no line at all, is kFailed: the step is not shown to have happened. The line is
 * not tied to any text: the prefixes are the request's, so a mode other than reset can have
 * its own. A line whose start was cut off by the bounded buffer is not taken as a line.
 *
 * @param[in]     status  the wait status from waitpid; ignored when reaped is false
 * @param[in]     reaped  true: waitpid returned the child; false: the child was given up unreaped
 * @param[out]    outcome the result; the fields set by poll (deadline_hit, killed) are kept
 * @return nothing
 */
void RestartRunner::finish(int status, bool reaped, Outcome & outcome)
{
  drain();
  outcome.exit_code = -1;
  outcome.term_signal = 0;
  if (reaped && WIFEXITED(status)) {
    outcome.exit_code = WEXITSTATUS(status);
  } else if (reaped && WIFSIGNALED(status)) {
    outcome.term_signal = WTERMSIG(status);
  }
  outcome.line.fill('\0');
  const char * last = nullptr;
  for (size_t i = 0; i < output_size_; i++) {   // bounded by the buffer
    bool line_start = (i == 0 && !output_cut_) || (i > 0 && output_[i - 1] == '\n');
    bool empty = (output_[i] == '\n' || output_[i] == '\r');   // a blank line is not a result
    if (line_start && !empty && output_[i] != '\0') {
      last = &output_[i];
    }
  }
  if (last != nullptr) {
    size_t n = 0;
    while (last[n] != '\0' && last[n] != '\n' && last[n] != '\r' && n + 1 < outcome.line.size()) {
      outcome.line[n] = last[n];
      n++;
    }
  }
  outcome.result = Result::kFailed;
  if (startsWord(outcome.line.data(), done_prefix_.data()) && outcome.exit_code == 0) {
    outcome.result = Result::kDone;
  } else if (startsWord(outcome.line.data(), after_prefix_.data())) {
    outcome.result = Result::kFailedAfterReset;
  }
  closePipe();
  pid_ = -1;
}

/**
 * @brief Looks at the running child once, without waiting: output, exit, deadline.
 *
 * Call every loop pass while running(). The child's end is taken with waitpid(WNOHANG). Past
 * the deadline: TERM to the script. After the grace: KILL to the script's process group. After a
 * further grace with the child still not reaped: the child is given up (outcome.abandoned) so
 * that the updater is never stuck behind it. Decision H, accepted by the owner, 13 Oct 2026.
 *
 * @param[in]     now_ms   current time, ms
 * @param[out]    outcome  filled when true is returned
 * @return true: the child ended or was given up (once); false: still running, or none running
 */
bool RestartRunner::poll(uint64_t now_ms, Outcome & outcome)
{
  if (!running()) {
    return false;
  }
  drain();
  int status = 0;
  pid_t r = ::waitpid(pid_, &status, WNOHANG);
  outcome.deadline_hit = term_sent_;
  outcome.killed = kill_sent_;
  outcome.abandoned = false;
  if (r == pid_) {
    finish(status, true, outcome);
    return true;
  }
  if (r < 0 && errno != EINTR) {
    // ECHILD or worse: the child cannot be waited for. It is over as far as the updater can tell.
    finish(0, false, outcome);
    return true;
  }
  if (!term_sent_ && now_ms - started_ms_ > deadline_ms_) {
    term_sent_ = true;
    term_ms_ = now_ms;
    (void)::kill(pid_, SIGTERM);   // a script that is gone already is reaped on the next pass
  } else if (term_sent_ && !kill_sent_ && now_ms - term_ms_ > kill_grace_ms_) {
    kill_sent_ = true;
    kill_ms_ = now_ms;
    if (::kill(-pid_, SIGKILL) != 0) {
      (void)::kill(pid_, SIGKILL);   // the group is gone; the child itself
    }
  } else if (kill_sent_ && now_ms - kill_ms_ > kill_grace_ms_) {
    outcome.deadline_hit = true;
    outcome.killed = true;
    outcome.abandoned = true;
    finish(0, false, outcome);
    return true;
  }
  return false;
}
