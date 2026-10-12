/*
 * frame.c — frame parser and builder (Interfaces Sec. 4.0, Figure 2), answer payload (Sec. 4.2).
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
#include "frame.h"

#include "crc32.h"

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)((uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8));
}

static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

void frame_parser_init(frame_parser_t *parser)
{
    parser->count = 0U;
    parser->total = 0U;
    parser->last_ms = 0U;
}

static void drop_held(frame_parser_t *parser)
{
    parser->count = 0U;
    parser->total = 0U;
}

/* Drop what is held; keep the byte that broke the header if it can start a new frame. */
static void start_over(frame_parser_t *parser, uint8_t byte)
{
    drop_held(parser);
    if (byte == FRAME_SYNC_0) {
        parser->bytes[0] = byte;
        parser->count = 1U;
    }
}

/* Checks the header field that the byte just stored completes, if any. */
static bool header_so_far_ok(const frame_parser_t *parser)
{
    const uint8_t *b = parser->bytes;

    switch (parser->count) {
    case 1U:
        return b[0] == FRAME_SYNC_0;
    case 2U:
        return b[1] == FRAME_SYNC_1;
    case FRAME_OFFSET_VERSION + 1U:
        return b[FRAME_OFFSET_VERSION] == FRAME_VERSION;
    case FRAME_OFFSET_LENGTH + 2U:
        return read_u16(&b[FRAME_OFFSET_LENGTH]) <= FRAME_PAYLOAD_MAX;
    default:
        return true;
    }
}

static bool check_value_ok(const frame_parser_t *parser)
{
    uint32_t covered = parser->total - FRAME_CHECK_BYTES;

    return crc32_compute(parser->bytes, covered) == read_u32(&parser->bytes[covered]);
}

static void decode(const frame_parser_t *parser, frame_t *frame)
{
    const uint8_t *b = parser->bytes;

    frame->type = b[FRAME_OFFSET_TYPE];
    frame->destination = b[FRAME_OFFSET_DESTINATION];
    frame->flags = b[FRAME_OFFSET_FLAGS];
    frame->length = read_u16(&b[FRAME_OFFSET_LENGTH]);
    frame->sequence = read_u32(&b[FRAME_OFFSET_SEQUENCE]);
    frame->expiry = read_u32(&b[FRAME_OFFSET_EXPIRY]);
    frame->payload = &b[FRAME_HEADER_BYTES];
}

bool frame_parser_feed(frame_parser_t *parser, uint8_t byte, uint32_t now_ms, frame_t *frame)
{
    /* Unsigned subtraction stays right when the millisecond count wraps. */
    if ((parser->count > 0U) && ((now_ms - parser->last_ms) > FRAME_GAP_MAX_MS)) {
        drop_held(parser);
    }
    parser->last_ms = now_ms;
    parser->bytes[parser->count] = byte;
    parser->count++;

    if (!header_so_far_ok(parser)) {
        start_over(parser, byte);
        return false;
    }
    if (parser->count == (FRAME_OFFSET_LENGTH + 2U)) {
        parser->total = FRAME_HEADER_BYTES + read_u16(&parser->bytes[FRAME_OFFSET_LENGTH]) +
                        FRAME_CHECK_BYTES;
    }
    if ((parser->total == 0U) || (parser->count < parser->total)) {
        return false;   /* not complete yet */
    }

    bool good = check_value_ok(parser);
    if (good) {
        decode(parser, frame);
    }
    drop_held(parser);   /* the bytes stay in place until the next frame overwrites them */
    return good;
}

uint32_t frame_build(const frame_t *frame, uint8_t *out, uint32_t room)
{
    uint32_t covered = FRAME_HEADER_BYTES + (uint32_t)frame->length;

    if ((frame->length > FRAME_PAYLOAD_MAX) || ((covered + FRAME_CHECK_BYTES) > room)) {
        return 0U;
    }
    out[0] = FRAME_SYNC_0;
    out[1] = FRAME_SYNC_1;
    out[FRAME_OFFSET_VERSION] = FRAME_VERSION;
    out[FRAME_OFFSET_TYPE] = frame->type;
    out[FRAME_OFFSET_DESTINATION] = frame->destination;
    out[FRAME_OFFSET_FLAGS] = frame->flags;
    write_u16(&out[FRAME_OFFSET_LENGTH], frame->length);
    write_u32(&out[FRAME_OFFSET_SEQUENCE], frame->sequence);
    write_u32(&out[FRAME_OFFSET_EXPIRY], frame->expiry);
    for (uint32_t i = 0U; i < frame->length; i++) {
        out[FRAME_HEADER_BYTES + i] = frame->payload[i];
    }
    write_u32(&out[covered], crc32_compute(out, covered));
    return covered + FRAME_CHECK_BYTES;
}

void answer_payload(uint32_t answered, uint8_t kind, uint8_t code,
                    uint8_t out[ANSWER_PAYLOAD_BYTES])
{
    write_u32(&out[ANSWER_OFFSET_SEQUENCE], answered);
    out[ANSWER_OFFSET_KIND] = kind;
    out[ANSWER_OFFSET_CODE] = code;
}
