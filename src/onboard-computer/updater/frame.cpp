#include "frame.hpp"

#include "bytes.hpp"
#include "config.hpp"
#include "crc32.hpp"
#include "protocol.hpp"

namespace frame
{

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

void encodeAnswer(const Answer & a, uint8_t * out)
{
  bytes::writeU32(&out[0], a.answered);
  out[4] = a.kind;
  out[5] = a.code;
}

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

bool isFinal(uint8_t kind)
{
  return kind == answer::kDone || kind == answer::kRefused || kind == answer::kAlreadyDone;
}

Parser::Parser()
: bytes_{}, count_(0), total_(0), last_ms_(0)
{
}

void Parser::reset()
{
  count_ = 0;
  total_ = 0;
}

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
    default:
      return true;
  }
}

// Drop what is held; keep the byte that broke the header if it can start a new frame.
void Parser::startOver(uint8_t byte)
{
  reset();
  if (byte == kSync0) {
    bytes_[0] = byte;
    count_ = 1;
  }
}

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
