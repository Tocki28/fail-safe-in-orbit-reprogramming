/*
 * frame.h — the frame of the Interfaces document (Sec. 4.0, Figure 2) and the answer payload
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
 * No hardware here: the same file compiles for the board and, with the Mac's cc, for the
 * unit tests (test/controller/unit).
 *
 * Used by data link A (uart_link.c). REQ-099 asks the two data links to share no software.
 * Whether data link B (CAN, not on this bench) may use this file or needs its own frame code
 * is not decided yet.
 */
#ifndef FRAME_H
#define FRAME_H

#include <stdbool.h>
#include <stdint.h>

/* Field values (Interfaces Sec. 4.0, frame table). */
#define FRAME_SYNC_0              0xA5U
#define FRAME_SYNC_1              0x5AU
#define FRAME_VERSION             0x01U
#define FRAME_TYPE_COMMAND        0x01U
#define FRAME_TYPE_ANSWER         0x02U
#define FRAME_TYPE_REPORT         0x03U
#define FRAME_DEST_ONBOARD        0x01U
#define FRAME_DEST_CONTROLLER     0x02U
#define FRAME_DEST_GROUND         0x03U
#define FRAME_FLAG_RELEASED       0x01U

/* Sizes (Interfaces Sec. 4.0, Figure 2): 16-byte header, 0-1024 payload bytes, 4-byte check. */
#define FRAME_HEADER_BYTES        16U
#define FRAME_PAYLOAD_MAX         1024U
#define FRAME_CHECK_BYTES         4U
#define FRAME_MAX_BYTES           (FRAME_HEADER_BYTES + FRAME_PAYLOAD_MAX + FRAME_CHECK_BYTES)

/* Byte offsets in the header (Interfaces Sec. 4.0, Figure 2). */
#define FRAME_OFFSET_VERSION      2U
#define FRAME_OFFSET_TYPE         3U
#define FRAME_OFFSET_DESTINATION  4U
#define FRAME_OFFSET_FLAGS        5U
#define FRAME_OFFSET_LENGTH       6U
#define FRAME_OFFSET_SEQUENCE     8U
#define FRAME_OFFSET_EXPIRY       12U

/* A frame with a gap of more than 10 ms inside it is dropped (Interfaces Sec. 6.2). */
#define FRAME_GAP_MAX_MS          10U

/* Answer payload (Interfaces Sec. 4.2): sequence number answered (4), kind (1), code (1). */
#define ANSWER_PAYLOAD_BYTES      6U
#define ANSWER_OFFSET_SEQUENCE    0U
#define ANSWER_OFFSET_KIND        4U
#define ANSWER_OFFSET_CODE        5U
#define ANSWER_RECEIVED           0x01U
#define ANSWER_DONE               0x02U
#define ANSWER_REFUSED            0x03U
#define ANSWER_ALREADY_DONE       0x04U
/* Code in a DONE answer on success (Interfaces Sec. 4.2). */
#define ANSWER_CODE_SUCCESS       0x00U
/* Refusal reasons (Interfaces Sec. 4.2 table). Only the codes used so far; the rest come with
 * the command checker (step 2b). */
#define REFUSED_IMPOSSIBLE        0x06U   /* impossible values, or an unknown command (REQ-028) */
#define REFUSED_TRIAL             0x07U   /* no new firmware during a trial (REQ-050) */
#define REFUSED_DAMAGED           0x08U   /* image damaged (REQ-005, REQ-044) */
#define REFUSED_NOT_OPERATOR      0x09U   /* image not from the operator (REQ-006, REQ-043) */
#define REFUSED_OTHER_CONTROLLER  0x0BU   /* image built for another controller (REQ-046) */
#define REFUSED_OTHER_SLOT        0x0EU   /* image built for the other slot (REQ-005) */

/* One frame's fields. payload points at length bytes. */
typedef struct {
    uint8_t type;
    uint8_t destination;
    uint8_t flags;
    uint16_t length;
    uint32_t sequence;
    uint32_t expiry;
    const uint8_t *payload;
} frame_t;

/* The parser's memory: one frame's bytes as they arrive. count never exceeds
 * FRAME_MAX_BYTES: Length is checked against 1024 before more bytes are taken. */
typedef struct {
    uint8_t bytes[FRAME_MAX_BYTES];
    uint32_t count;     /* bytes held so far */
    uint32_t total;     /* the whole frame's size once Length has arrived; 0 before */
    uint32_t last_ms;   /* arrival time of the last byte held */
} frame_parser_t;

void frame_parser_init(frame_parser_t *parser);

/* Take one byte that arrived at now_ms (a millisecond count that may wrap). True when this
 * byte completes a frame that passed every check; *frame then describes it, and its payload
 * points into the parser, valid until the next call. */
bool frame_parser_feed(frame_parser_t *parser, uint8_t byte, uint32_t now_ms, frame_t *frame);

/* Write *frame as bytes into out (room bytes available), check value included. Returns the
 * frame's size, or 0 if the payload is longer than 1024 bytes or out is too small. */
uint32_t frame_build(const frame_t *frame, uint8_t *out, uint32_t room);

/* The 6-byte answer payload (Interfaces Sec. 4.2). */
void answer_payload(uint32_t answered, uint8_t kind, uint8_t code,
                    uint8_t out[ANSWER_PAYLOAD_BYTES]);

#endif /* FRAME_H */
