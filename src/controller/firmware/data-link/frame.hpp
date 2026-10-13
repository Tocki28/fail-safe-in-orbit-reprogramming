/*
 * frame.hpp — the frame of the Interfaces document (Sec. 4.0, Figure 2) and the answer payload
 * (Sec. 4.2): a parser that takes one received byte at a time, and a builder.
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
 *     16+L      4  Check value = CRC-32 of every byte before it (common/crc32.c)
 * Multi-byte fields are little-endian (Interfaces Sec. 4.0).
 *
 * No hardware here: the same file compiles for the board and, with the Mac's clang++, for the
 * unit tests (test/controller/unit).
 *
 * Used by data link A (uart_link.cpp). REQ-099 asks the two data links to share no software.
 * Whether data link B (CAN, not on this bench) may use this file or needs its own frame code
 * is not decided yet.
 *
 * C++: class FrameParser (C: frame_parser_t and the frame_parser_ functions); constexpr
 * constants instead of #define, each named after its C macro (FRAME_HEADER_BYTES becomes
 * kFrameHeaderBytes); enum class for Type, Destination and the answer kinds and codes, so a
 * code cannot be passed where a kind is expected.
 */
#ifndef FRAME_HPP
#define FRAME_HPP

#include <array>
#include <cstdint>

/* C++: constexpr instead of #define. A constexpr has a type and obeys scope; the compiler
 * computes it, so it costs no RAM and no code, as the macro did. */

/* Field values (Interfaces Sec. 4.0, frame table). */
constexpr uint8_t kFrameSync0 = 0xA5U;
constexpr uint8_t kFrameSync1 = 0x5AU;
constexpr uint8_t kFrameVersion = 0x01U;
constexpr uint8_t kFrameFlagReleased = 0x01U;

/* Sizes (Interfaces Sec. 4.0, Figure 2): 16-byte header, 0-1024 payload bytes, 4-byte check. */
constexpr uint32_t kFrameHeaderBytes = 16U;
constexpr uint32_t kFramePayloadMax = 1024U;
constexpr uint32_t kFrameCheckBytes = 4U;
constexpr uint32_t kFrameMaxBytes = kFrameHeaderBytes + kFramePayloadMax + kFrameCheckBytes;

/* Byte offsets in the header (Interfaces Sec. 4.0, Figure 2). */
constexpr uint32_t kFrameOffsetVersion = 2U;
constexpr uint32_t kFrameOffsetType = 3U;
constexpr uint32_t kFrameOffsetDestination = 4U;
constexpr uint32_t kFrameOffsetFlags = 5U;
constexpr uint32_t kFrameOffsetLength = 6U;
constexpr uint32_t kFrameOffsetSequence = 8U;
constexpr uint32_t kFrameOffsetExpiry = 12U;

/* A frame with a gap of more than 10 ms inside it is dropped (Interfaces Sec. 6.2). */
constexpr uint32_t kFrameGapMaxMs = 10U;

/* Type and Destination field values (Interfaces Sec. 4.0, frame table). The underlying type is
 * fixed (uint8_t), so a received byte with any other value is still a valid FrameType or
 * Destination: it matches none of the names and is ignored, as in the C version. */
enum class FrameType : uint8_t {
    kCommand = 0x01U,
    kAnswer = 0x02U,
    kReport = 0x03U,
};

enum class Destination : uint8_t {
    kOnboard = 0x01U,
    kController = 0x02U,
    kGround = 0x03U,
};

/* Answer payload (Interfaces Sec. 4.2): sequence number answered (4), kind (1), code (1). */
constexpr uint32_t kAnswerPayloadBytes = 6U;
constexpr uint32_t kAnswerOffsetSequence = 0U;
constexpr uint32_t kAnswerOffsetKind = 4U;
constexpr uint32_t kAnswerOffsetCode = 5U;

enum class AnswerKind : uint8_t {
    kReceived = 0x01U,
    kDone = 0x02U,
    kRefused = 0x03U,
    kAlreadyDone = 0x04U,
};

/* The code byte of an answer: 0x00 in a DONE on success, or a refusal reason (Interfaces
 * Sec. 4.2 table). Only the codes used so far; the rest come with the command checker (step
 * 2b). [[nodiscard]] on the type: the compiler warns wherever a returned code is ignored. */
enum class [[nodiscard]] AnswerCode : uint8_t {
    kSuccess = 0x00U,
    kNotArmed = 0x01U,          /* dangerous command without a valid arming command (REQ-029) */
    kArmExpired = 0x02U,        /* arming command expired (REQ-030) */
    kMode = 0x05U,              /* not allowed in the current mode (REQ-027) */
    kImpossible = 0x06U,        /* impossible values, or an unknown command (REQ-028) */
    kTrial = 0x07U,             /* no new firmware during a trial (REQ-050) */
    kDamaged = 0x08U,           /* image damaged (REQ-005, REQ-044) */
    kNotOperator = 0x09U,       /* image not from the operator (REQ-006, REQ-043) */
    kOtherController = 0x0BU,   /* image built for another controller (REQ-046) */
    kOtherSlot = 0x0EU,         /* image built for the other slot (REQ-005) */
    kTestsNotPassed = 0x0FU,    /* trial tests not passed (REQ-012) */
};

/* One frame's fields. payload points at length bytes. */
struct Frame {
    FrameType type;
    Destination destination;
    uint8_t flags;
    uint16_t length;
    uint32_t sequence;
    uint32_t expiry;
    const uint8_t *payload;
};

class FrameParser {
public:
    /* Forget any partial frame (C: frame_parser_init). A new parser starts empty. */
    void reset();

    /* Take one byte that arrived at now_ms (a millisecond count that may wrap). True when this
     * byte completes a frame that passed every check; `frame` then describes it, and its
     * payload points into the parser, valid until the next call. */
    [[nodiscard]] bool feed(uint8_t byte, uint32_t now_ms, Frame &frame);

private:
    void dropHeld();
    void startOver(uint8_t byte);
    bool headerSoFarOk() const;
    bool checkValueOk() const;
    void decode(Frame &frame) const;

    /* The parser's memory: one frame's bytes as they arrive. count_ never exceeds
     * kFrameMaxBytes: Length is checked against 1024 before more bytes are taken. */
    std::array<uint8_t, kFrameMaxBytes> bytes_{};
    uint32_t count_ = 0U;     /* bytes held so far */
    uint32_t total_ = 0U;     /* the whole frame's size once Length has arrived; 0 before */
    uint32_t last_ms_ = 0U;   /* arrival time of the last byte held */
};

/* Write `frame` as bytes into out (room bytes available), check value included. Returns the
 * frame's size, or 0 if the payload is longer than 1024 bytes or out is too small. */
[[nodiscard]] uint32_t buildFrame(const Frame &frame, uint8_t *out, uint32_t room);

/* The 6-byte answer payload (Interfaces Sec. 4.2). C++: std::array of the exact size, so the
 * compiler checks that the caller's buffer is 6 bytes (C: a pointer, size unchecked). */
void answerPayload(uint32_t answered, AnswerKind kind, AnswerCode code,
                   std::array<uint8_t, kAnswerPayloadBytes> &out);

#endif /* FRAME_HPP */
