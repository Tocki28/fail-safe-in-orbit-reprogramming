/*
 * data_link.hpp — data link A's frame layer and stop-and-wait (Interfaces Sec. 4.0, 4.2, 6.2).
 *
 * Stop-and-wait: one command at a time is outstanding on the link. The next command goes only
 * after the controller's last answer to the previous one (DONE, REFUSED or ALREADY DONE), or
 * after its timeout, because the controller cannot receive while it writes its flash
 * (src/controller/common/flash.c: code stalls during an erase).
 *
 * Sequence numbers: one counter for every frame the onboard computer sends on this link,
 * whoever asked for it (the ground, a delivery, the beat), from 1 at every start. The frame
 * has no source field, so two senders with separate counters on one link could not be told
 * apart; the controller sees only the onboard computer as its sender. An ARM names its
 * dangerous command by sequence number (Interfaces Sec. 5.2), but the controller sees the
 * onboard computer's number, not the ground's. Decision (owner): the onboard computer holds
 * the ground's ARM, checks it against the ground's command, and sends the controller a pair,
 * an ARM naming the number the command will carry on this link (nextSequence() + 1 when the ARM
 * goes out), then the command, with no frame between them (updater.hpp, ARM AND DANGEROUS
 * COMMANDS). Open (as for the controller, REQ-065): keeping the counter over restarts, and
 * what follows 2^32 - 1; here it goes back to 1.
 */
#ifndef FSIR_UPDATER_DATA_LINK_HPP
#define FSIR_UPDATER_DATA_LINK_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "frame.hpp"
#include "transport.hpp"

// Receives every frame that arrives from the controller and passes its checks.
class ControllerFrameHandler
{
public:
  ControllerFrameHandler() = default;
  virtual ~ControllerFrameHandler() = default;
  ControllerFrameHandler(const ControllerFrameHandler &) = delete;
  ControllerFrameHandler & operator=(const ControllerFrameHandler &) = delete;

  // f.payload is valid only during the call.
  virtual void onControllerFrame(const frame::Frame & f) = 0;
};

class DataLink
{
public:
  explicit DataLink(Transport & transport);

  // Reads what has arrived and passes each complete frame to handler. listen_ms is the time
  // the parser measures the 10 ms gap with (main.cpp). False if the channel failed.
  bool receive(ControllerFrameHandler & handler, uint64_t listen_ms);

  // Forgets a part frame, e.g. after the device was opened again.
  void resetParser();

  // Sends one command to the controller (Destination 0x02) with the next sequence number and
  // starts its timeout. Only when !busy(). False if the channel failed; nothing is then
  // outstanding.
  bool sendCommand(const uint8_t * payload, uint16_t length, uint32_t expiry,
    uint32_t timeout_ms, uint64_t now_ms, uint32_t & sequence);

  // The sequence number the next command sent will carry (Interfaces Sec. 4.0; 1 after 2^32 - 1).
  uint32_t nextSequence() const;

  // A command is outstanding: no other may be sent.
  bool busy() const;
  uint32_t outstanding() const;

  // The outstanding command got its last answer.
  void release();

  // True once when the outstanding command's timeout has passed; it is then released.
  bool timedOut(uint64_t now_ms);

  // For the log: frames sent and received.
  uint32_t sentCount() const;
  uint32_t receivedCount() const;

private:
  Transport & transport_;
  frame::Parser parser_;
  std::array<uint8_t, 4096> rx_;
  std::array<uint8_t, frame::kMaxBytes> tx_;
  uint32_t next_sequence_;
  bool busy_;
  uint32_t outstanding_;
  uint64_t deadline_ms_;
  uint32_t sent_;
  uint32_t received_;
};

#endif  // FSIR_UPDATER_DATA_LINK_HPP
