/*
 * command_gate.hpp — the onboard computer's side of the ground link (Architecture Sec. 6.0,
 * "Command gate"): takes every command from the ground, checks it, passes it on, logs it, and
 * sends answers and reports back down.
 *
 * Bench link: UDP, one frame (Interfaces Sec. 4.0, with its CRC-32) per datagram, on port 47001
 * of all interfaces. Answers and reports go to the source address and port of the most recent
 * good command frame from the ground. A damaged datagram does not move them: anyone on the
 * network could otherwise redirect the downlink with one bad packet.
 *
 * BENCH LIMITS (accepted by the owner for the two-day demo):
 *   - no radio envelope (Interfaces Sec. 4.1): commands are neither encrypted nor signed, so
 *     REQ-064, REQ-127 and REQ-129 are not met for commands. Firmware images are still checked
 *     for both signatures (image_checker.hpp);
 *   - old commands sent again (REQ-065) are caught for ARMs and dangerous commands only, and
 *     only the last 64 (UsedSequences). A command sent again with the same sequence number and the
 *     same bytes is not an old command but a resend after a lost answer: ALREADY DONE once it
 *     has finished, RECEIVED while it has not (REQ-031, Interfaces Sec. 4.2; owner, 13 Oct 2026).
 *     That holds for ARMs and dangerous commands only: other ground commands are not recognised
 *     as repeats. No expiry check (REQ-066): the ground console restarts its counter at 1 and
 *     the board's clock is not set from ground time;
 *   - REQ-067 (commands only from the radio link) stands only as "only from this UDP port".
 */
#ifndef FSIR_UPDATER_COMMAND_GATE_HPP
#define FSIR_UPDATER_COMMAND_GATE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <netinet/in.h>

#include "config.hpp"
#include "frame.hpp"

class CommandGate
{
public:
  explicit CommandGate(uint16_t port);
  ~CommandGate();
  CommandGate(const CommandGate &) = delete;
  CommandGate & operator=(const CommandGate &) = delete;

  // Opens the UDP socket on all interfaces, non-blocking and close-on-exec. False if that failed; errno tells why.
  bool open();

  int fd() const;

  // Reads one datagram. True when it is a good command frame for the onboard computer or the
  // controller; command.payload then points into the gate, valid until the next call.
  // Anything else is dropped and logged, without an answer (Interfaces Sec. 4.2).
  bool receive(frame::Frame & command);

  // One answer to the ground (Destination 0x03): to the command with ground sequence number
  // answered.
  bool answer(uint32_t answered, uint8_t kind, uint8_t code);

  // One report to the ground (Destination 0x03), payload as made by the report store.
  bool sendReport(const uint8_t * payload, uint16_t length);

  bool haveGround() const;

  // Datagrams dropped since start, and how many of those got a log line (the rest were
  // suppressed by the rate limit, config::kDropLogEveryMs).
  uint32_t droppedCount() const;
  uint32_t droppedLogged() const;

private:
  bool dropLogDue();
  bool sendToGround(uint8_t type, const uint8_t * payload, uint16_t length);

  uint16_t port_;
  int fd_;
  sockaddr_in ground_;
  bool have_ground_;
  // Sequence number of the next frame to the ground: one higher than the last (Interfaces
  // Sec. 4.0). The ground link's own counter, apart from data link A's, so that each receiver
  // sees its numbers go up by one.
  uint32_t next_sequence_;
  uint32_t dropped_;          // datagrams dropped since start
  uint32_t dropped_logged_;   // of them, logged
  uint32_t suppressed_;       // dropped since the last logged one
  uint64_t last_drop_log_ms_;
  std::array<uint8_t, 2048> rx_;   // larger than any frame, so a longer datagram shows
  std::array<uint8_t, frame::kMaxBytes> tx_;
};

// A ground command for the controller, waiting for data link A.
struct ForwardedCommand
{
  uint32_t ground_sequence;
  uint32_t expiry;
  uint16_t length;
  bool armed;   // a dangerous command whose held ARM was checked and used up on arrival
  uint64_t arm_at_ms;   // when that ARM arrived; checked again just before the ARM is sent on
  std::array<uint8_t, frame::kPayloadMax> payload;
};

// Ground commands for the controller, in arrival order, while data link A is busy.
class ForwardQueue
{
public:
  ForwardQueue();
  bool empty() const;
  bool full() const;
  // False if full. armed: the command is dangerous and its ARM was used up (updater.hpp).
  bool push(const frame::Frame & command, bool armed, uint64_t arm_at_ms);
  // The oldest; only when !empty().
  const ForwardedCommand & front() const;
  void pop();

private:
  std::array<ForwardedCommand, config::kForwardQueueSize> items_;
  size_t head_;
  size_t count_;
};

// Which ground command each data-link sequence number carried, so the controller's answers go
// back with the ground's own sequence number. Why the numbers differ: data_link.hpp.
class ForwardTable
{
public:
  ForwardTable();
  // Replaces the oldest entry when full.
  void add(uint32_t link_sequence, uint32_t ground_sequence);
  bool find(uint32_t link_sequence, uint32_t & ground_sequence) const;
  void remove(uint32_t link_sequence);

private:
  struct Entry
  {
    bool used;
    uint32_t link_sequence;
    uint32_t ground_sequence;
  };
  std::array<Entry, config::kForwardTableSize> entries_;
  size_t next_;
};

// The ground sequence numbers of the ARMs and dangerous commands accepted since start, the last
// config::kUsedSequenceCount of them, each with its payload bytes and how it ended. A repeat
// with other bytes is refused 0x04 (REQ-065); a repeat with the same bytes is a resend after a
// lost answer (REQ-031, Interfaces Sec. 4.2) and is answered from the entry's state.
class UsedSequences
{
public:
  // What an incoming ARM or dangerous command is, compared with the ring.
  enum class Repeat : uint8_t
  {
    kNew,            // sequence number not in the ring
    kSamePending,    // same number, same bytes, first one not finished: answer RECEIVED
    kSameDone,       // same number, same bytes, first one ended DONE: answer ALREADY DONE
    kSameRefused,    // same number, same bytes, first one ended REFUSED: not ALREADY DONE
    kDifferent,      // same number, other bytes: REFUSED 0x04
  };

  UsedSequences();
  // Compares a command with the ring; changes nothing.
  Repeat check(uint32_t sequence, const uint8_t * payload, uint16_t length) const;
  // Replaces the oldest when full; the new entry is pending. False (nothing stored) when the
  // payload is longer than config::kUsedPayloadMax.
  [[nodiscard]] bool add(uint32_t sequence, const uint8_t * payload, uint16_t length);
  // The entry's final answer has gone to the ground: done = DONE or ALREADY DONE, else REFUSED.
  // Only a pending entry changes.
  void finish(uint32_t sequence, bool done);

private:
  enum class State : uint8_t
  {
    kPending,
    kDone,
    kRefused,
  };

  struct Entry
  {
    bool used;
    State state;
    uint32_t sequence;
    uint16_t length;
    std::array<uint8_t, config::kUsedPayloadMax> payload;
  };
  std::array<Entry, config::kUsedSequenceCount> entries_;
  size_t next_;
};

#endif  // FSIR_UPDATER_COMMAND_GATE_HPP
