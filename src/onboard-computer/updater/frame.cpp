#include "frame.hpp"

#include "bytes.hpp"
#include "config.hpp"
#include "crc32.hpp"
#include "protocol.hpp"

namespace frame
{

/**
 * @brief Writes f as bytes, check value included.
 *
 * Layout of Interfaces Sec. 4.0, Figure 2 (frame.hpp). Returns 0 if the payload is longer than
 * 1024 bytes or out is too small.
 *
 * @param[in] f  the frame to write; f.payload must point at f.length bytes
 * @param[out] out  receives the frame, little-endian fields, CRC-32 last
 * @param[in] room  size of out, bytes
 * @return size of the frame written, bytes; 0 if it does not fit
 */
size_t encode(const Frame & f, uint8_t * out, size_t room)
{
  size_t covered = kHeaderBytes + f.length;
  if (f.length > kPayloadMax || covered + kCheckBytes > room) {
    return 0;
  }
  out[0] = kSync0;
  out[1] = kSync1;
  out[kOffsetVersion] = kVersion;
  out[kOffsetType] = f.type;
  out[kOffsetDestination] = f.destination;
  out[kOffsetFlags] = f.flags;
  bytes::writeU16(&out[kOffsetLength], f.length);
  bytes::writeU32(&out[kOffsetSequence], f.sequence);
  bytes::writeU32(&out[kOffsetExpiry], f.expiry);
  for (size_t i = 0; i < f.length; i++) {
    out[kHeaderBytes + i] = f.payload[i];
  }
  bytes::writeU32(&out[covered], crc::crc32(out, covered));
  return covered + kCheckBytes;
}

/**
 * @brief Reads one whole frame, e.g. one UDP datagram.
 *
 * Checks Sync, Version, Length equal to what follows the header, and the check value. f.payload
 * then points into b.
 *
 * @param[in] b  the bytes of the frame
 * @param[in] size  number of bytes in b
 * @param[out] f  the frame's fields; unchanged when false is returned
 * @return true: b is one valid frame and f describes it; false: size, Sync, Version, Length or check value is wrong
 */
bool decode(const uint8_t * b, size_t size, Frame & f)
{
  if (size < kMinBytes || size > kMaxBytes) {
    return false;
  }
  if (b[0] != kSync0 || b[1] != kSync1 || b[kOffsetVersion] != kVersion) {
    return false;
  }
  uint16_t length = bytes::readU16(&b[kOffsetLength]);
  if (length > kPayloadMax || kHeaderBytes + length + kCheckBytes != size) {
    return false;
  }
  size_t covered = kHeaderBytes + length;
  if (crc::crc32(b, covered) != bytes::readU32(&b[covered])) {
    return false;
  }
  f.type = b[kOffsetType];
  f.destination = b[kOffsetDestination];
  f.flags = b[kOffsetFlags];
  f.length = length;
  f.sequence = bytes::readU32(&b[kOffsetSequence]);
  f.expiry = bytes::readU32(&b[kOffsetExpiry]);
  f.payload = &b[kHeaderBytes];
  return true;
}

/**
 * @brief Writes the 6-byte answer payload.
 *
 * Sequence number answered (4 bytes), kind (1), code (1) (Interfaces Sec. 4.2).
 *
 * @param[in] a  the answer to write
 * @param[out] out  receives kAnswerBytes (6) bytes
 * @return nothing
 */
void encodeAnswer(const Answer & a, uint8_t * out)
{
  bytes::writeU32(&out[0], a.answered);
  out[4] = a.kind;
  out[5] = a.code;
}

/**
 * @brief Reads the 6-byte answer payload of a frame.
 *
 * @param[in] f  a decoded frame
 * @param[out] a  the answer; unchanged when false is returned
 * @return true: f is an answer frame with a 6-byte payload and a holds it; false: f is no answer of 6 bytes
 */
bool decodeAnswer(const Frame & f, Answer & a)
{
  if (f.type != kTypeAnswer || f.length != kAnswerBytes) {
    return false;
  }
  a.answered = bytes::readU32(&f.payload[0]);
  a.kind = f.payload[4];
  a.code = f.payload[5];
  return true;
}

/**
 * @brief Tells whether an answer kind ends a command.
 *
 * DONE, REFUSED and ALREADY DONE end a command; RECEIVED does not.
 *
 * @param[in] kind  answer kind (protocol.hpp)
 * @return true: DONE, REFUSED or ALREADY DONE; false: any other kind
 */
bool isFinal(uint8_t kind)
{
  return kind == answer::kDone || kind == answer::kRefused || kind == answer::kAlreadyDone;
}

/**
 * @brief Builds a parser that holds no bytes.
 * @return nothing (constructor)
 */
Parser::Parser()
: bytes_{}, count_(0), total_(0), last_ms_(0)
{
}

/**
 * @brief Drops the bytes held, so that the next byte starts a frame.
 * @return nothing
 */
void Parser::reset()
{
  count_ = 0;
  total_ = 0;
}

/**
 * @brief Checks the header field whose last byte has just arrived.
 *
 * Each header field is checked when its last byte arrives: Sync 0, Sync 1, Version, and Length
 * against the largest payload.
 * @return true: the bytes held so far are consistent with a frame; false: the header is broken
 */
bool Parser::headerSoFarOk() const
{
  switch (count_) {
    case 1:
      return bytes_[0] == kSync0;
    case 2:
      return bytes_[1] == kSync1;
    case kOffsetVersion + 1:
      return bytes_[kOffsetVersion] == kVersion;
    case kOffsetLength + 2:
      return bytes::readU16(&bytes_[kOffsetLength]) <= kPayloadMax;
    default:   // count_ is a byte count, not an enum: every other count has nothing to check yet
      return true;
  }
}

/**
 * @brief Drops what is held and keeps the byte that broke the header if it can start a new frame.
 *
 * Drop what is held; keep the byte that broke the header if it can start a new frame.
 *
 * @param[in] byte  the byte that broke the header
 * @return nothing
 */
void Parser::startOver(uint8_t byte)
{
  reset();
  if (byte == kSync0) {
    bytes_[0] = byte;
    count_ = 1;
  }
}

/**
 * @brief Takes one received byte.
 *
 * True when this byte completes a frame that passed every check; f then describes it, and its
 * payload points into the parser, valid until the next feed().
 * Rules, as in frame.c on the controller: a frame whose check value does not match is dropped
 * whole (REQ-107); a gap of more than 10 ms inside a frame drops what was held (Interfaces
 * Sec. 6.2).
 *
 * @param[in] byte  the received byte
 * @param[in] now_ms  arrival time of the byte, ms; used to measure the gap
 * @param[out] f  the completed frame; set only when true is returned
 * @return true: this byte completed a valid frame; false: no frame complete yet, or the frame was dropped
 */
bool Parser::feed(uint8_t byte, uint64_t now_ms, Frame & f)
{
  if (count_ > 0 && now_ms - last_ms_ > config::kFrameGapMaxMs) {
    reset();
  }
  last_ms_ = now_ms;
  // count_ < kMaxBytes here: Length is checked against 1024 at byte 8, so a frame never
  // needs more than kMaxBytes, and the frame is handed over and dropped at its last byte.
  bytes_[count_] = byte;
  count_++;

  if (!headerSoFarOk()) {
    startOver(byte);
    return false;
  }
  if (count_ == kOffsetLength + 2) {
    total_ = kHeaderBytes + bytes::readU16(&bytes_[kOffsetLength]) + kCheckBytes;
  }
  if (total_ == 0 || count_ < total_) {
    return false;   // not complete yet
  }

  bool good = decode(bytes_.data(), total_, f);
  reset();   // the bytes stay in place until the next frame overwrites them
  return good;
}

}  // namespace frame
