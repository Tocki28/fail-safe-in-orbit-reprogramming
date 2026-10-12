/*
 * frame.hpp — the frame of the Interfaces document (Sec. 4.0, Figure 2) and the answer payload
 * (Sec. 4.2).
 *
 *   offset  bytes  field
 *        0      2  Sync 0xA5 0x5A
 *        2      1  Version 0x01
 *        3      1  Type: 0x01 command, 0x02 answer, 0x03 report
 *        4      1  Destination: 0x01 onboard computer, 0x02 controller, 0x03 ground
 *        5      1  Flags: bit 0 = released scheduled command; other bits 0
 *        6      2  Length = payload bytes, 0 to 1024
 *        8      4  Sequence number
 *       12      4  Expiry (seconds of ground time; 0 in answers and reports)
 *       16      L  Payload
 *     16+L      4  Check value = CRC-32 of every byte before it
 * Multi-byte fields are little-endian.
 *
 * The onboard computer's own code, written to give the same bytes as the controller's
 * src/controller/firmware/data-link/frame.c (commit 5c0b5f8); test/onboard-computer/unit compares the two on
 * the same inputs. Used for data link A and for the ground link. REQ-099 asks only the two
 * data links to share no software; data link B (CAN, not on this bench) will need its own.
 */
#ifndef FSIR_UPDATER_FRAME_HPP
#define FSIR_UPDATER_FRAME_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace frame
{

constexpr uint8_t kSync0 = 0xA5;
constexpr uint8_t kSync1 = 0x5A;
constexpr uint8_t kVersion = 0x01;
constexpr uint8_t kTypeCommand = 0x01;
constexpr uint8_t kTypeAnswer = 0x02;
constexpr uint8_t kTypeReport = 0x03;
constexpr uint8_t kDestOnboard = 0x01;
constexpr uint8_t kDestController = 0x02;
constexpr uint8_t kDestGround = 0x03;

constexpr size_t kHeaderBytes = 16;
constexpr size_t kPayloadMax = 1024;
constexpr size_t kCheckBytes = 4;
constexpr size_t kMinBytes = kHeaderBytes + kCheckBytes;
constexpr size_t kMaxBytes = kHeaderBytes + kPayloadMax + kCheckBytes;

constexpr size_t kOffsetVersion = 2;
constexpr size_t kOffsetType = 3;
constexpr size_t kOffsetDestination = 4;
constexpr size_t kOffsetFlags = 5;
constexpr size_t kOffsetLength = 6;
constexpr size_t kOffsetSequence = 8;
constexpr size_t kOffsetExpiry = 12;

// Answer payload: sequence number answered (4), kind (1), code (1).
constexpr size_t kAnswerBytes = 6;

// One frame's fields. payload points at length bytes owned by someone else.
struct Frame
{
  uint8_t type;
  uint8_t destination;
  uint8_t flags;
  uint16_t length;
  uint32_t sequence;
  uint32_t expiry;
  const uint8_t * payload;
};

struct Answer
{
  uint32_t answered;   // the sequence number of the command answered
  uint8_t kind;        // RECEIVED, DONE, REFUSED, ALREADY DONE (protocol.hpp)
  uint8_t code;
};

// Writes f as bytes, check value included. Returns the size, or 0 if the payload is longer than
// 1024 bytes or out is too small.
size_t encode(const Frame & f, uint8_t * out, size_t room);

// Reads one whole frame, e.g. one UDP datagram: Sync, Version, Length equal to what follows
// the header, and the check value. f.payload then points into bytes.
bool decode(const uint8_t * bytes, size_t size, Frame & f);

// The 6-byte answer payload, and its reading (false if the frame is no answer of 6 bytes).
void encodeAnswer(const Answer & a, uint8_t * out);
bool decodeAnswer(const Frame & f, Answer & a);

// DONE, REFUSED and ALREADY DONE end a command; RECEIVED does not.
bool isFinal(uint8_t kind);

// Takes one received byte at a time, as frame.c does on the controller, with the same rules:
//   - each header field is checked when its last byte arrives; a byte that breaks the header
//     may be the first Sync byte of the next frame, so it is looked at again as a start;
//   - a frame whose check value does not match is dropped whole (REQ-107);
//   - a gap of more than 10 ms inside a frame drops what was held (Interfaces Sec. 6.2).
// The caller supplies the time of each byte in ms (see main.cpp: the listening clock).
class Parser
{
public:
  Parser();

  void reset();

  // True when this byte completes a frame that passed every check; f then describes it, and
  // its payload points into the parser, valid until the next feed().
  bool feed(uint8_t byte, uint64_t now_ms, Frame & f);

private:
  bool headerSoFarOk() const;
  void startOver(uint8_t byte);

  std::array<uint8_t, kMaxBytes> bytes_;
  size_t count_;      // bytes held
  size_t total_;      // the whole frame's size once Length has arrived; 0 before
  uint64_t last_ms_;  // time of the last byte held
};

}  // namespace frame

#endif  // FSIR_UPDATER_FRAME_HPP
