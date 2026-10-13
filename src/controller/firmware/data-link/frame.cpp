/*
 * frame.cpp — frame parser and builder (Interfaces Sec. 4.0, Figure 2), answer payload (Sec. 4.2).
 *
 * The parser checks each header field as soon as its last byte arrives, so a broken header is
 * dropped at once, not after waiting for its Length bytes. Rules:
 *   - a byte that breaks the header (Sync, Version, Length above 1024) may itself be the first
 *     Sync byte of the next frame, so it is looked at again as a possible start;
 *   - a frame whose check value does not match is dropped whole (REQ-107);
 *   - a frame whose Flags bits 1 to 7 are not 0 is dropped whole the same way, with no answer
 *     (Interfaces Sec. 4.0, frame table: Flags bit 0 only, other bits 0);
 *   - a gap of more than 10 ms inside a frame drops what was held (Interfaces Sec. 6.2).
 * Limit: after a false start inside noise, a real frame that follows without a 10 ms gap can
 * be taken as payload of the false one and is lost with it; its sender gets no answer.
 */
#include "frame.hpp"

#include "crc32.h"

namespace {

/**
 * @brief Read a little-endian 16-bit value.
 *
 * @param[in] bytes  two bytes, least significant first
 * @return the value
 */
uint16_t readU16(const uint8_t *bytes)
{
    return static_cast<uint16_t>(static_cast<uint32_t>(bytes[0]) |
                                 (static_cast<uint32_t>(bytes[1]) << 8));
}

/**
 * @brief Read a little-endian 32-bit value.
 *
 * @param[in] bytes  four bytes, least significant first
 * @return the value
 */
uint32_t readU32(const uint8_t *bytes)
{
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

/**
 * @brief Write a 16-bit value little-endian.
 *
 * @param[out] bytes  two bytes, least significant first
 * @param[in]  value  the value to write
 */
void writeU16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
}

/**
 * @brief Write a 32-bit value little-endian.
 *
 * @param[out] bytes  four bytes, least significant first
 * @param[in]  value  the value to write
 */
void writeU32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
    bytes[2] = static_cast<uint8_t>(value >> 16);
    bytes[3] = static_cast<uint8_t>(value >> 24);
}

}  // namespace

/**
 * @brief Drop any held bytes and forget the time of the last byte.
 */
void FrameParser::reset()
{
    count_ = 0U;
    total_ = 0U;
    last_ms_ = 0U;
}

/**
 * @brief Drop the bytes held so far; the arrival time of the last byte is kept.
 */
void FrameParser::dropHeld()
{
    count_ = 0U;
    total_ = 0U;
}

/**
 * @brief Drop what is held; keep the byte that broke the header if it can start a new frame.
 *
 * @param[in] byte  the byte that broke the header; kept as the first held byte if it is the
 *                  first Sync byte
 */
void FrameParser::startOver(uint8_t byte)
{
    dropHeld();
    if (byte == kFrameSync0) {
        bytes_[0] = byte;
        count_ = 1U;
    }
}

/**
 * @brief Check the header field that the byte just stored completes, if any.
 *
 * The fields checked are Sync byte 0, Sync byte 1, Version and Length (not above the payload
 * maximum). Any other byte position passes.
 *
 * This switch is on a byte count, not on an enum, so -Wswitch-enum does not apply and the
 * default is intended: it stands for every byte position that has no header check (most of
 * them), not for a forgotten case. No enumerator list exists to complete. The bytes it judges are
 * input from the link, not memory: a bad one drops the frame, never the fault reaction (owner,
 * 13 Oct 2026; firmware/fault.hpp).
 *
 * @return true if the header is still acceptable; false if the last byte broke it
 */
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
    default:   /* input from the link: a byte position with no header check passes */
        return true;
    }
}

/**
 * @brief Compare the CRC-32 of the held frame with the check value at its end.
 *
 * @return true if they match; false if the frame is damaged
 */
bool FrameParser::checkValueOk() const
{
    uint32_t covered = total_ - kFrameCheckBytes;

    return crc32_compute(bytes_.data(), covered) == readU32(&bytes_[covered]);
}

/**
 * @brief Check that the held frame's Flags byte has no bit set except bit 0.
 *
 * Interfaces Sec. 4.0 (frame table) gives Flags bit 0 as "released scheduled command" and says
 * the other bits are 0. A frame with any of bits 1 to 7 set is not one this version defines, so
 * it is not acted on.
 *
 * @return true if bits 1 to 7 of Flags are 0; false otherwise
 */
bool FrameParser::flagsOk() const
{
    return (bytes_[kFrameOffsetFlags] & static_cast<uint8_t>(~kFrameFlagReleased)) == 0U;
}

/**
 * @brief Copy the header fields of the held frame into a Frame.
 *
 * The payload pointer in the Frame points into the parser's own buffer, so it is valid only
 * until the next call to feed().
 *
 * @param[out] frame  the decoded frame
 */
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

/**
 * @brief Take one received byte; report when it completes a good frame.
 *
 * A gap of more than kFrameGapMaxMs since the previous byte drops what was held first. A byte
 * that breaks the header restarts the search (startOver). A complete frame whose check value
 * does not match, or whose Flags bits 1 to 7 are not 0, is dropped whole: no answer, like a
 * damaged frame.
 *
 * @param[in]  byte    the received byte
 * @param[in]  now_ms  the millisecond the byte arrived
 * @param[out] frame   the decoded frame, written only when true is returned
 * @return true if this byte completed a frame with a matching check value and Flags bits 1 to 7
 *         equal to 0; false otherwise
 */
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

    bool good = checkValueOk() && flagsOk();
    if (good) {
        decode(frame);
    }
    dropHeld();   /* the bytes stay in place until the next frame overwrites them */
    return good;
}

/**
 * @brief Write a frame (header, payload, check value) into a buffer.
 *
 * The check value is the CRC-32 of the header and payload. Nothing is written when the payload
 * is too long or the frame does not fit.
 *
 * @param[in]  frame  the frame to write; frame.payload must hold frame.length bytes
 * @param[out] out    the buffer to write into
 * @param[in]  room   size of out, in bytes
 * @return the number of bytes written; 0 if frame.length is above kFramePayloadMax or the frame
 *         does not fit in room
 */
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

/**
 * @brief Write the answer payload: the sequence number answered, the kind and the code.
 *
 * @param[in]  answered  sequence number of the frame being answered
 * @param[in]  kind      the kind of answer
 * @param[in]  code      the answer code
 * @param[out] out       the payload
 */
void answerPayload(uint32_t answered, AnswerKind kind, AnswerCode code,
                   std::array<uint8_t, kAnswerPayloadBytes> &out)
{
    writeU32(&out[kAnswerOffsetSequence], answered);
    out[kAnswerOffsetKind] = static_cast<uint8_t>(kind);
    out[kAnswerOffsetCode] = static_cast<uint8_t>(code);
}
