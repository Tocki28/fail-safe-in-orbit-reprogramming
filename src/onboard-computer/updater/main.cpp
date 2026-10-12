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
#include <cstring>
#include <poll.h>

#include "clock.hpp"
#include "command_gate.hpp"
#include "config.hpp"
#include "controller_watch.hpp"
#include "data_link.hpp"
#include "image_checker.hpp"
#include "keys.h"
#include "log.hpp"
#include "options.hpp"
#include "report_store.hpp"
#include "serial_port.hpp"
#include "update_manager.hpp"
#include "updater.hpp"

namespace
{

constexpr int kExitStopped = 0;     // stopped by SIGINT or SIGTERM
constexpr int kExitUsage = 2;       // bad command line
constexpr int kExitCannotRun = 3;   // keys, data directory or socket not usable
constexpr int kExitLoopFailed = 4;  // poll() failed

volatile std::sig_atomic_t g_stop = 0;

extern "C" void onSignal(int)
{
  g_stop = 1;
}

// No SA_RESTART: poll() returns early with EINTR, and the loop sees g_stop at once.
void catchSignals()
{
  struct sigaction action{};
  action.sa_handler = onSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  (void)sigaction(SIGINT, &action, nullptr);
  (void)sigaction(SIGTERM, &action, nullptr);
}

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

  // 4. Data link A, the reports, the watch, and the wiring.
  SerialPort serial(options.device);
  DataLink link(serial);
  ReportStore reports;
  ControllerWatch watch(config::kSilenceMs, clocks::nowMs());
  Updater updater(gate, link, store, checker, reports, watch, options.beat_s * 1000U);

  logging::line("fsir-updater: ground on UDP port %u (all interfaces); data link A on %s; "
    "images in %s; keys %s; beat %s", options.udp_port, options.device, options.data_dir,
    (options.key_a_file == nullptr && options.key_b_file == nullptr) ? "built in" : "from files",
    (options.beat_s == 0) ? "off" : "on");

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
