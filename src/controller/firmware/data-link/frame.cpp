/*
 * frame.cpp — frame parser and builder (Interfaces Sec. 4.0, Figure 2), answer payload (Sec. 4.2).
 *
 * The parser checks each header field as soon as its last byte arrives, so a broken header is
 * dropped at once, not after waiting for its Length bytes. Rules:
 *   - a byte that breaks the header (Sync, Version, Length above 1024) may itself be the first
 *     Sync byte of the next frame, so it is looked at again as a possible start;
 *   - a frame whose check value does not match is dropped whole (REQ-107);
 *   - a gap of more than 10 ms inside a frame drops what was held (Interfaces Sec. 6.2).
 * Limit: after a false start inside noise, a real frame that follows without a 10 ms gap can
 * be taken as payload of the false one and is lost with it; its sender gets no answer.
 */
#include "frame.hpp"

#include "crc32.h"

namespace {

uint16_t readU16(const uint8_t *bytes)
{
    return static_cast<uint16_t>(static_cast<uint32_t>(bytes[0]) |
                                 (static_cast<uint32_t>(bytes[1]) << 8));
}

uint32_t readU32(const uint8_t *bytes)
{
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

void writeU16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
}

void writeU32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
    bytes[2] = static_cast<uint8_t>(value >> 16);
    bytes[3] = static_cast<uint8_t>(value >> 24);
}

}  // namespace

void FrameParser::reset()
{
    count_ = 0U;
    total_ = 0U;
    last_ms_ = 0U;
}

void FrameParser::dropHeld()
{
    count_ = 0U;
    total_ = 0U;
}

/* Drop what is held; keep the byte that broke the header if it can start a new frame. */
void FrameParser::startOver(uint8_t byte)
{
    dropHeld();
    if (byte == kFrameSync0) {
        bytes_[0] = byte;
        count_ = 1U;
    }
}

/* Checks the header field that the byte just stored completes, if any. */
bool FrameParser::headerSoFarOk() const
{
    switch (count_) {
    case 1U:
        return bytes_[0] == kFrameSync0;
    case 2U:
        return bytes_[1] == kFrameSync1;
    case kFrameOffsetVersion + 1U:
        return bytes_[kFrameOffsetVersion] == kFrameVersion;
    case kFrameOffsetLength + 2U:
        return readU16(&bytes_[kFrameOffsetLength]) <= kFramePayloadMax;
    default:
        return true;
    }
}

bool FrameParser::checkValueOk() const
{
    uint32_t covered = total_ - kFrameCheckBytes;

    return crc32_compute(bytes_.data(), covered) == readU32(&bytes_[covered]);
}

void FrameParser::decode(Frame &frame) const
{
    frame.type = static_cast<FrameType>(bytes_[kFrameOffsetType]);
    frame.destination = static_cast<Destination>(bytes_[kFrameOffsetDestination]);
    frame.flags = bytes_[kFrameOffsetFlags];
    frame.length = readU16(&bytes_[kFrameOffsetLength]);
    frame.sequence = readU32(&bytes_[kFrameOffsetSequence]);
    frame.expiry = readU32(&bytes_[kFrameOffsetExpiry]);
    frame.payload = &bytes_[kFrameHeaderBytes];
}

bool FrameParser::feed(uint8_t byte, uint32_t now_ms, Frame &frame)
{
    /* Unsigned subtraction stays right when the millisecond count wraps. */
    if ((count_ > 0U) && ((now_ms - last_ms_) > kFrameGapMaxMs)) {
        dropHeld();
    }
    last_ms_ = now_ms;
    bytes_[count_] = byte;
    count_++;

    if (!headerSoFarOk()) {
        startOver(byte);
        return false;
    }
    if (count_ == (kFrameOffsetLength + 2U)) {
        total_ = kFrameHeaderBytes + readU16(&bytes_[kFrameOffsetLength]) + kFrameCheckBytes;
    }
    if ((total_ == 0U) || (count_ < total_)) {
        return false;   /* not complete yet */
    }

    bool good = checkValueOk();
    if (good) {
        decode(frame);
    }
    dropHeld();   /* the bytes stay in place until the next frame overwrites them */
    return good;
}

uint32_t buildFrame(const Frame &frame, uint8_t *out, uint32_t room)
{
    uint32_t covered = kFrameHeaderBytes + static_cast<uint32_t>(frame.length);

    if ((frame.length > kFramePayloadMax) || ((covered + kFrameCheckBytes) > room)) {
        return 0U;
    }
    out[0] = kFrameSync0;
    out[1] = kFrameSync1;
    out[kFrameOffsetVersion] = kFrameVersion;
    out[kFrameOffsetType] = static_cast<uint8_t>(frame.type);
    out[kFrameOffsetDestination] = static_cast<uint8_t>(frame.destination);
    out[kFrameOffsetFlags] = frame.flags;
    writeU16(&out[kFrameOffsetLength], frame.length);
    writeU32(&out[kFrameOffsetSequence], frame.sequence);
    writeU32(&out[kFrameOffsetExpiry], frame.expiry);
    for (uint32_t i = 0U; i < frame.length; i++) {
        out[kFrameHeaderBytes + i] = frame.payload[i];
    }
    writeU32(&out[covered], crc32_compute(out, covered));
    return covered + kFrameCheckBytes;
}

void answerPayload(uint32_t answered, AnswerKind kind, AnswerCode code,
                   std::array<uint8_t, kAnswerPayloadBytes> &out)
{
    writeU32(&out[kAnswerOffsetSequence], answered);
    out[kAnswerOffsetKind] = static_cast<uint8_t>(kind);
    out[kAnswerOffsetCode] = static_cast<uint8_t>(code);
}
