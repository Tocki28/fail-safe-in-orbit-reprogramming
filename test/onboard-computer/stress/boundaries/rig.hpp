/*
 * rig.hpp — an Updater on a fake data link A and a loopback UDP port, with a fake controller
 * that counts how often each command is passed on to it (stress audit Parts A.4 and A.6).
 *
 * The test plays the ground over UDP, the controller through the fake transport, and the clock
 * (the monotonic time handed to the Updater, and the system clock through stress_clock.hpp).
 * Everything goes through the Updater's public interface: onGroundReadable, onLinkReadable, tick.
 * The same wiring as UpdaterRig in test/onboard-computer/unit/test_updater.cpp:1141-1360, written
 * again here because that file may not be changed.
 */
#ifndef FSIR_STRESS_BOUNDARIES_RIG_HPP
#define FSIR_STRESS_BOUNDARIES_RIG_HPP

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "bytes.hpp"
#include "command_gate.hpp"
#include "config.hpp"
#include "controller_watch.hpp"
#include "data_link.hpp"
#include "frame.hpp"
#include "image_checker.hpp"
#include "keys.h"
#include "protocol.hpp"
#include "report_store.hpp"
#include "transport.hpp"
#include "update_manager.hpp"
#include "updater.hpp"

namespace stress
{

// UDP port of the ground link in this audit. Test choice: the unit tests use 47931
// (test_updater.cpp:1149), the bench uses 47001 (config.hpp:22); this one is neither.
constexpr uint16_t kPort = 47983;

// Monotonic time the rig starts at, ms. Same start as the unit tests' rig (test_updater.cpp:1175).
constexpr uint64_t kRigStartMs = 1000;

// Time added to the parser's clock for each frame from the controller, ms. The parser measures a
// 10 ms gap inside a frame (config::kFrameGapMaxMs); every frame here arrives in one piece, so
// any step larger than that keeps one frame from joining the next (test_updater.cpp:1241).
constexpr uint64_t kListenStepMs = 100;

// Upper bound of ticks in one pump() call (code rule B2). An ARM/command pair needs 2 sends and
// 2 answers; a full queue (config::kForwardQueueSize = 8) of pairs needs 16 of each.
constexpr size_t kPumpPassesMax = 64;

// Longest wait for a loopback datagram to reach the gate, ms. Loopback delivery is immediate;
// 1000 ms is the unit tests' bound (test_updater.cpp:1359).
constexpr int kGroundWaitMs = 1000;

// How long answers() waits for the next datagram, ms. The unit tests wait 20 ms in the same
// place (test_updater.cpp:1340).
constexpr int kAnswerQuietMs = 20;

// Receive buffer asked for the test's own UDP socket, bytes. Test choice: room for the answers of
// a few hundred commands between two drains; a refused setting only makes the buffer smaller.
constexpr int kClientBufferBytes = 4 * 1024 * 1024;

// A frame the updater sent on data link A, taken apart.
struct LinkFrame
{
  uint8_t type;
  uint8_t destination;
  uint32_t sequence;
  uint32_t expiry;
  std::vector<uint8_t> payload;
};

// An answer the updater sent to the ground.
struct GroundAnswer
{
  uint32_t answered;
  uint8_t kind;
  uint8_t code;
};

// Data link A's device as the test sees it: keeps what the updater sends, hands back what the
// test puts in.
class FakeTransport final : public Transport
{
public:
  std::vector<uint8_t> sent;
  std::vector<uint8_t> incoming;

  /**
   * @brief Keeps the bytes the updater sends.
   *
   * @param[in]     data  the bytes
   * @param[in]     size  number of bytes
   * @return true, always
   */
  bool sendBytes(const uint8_t * data, size_t size) override
  {
    sent.insert(sent.end(), data, data + size);
    return true;
  }

  /**
   * @brief Hands the updater the bytes the test put in incoming.
   *
   * @param[out]    buffer    where the bytes go
   * @param[in]     capacity  size of buffer, bytes
   * @return the number of bytes written to buffer
   */
  long receiveBytes(uint8_t * buffer, size_t capacity) override
  {
    const size_t n = std::min(capacity, incoming.size());
    std::memcpy(buffer, incoming.data(), n);
    incoming.erase(incoming.begin(), incoming.begin() + static_cast<long>(n));
    return static_cast<long>(n);
  }

  /**
   * @brief Gives the descriptor to poll.
   *
   * @return -1, always (no descriptor)
   */
  int pollFd() const override
  {
    return -1;
  }
};

// The updater with its ground socket, its fake controller and its clock.
class Rig
{
public:
  /**
   * @brief Builds the rig over a data directory.
   *
   * No restart script is configured, so a restart or reset is only counted and logged; this
   * audit never runs restore.sh. The controller watch starts at kRigStartMs with the silence
   * limit of the program (config::kSilenceMs).
   *
   * @param[in]     dir  data directory of the updater (made by open() if absent)
   */
  explicit Rig(const std::string & dir)
  : transport_(), link_(transport_), gate_(kPort), store_(dir.c_str()),
    checker_(PUBLIC_KEY_A, PUBLIC_KEY_B), reports_(dir.c_str(), config::kReportStoreBytes),
    watch_(config::kSilenceMs, kRigStartMs),
    updater_(gate_, link_, store_, checker_, reports_, watch_, 0, "", config::kRestartScriptMs),
    client_(-1), now_(kRigStartMs), listen_(0), next_controller_sequence_(1), parser_(),
    pending_(), passed_{}, ignore_{}, auto_answer_(true), reports_seen_()
  {
  }

  /** @brief Closes the test's own socket. */
  ~Rig()
  {
    if (client_ >= 0) {
      (void)::close(client_);
    }
  }

  Rig(const Rig &) = delete;
  Rig & operator=(const Rig &) = delete;

  /**
   * @brief Opens the ground socket, the test's own socket and the stores.
   *
   * @return true: all open; false: the port is in use or a store failed
   */
  bool open()
  {
    client_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    const int buffer_bytes = kClientBufferBytes;
    (void)::setsockopt(client_, SOL_SOCKET, SO_RCVBUF, &buffer_bytes, sizeof buffer_bytes);
    return client_ >= 0 && gate_.open() && store_.ready() && reports_.load() && checker_.ready();
  }

  /** @brief The monotonic time the updater is given, ms. @return the time */
  uint64_t now() const { return now_; }

  /** @brief Moves the monotonic time on. @param[in] ms time to add, ms @return nothing */
  void advance(uint64_t ms) { now_ += ms; }

  /** @brief Sets the monotonic time. @param[in] ms new time, ms @return nothing */
  void setNow(uint64_t ms) { now_ = ms; }

  /** @brief The controller watch, to read its state. @return the watch */
  ControllerWatch & watch() { return watch_; }

  /**
   * @brief Chooses whether the fake controller answers DONE to every command by itself.
   *
   * @param[in]     on  true: pump() answers each command DONE; false: the test answers
   * @return nothing
   */
  void autoAnswer(bool on) { auto_answer_ = on; }

  /**
   * @brief Makes the fake controller leave the next command with this identifier unanswered.
   *
   * @param[in]     id  the command identifier, payload byte 0
   * @return nothing
   */
  void ignoreOnce(uint8_t id) { ignore_[id] = true; }

  /** @brief Commands passed on to the controller with this identifier since clearCounts.
   *  @param[in] id the identifier @return the count */
  uint32_t passed(uint8_t id) const { return passed_[id]; }

  /** @brief Forgets the counts and the frame list. @return nothing */
  void clearCounts()
  {
    passed_.fill(0);
    frames_.clear();
  }

  /** @brief Every command frame passed on since clearCounts, in order. @return the frames */
  const std::vector<LinkFrame> & frames() const { return frames_; }

  /**
   * @brief Puts one frame from the controller into data link A; the updater reads it.
   *
   * @param[in]     type     frame::kType...
   * @param[in]     payload  the frame's payload
   * @return nothing
   */
  void fromController(uint8_t type, const std::vector<uint8_t> & payload)
  {
    frame::Frame f{type, frame::kDestOnboard, 0, static_cast<uint16_t>(payload.size()),
                   next_controller_sequence_++, 0, payload.data()};
    std::array<uint8_t, frame::kMaxBytes> out{};
    const size_t n = frame::encode(f, out.data(), out.size());
    transport_.incoming.assign(out.begin(), out.begin() + static_cast<long>(n));
    listen_ += kListenStepMs;
    (void)updater_.onLinkReadable(listen_, now_);
    collect();
  }

  /**
   * @brief An answer from the controller to a command of the updater.
   *
   * @param[in]     answered  the data-link sequence number of that command
   * @param[in]     kind      answer::kReceived, kDone, kRefused
   * @param[in]     code      the answer's code
   * @return nothing
   */
  void answer(uint32_t answered, uint8_t kind, uint8_t code)
  {
    std::vector<uint8_t> p(frame::kAnswerBytes, 0);
    frame::encodeAnswer(frame::Answer{answered, kind, code}, p.data());
    fromController(frame::kTypeAnswer, p);
  }

  /**
   * @brief A HEALTH report from the controller showing this mode.
   *
   * @param[in]     mode  report::kMode...
   * @return nothing
   */
  void health(uint8_t mode)
  {
    std::vector<uint8_t> p(report::kHealthBytes, 0);
    p[report::kOffsetKind] = report::kHealth;
    p[report::kOffsetMode] = mode;
    p[report::kOffsetFirmwareKind] = 0x02;
    fromController(frame::kTypeReport, p);
  }

  /**
   * @brief An EVENT report from the controller.
   *
   * @param[in]     code  the event code (Interfaces Sec. 5.4)
   * @return nothing
   */
  void event(uint16_t code)
  {
    std::vector<uint8_t> p(report::kEventMinBytes, 0);
    p[report::kOffsetKind] = report::kEvent;
    bytes::writeU16(&p[report::kOffsetEventCode], code);
    fromController(frame::kTypeReport, p);
  }

  /**
   * @brief Runs the updater's timers once at the current time and collects what it sent.
   *
   * @return nothing
   */
  void tick()
  {
    updater_.tick(now_);
    collect();
  }

  /**
   * @brief Runs ticks until the updater has nothing more to send, answering for the controller.
   *
   * Each tick may send one frame on data link A. With autoAnswer on, every command frame is
   * answered DONE at once, except one whose identifier ignoreOnce named. Bounded by
   * kPumpPassesMax passes.
   *
   * @return nothing
   */
  void pump()
  {
    for (size_t pass = 0; pass < kPumpPassesMax; pass++) {
      updater_.tick(now_);
      collect();
      if (pending_.empty()) {
        return;
      }
      std::vector<LinkFrame> batch;
      batch.swap(pending_);
      for (const LinkFrame & f : batch) {
        frames_.push_back(f);
        if (f.payload.empty()) {
          continue;
        }
        const uint8_t id = f.payload[0];
        passed_[id]++;
        if (ignore_[id]) {
          ignore_[id] = false;
          continue;
        }
        if (auto_answer_) {
          answer(f.sequence, answer::kDone, answer::kSuccess);
        }
      }
    }
  }

  /**
   * @brief Sends one command from the ground over UDP, lets the updater act, then pumps.
   *
   * @param[in]     destination  frame::kDestOnboard or kDestController
   * @param[in]     sequence     the ground's sequence number
   * @param[in]     payload      the command's payload
   * @param[in]     expiry       the Expiry field, seconds of ground time
   * @return true: the gate received the datagram; false: it did not arrive in kGroundWaitMs
   */
  bool ground(uint8_t destination, uint32_t sequence, const std::vector<uint8_t> & payload,
    uint32_t expiry)
  {
    if (!send(destination, sequence, payload, expiry)) {
      return false;
    }
    pump();
    return true;
  }

  /**
   * @brief Sends one command from the ground and lets the updater read it; does not pump.
   *
   * @param[in]     destination  frame::kDestOnboard or kDestController
   * @param[in]     sequence     the ground's sequence number
   * @param[in]     payload      the command's payload
   * @param[in]     expiry       the Expiry field, seconds of ground time
   * @return true: the gate received the datagram; false: it did not arrive in kGroundWaitMs
   */
  bool send(uint8_t destination, uint32_t sequence, const std::vector<uint8_t> & payload,
    uint32_t expiry)
  {
    frame::Frame f{frame::kTypeCommand, destination, 0, static_cast<uint16_t>(payload.size()),
                   sequence, expiry, payload.data()};
    std::array<uint8_t, frame::kMaxBytes> out{};
    const size_t n = frame::encode(f, out.data(), out.size());
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kPort);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::sendto(client_, out.data(), n, 0, reinterpret_cast<const sockaddr *>(&to),
          sizeof to) != static_cast<ssize_t>(n)) {
      return false;
    }
    pollfd p{};
    p.fd = gate_.fd();
    p.events = POLLIN;
    if (::poll(&p, 1, kGroundWaitMs) != 1) {
      return false;
    }
    updater_.onGroundReadable(now_);
    collect();
    return true;
  }

  /**
   * @brief Reads every answer the updater has sent to the ground, waiting until it is quiet.
   *
   * Loopback delivery is not always complete when sendto returns on a loaded Mac (measured: a
   * non-blocking read lost about 15 % of the answers), so this waits up to kAnswerQuietMs for
   * each next datagram and stops at the first quiet period. Reports are kept apart
   * (takeReports()). Bounded by kDrainMax datagrams.
   *
   * @return the answers in order
   */
  std::vector<GroundAnswer> answers()
  {
    std::vector<GroundAnswer> result;
    std::array<uint8_t, 2048> buffer{};
    for (size_t i = 0; i < kDrainMax; i++) {
      pollfd p{};
      p.fd = client_;
      p.events = POLLIN;
      if (::poll(&p, 1, kAnswerQuietMs) != 1) {
        break;
      }
      const ssize_t n = ::recv(client_, buffer.data(), buffer.size(), MSG_DONTWAIT);
      if (n <= 0) {
        continue;
      }
      frame::Frame f{};
      frame::Answer a{};
      if (!frame::decode(buffer.data(), static_cast<size_t>(n), f)) {
        continue;
      }
      if (f.type == frame::kTypeAnswer && frame::decodeAnswer(f, a)) {
        result.push_back(GroundAnswer{a.answered, a.kind, a.code});
      } else if (f.type == frame::kTypeReport) {
        reports_seen_.emplace_back(f.payload, f.payload + f.length);
      }
    }
    return result;
  }

  /**
   * @brief Throws away what is already in the test's socket, without waiting.
   *
   * @return nothing
   */
  void discard()
  {
    std::array<uint8_t, 2048> buffer{};
    for (size_t i = 0; i < kDrainMax; i++) {
      if (::recv(client_, buffer.data(), buffer.size(), MSG_DONTWAIT) <= 0) {
        break;
      }
    }
  }

  /** @brief The report payloads the ground has received and not yet taken. @return them */
  std::vector<std::vector<uint8_t>> takeReports()
  {
    std::vector<std::vector<uint8_t>> r;
    r.swap(reports_seen_);
    return r;
  }

private:
  // Upper bound of datagrams read by one answers() call (code rule B2): more than any one
  // exchange of this audit produces between two drains.
  static constexpr size_t kDrainMax = 100000;

  /**
   * @brief Moves the bytes the updater sent into whole frames, in order.
   *
   * @return nothing
   */
  void collect()
  {
    for (uint8_t byte : transport_.sent) {
      frame::Frame f{};
      if (parser_.feed(byte, 0, f)) {
        pending_.push_back(LinkFrame{f.type, f.destination, f.sequence, f.expiry,
                                     std::vector<uint8_t>(f.payload, f.payload + f.length)});
      }
    }
    transport_.sent.clear();
  }

  FakeTransport transport_;
  DataLink link_;
  CommandGate gate_;
  UploadStore store_;
  ImageChecker checker_;
  ReportStore reports_;
  ControllerWatch watch_;
  Updater updater_;
  int client_;
  uint64_t now_;
  uint64_t listen_;
  uint32_t next_controller_sequence_;
  frame::Parser parser_;
  std::vector<LinkFrame> pending_;
  std::vector<LinkFrame> frames_;
  std::array<uint32_t, 256> passed_;
  std::array<bool, 256> ignore_;
  bool auto_answer_;
  std::vector<std::vector<uint8_t>> reports_seen_;
};

}  // namespace stress

#endif  // FSIR_STRESS_BOUNDARIES_RIG_HPP
