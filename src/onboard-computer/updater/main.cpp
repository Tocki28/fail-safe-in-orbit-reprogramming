/*
 * main.cpp — fsir-updater: the onboard computer's part of an update, for the bench (STM32MP257F
 * board, Linux). Takes firmware from the ground over UDP, checks it, stores it, delivers it to
 * the controller over data link A, and passes commands, answers and reports between the two.
 * The parts: updater.hpp.
 *
 * One thread, one poll() loop over the UDP socket and the serial device. Why no threads:
 * stop-and-wait allows one command at a time on data link A anyway, nothing here takes long
 * enough to need its own thread (the longest pass is an image check, two Ed25519 checks over
 * at most 40 KB), and one thread has no shared data to lock.
 *
 * The 10 ms gap rule (Interfaces Sec. 6.2) needs the time each byte arrived. Linux tells only
 * when read() ran. Bytes that arrive while the loop is busy (an image check, an fsync) would
 * look as if they came late, and a good frame would be dropped. So the parser's clock is the
 * listening clock: the time the loop has spent waiting in poll(). It does not move while the
 * loop is busy, so busy time is never taken for a gap; a real gap that falls wholly in busy
 * time is missed, and the frame's check value then drops what it spoiled.
 */
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <poll.h>

#include "clock.hpp"
#include "command_gate.hpp"
#include "config.hpp"
#include "controller_watch.hpp"
#include "data_link.hpp"
#include "image_checker.hpp"
#include "image_file_check.hpp"
#include "keys.h"
#include "log.hpp"
#include "options.hpp"
#include "report_store.hpp"
#include "serial_port.hpp"
#include "update_manager.hpp"
#include "updater.hpp"

namespace
{

constexpr int kExitStopped = 0;     // stopped by SIGINT or SIGTERM; in check-only mode: image passed
constexpr int kExitImageRefused = 1;   // check-only mode: the image did not pass
constexpr int kExitUsage = 2;       // bad command line
constexpr int kExitCannotRun = 3;   // keys, data directory or socket not usable
constexpr int kExitLoopFailed = 4;  // poll() failed

volatile std::sig_atomic_t g_stop = 0;

/**
 * @brief Signal handler for SIGINT and SIGTERM: asks the main loop to stop.
 *
 * Only sets a flag, which is all a handler may safely do; the loop reads it after poll() returns.
 *
 * @param[in]     signal_number  the signal delivered (unused)
 * @return nothing
 */
extern "C" void onSignal(int signal_number)
{
  (void)signal_number;   // the same reaction for both signals
  g_stop = 1;
}

/**
 * @brief Installs the handler that stops the loop on SIGINT and SIGTERM, and ignores SIGPIPE.
 *
 * No SA_RESTART: poll() returns early with EINTR, and the loop sees g_stop at once.
 * SIGPIPE is ignored so that a closed pipe (a log reader that went away, a socket peer that
 * closed) makes write() return EPIPE, which every caller already checks, instead of killing
 * the updater in the middle of an update (audit finding #8; REQ-063 asks for an update to
 * survive what the environment does).
 *
 * @return nothing
 */
void catchSignals()
{
  struct sigaction action{};
  action.sa_handler = onSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  (void)sigaction(SIGINT, &action, nullptr);
  (void)sigaction(SIGTERM, &action, nullptr);
  struct sigaction ignore{};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  ignore.sa_flags = 0;
  (void)sigaction(SIGPIPE, &ignore, nullptr);
}

/**
 * @brief Fills a public key from a file or from the built-in key.
 *
 * Uses the built-in key when no file is named; otherwise reads the file, which must hold exactly 32 bytes.
 *
 * @param[in]    file  path of the key file, or null for the built-in key
 * @param[in]    built_in  built-in 32-byte public key
 * @param[out]   key  the 32-byte public key
 * @param[in]    name  key name used in the error message
 * @return true: key filled; false: the file could not be read as 32 bytes
 */
bool loadKey(const char * file, const uint8_t * built_in, std::array<uint8_t, 32> & key,
  const char * name)
{
  if (file == nullptr) {
    std::memcpy(key.data(), built_in, key.size());
    return true;
  }
  if (!readKeyFile(file, key.data())) {
    logging::line("cannot read public key %s from %s: it must be exactly 32 bytes", name, file);
    return false;
  }
  return true;
}

}  // namespace

/**
 * @brief Runs fsir-updater: sets up the keys, storage and links, then loops until stopped.
 *
 * One poll() loop over the UDP socket and the serial device; exit code values are the kExit constants.
 *
 * @param[in]    argc  number of command-line arguments
 * @param[in]    argv  command-line arguments
 * @return 0: stopped by SIGINT or SIGTERM (check-only mode: the image passed); 1: check-only mode, the image did not pass; 2: bad command line; 3: keys, data directory or socket not usable; 4: poll() failed
 */
int main(int argc, char ** argv)
{
  Options options{};
  if (!parseOptions(argc, argv, options)) {
    printUsage(argv[0]);
    return kExitUsage;
  }
  logging::start(clocks::nowMs(), options.verbose);
  catchSignals();

  // 1. The checking keys: built in from src/controller/common/keys.h, or from files.
  std::array<uint8_t, 32> key_a{};
  std::array<uint8_t, 32> key_b{};
  if (!loadKey(options.key_a_file, PUBLIC_KEY_A, key_a, "A") ||
      !loadKey(options.key_b_file, PUBLIC_KEY_B, key_b, "B")) {
    return kExitCannotRun;
  }
  ImageChecker checker(key_a.data(), key_b.data());
  if (!checker.ready()) {
    logging::line("OpenSSL did not accept the public keys as Ed25519 keys");
    return kExitCannotRun;
  }

  // Check-only mode (REQ-118): the checks of an upload on one file, one result line, no
  // storage, no sockets. The restore script asks it about every image before it writes any.
  if (options.check_image != nullptr) {
    uint8_t kind = 0;
    if (!parseKindName(options.check_kind, kind)) {
      printUsage(argv[0]);
      return kExitUsage;
    }
    std::array<char, 384> line{};
    bool passed = checkImageFile(options.check_image, kind, checker, line.data(), line.size());
    (void)std::printf("%s\n", line.data());
    return passed ? kExitStopped : kExitImageRefused;
  }

  // 2. Image storage.
  UploadStore store(options.data_dir);
  if (!store.ready()) {
    logging::line("cannot use data directory %s: %s", options.data_dir, std::strerror(errno));
    return kExitCannotRun;
  }

  // 3. The ground link.
  CommandGate gate(options.udp_port);
  if (!gate.open()) {
    logging::line("cannot open UDP port %u: %s", options.udp_port, std::strerror(errno));
    return kExitCannotRun;
  }

  // 3b. The restart script (decision H): --restore-script, else restore.sh next to this program.
  std::array<char, 512> default_script{};
  const char * restart_script = options.restore_script;
  if (restart_script == nullptr) {
    if (defaultRestoreScript(argv[0], default_script.data(), default_script.size())) {
      restart_script = default_script.data();
    } else {
      logging::line("restart script: the program's directory is not known (argv[0] has no slash "
        "and there is no /proc); give --restore-script PATH. Without it a trial overrun is logged "
        "and the controller is not restarted");
    }
  }

  // 4. Data link A, the reports, the watch, and the wiring.
  SerialPort serial(options.device);
  DataLink link(serial);
  ReportStore reports;
  ControllerWatch watch(config::kSilenceMs, clocks::nowMs());
  Updater updater(gate, link, store, checker, reports, watch, options.beat_s * 1000U,
    restart_script, config::kRestartScriptMs);

  logging::line("fsir-updater: ground on UDP port %u (all interfaces); data link A on %s; "
    "images in %s; keys %s; beat %s; restart script %s", options.udp_port, options.device,
    options.data_dir,
    (options.key_a_file == nullptr && options.key_b_file == nullptr) ? "built in" : "from files",
    (options.beat_s == 0) ? "off" : "on", (restart_script != nullptr) ? restart_script : "none");

  // 5. The loop.
  uint64_t listen_ms = 0;
  uint64_t next_open_ms = 0;
  int last_open_error = 0;
  int exit_code = kExitStopped;
  while (g_stop == 0) {
    uint64_t now = clocks::nowMs();
    if (!serial.isOpen() && now >= next_open_ms) {
      if (serial.open()) {
        link.resetParser();
        last_open_error = 0;
        logging::line("data link A: %s open, 115200 8N1 raw", options.device);
      } else {
        if (errno != last_open_error) {   // once per kind of failure, not every second
          logging::line("data link A: cannot open %s: %s; trying every %llu ms", options.device,
            std::strerror(errno), static_cast<unsigned long long>(config::kReopenEveryMs));
          last_open_error = errno;
        }
        next_open_ms = now + config::kReopenEveryMs;
      }
    }

    std::array<pollfd, 2> watched{};
    watched[0].fd = gate.fd();
    watched[0].events = POLLIN;
    watched[1].fd = serial.pollFd();   // -1 while closed: poll() skips it
    watched[1].events = POLLIN;
    uint64_t before = clocks::nowMs();
    int ready = poll(watched.data(), watched.size(), config::kPollMs);
    uint64_t after = clocks::nowMs();
    listen_ms += after - before;
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      logging::line("poll failed: %s", std::strerror(errno));
      exit_code = kExitLoopFailed;
      break;
    }

    if ((watched[0].revents & POLLIN) != 0) {
      updater.onGroundReadable(after);
    }
    constexpr short kLinkEvents = POLLIN | POLLERR | POLLHUP | POLLNVAL;
    if ((watched[1].revents & kLinkEvents) != 0 && !updater.onLinkReadable(listen_ms, after)) {
      logging::line("data link A: %s lost; trying to open it every %llu ms", options.device,
        static_cast<unsigned long long>(config::kReopenEveryMs));
      serial.close();
      next_open_ms = after + config::kReopenEveryMs;
    }
    updater.tick(clocks::nowMs());
  }
  logging::line("fsir-updater: stopped");
  return exit_code;
}
