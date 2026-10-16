/*
 * fz_command.cpp — fuzz target 2: the command payloads the controller takes from the link,
 * through the real interfaces: UpdateReceiver::isUpload, parse and accept [update_receiver.hpp:
 * 74-80]; ArmState::isArm and parse [arm_state.hpp:52-55]; TestImage::isTestImage and accept
 * [test_image.hpp:51-54]; and the mode manager's intake (acceptDangerous, acceptLeaveFallback,
 * acceptTestImage), driven the way test/controller/unit/test_mode.cpp drives it.
 *
 * The routing is main.cpp's own: the build extracts the functions handleUpload, handleTestImage,
 * routeUpload, handleArm, handleDangerous, handleLeaveFallback, isGetStatus and handle from
 * src/controller/firmware/main.cpp into build/fz_route.inc and this file includes them
 * (the Makefile fails if one is missing), so the routing under test is the flight text, not a
 * copy that can drift. The chip is replaced by the unit tests' fakes (fake_port.cpp,
 * fake_mode_port.cpp, fake_uart_link.cpp), compiled from their place.
 *
 * What is checked after every command (the rule of CODE_RULES A5, A6, B1, B4):
 *   - a byte from the link never reaches the fault reaction (fz_fault records the call);
 *   - a dangerous command is answered RECEIVED only after an ARM that names this command and
 *     this sequence number, less than 30 s old, and the ARM is used up (REQ-029, REQ-030;
 *     Interfaces Sec. 5.2); mode and size rules of the command table (Interfaces Sec. 5.1,
 *     lines 357-364 of docs/interfaces/main.tex);
 *   - flash is erased or written only in the slot that is neither running nor confirmed, with a
 *     valid boot state and no trial, never for image number 0xFFFF (REQ-007, REQ-050, REQ-139);
 *     the bootloader, the running slot and the last-resort region never change;
 *   - an UPLOAD END or SWITCH answered DONE is confirmed by the bootloader's own, independent
 *     image check (bl_image_check.c): a damaged, unsigned or foreign image is never accepted;
 *   - the transfer rules: BEGIN length from 192 to 40960, a CHUNK at exactly the next byte, END
 *     only after the last byte; the test image is accepted only with a right CRC-32.
 * A SWITCH of a given-up image is refused 0x13 (REQ-019, decision 30).
 *
 * One input is a case: a start state, then a short script of commands. The count of inputs is
 * the number of command payloads routed. Each case is a pure function of (seed, case index).
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "arm_state.hpp"
#include "backup_layout.h"
#include "boot_slot.hpp"
#include "data-link/frame.hpp"
#include "fake_port.hpp"
#include "fake_uart_link.hpp"
#include "frame_sender.hpp"
#include "fz_common.h"
#include "fz_fault.h"
#include "health_monitor.hpp"
#include "image_format.h"
#include "layout.h"
#include "mode_manager.hpp"
#include "test_image.hpp"
#include "uart_link.hpp"
#include "update_receiver.hpp"
#include "updater/crc32.hpp"

extern "C" {
#include "bl_image_check.h"
}

namespace {

/* ---- Constants -------------------------------------------------------------------------- */

/* Payload room: the largest payload a frame carries (Interfaces Sec. 4.0, 1024 bytes). */
constexpr uint32_t kPayloadMax = kFramePayloadMax;
/* Identifiers of the command table [docs/interfaces/main.tex, lines 357-364], written here from
 * the document and not taken from the firmware's headers, so a wrong value there shows. */
constexpr uint8_t kIdBegin = 0x01U;
constexpr uint8_t kIdChunk = 0x02U;
constexpr uint8_t kIdEnd = 0x03U;
constexpr uint8_t kIdCancel = 0x04U;
constexpr uint8_t kIdArm = 0x0DU;
constexpr uint8_t kIdSwitch = 0x20U;
constexpr uint8_t kIdReturn = 0x21U;
constexpr uint8_t kIdStartFailed = 0x22U;
constexpr uint8_t kIdLeaveFallback = 0x23U;
constexpr uint8_t kIdGetStatus = 0x25U;
constexpr uint8_t kIdConfirm = 0x26U;
/* Answer kinds and codes [Interfaces Sec. 4.2; frame.hpp]. */
constexpr uint8_t kKindReceived = 0x01U;
constexpr uint8_t kKindDone = 0x02U;
constexpr uint8_t kCodeOk = 0x00U;
constexpr uint8_t kCodeTestsNotPassed = 0x0FU;
/* The ARM lives 30 s [arm_state.hpp:41, bench value for TBD-arm, owner decision Q]. */
constexpr uint32_t kArmLifeMs = 30000U;
constexpr uint16_t kTestImageId = 0xFFFFU;
constexpr uint32_t kImageMin = 192U;   /* header 64 + two signatures 128 [layout.h; Sec. 5.5] */
constexpr uint32_t kChunkData = 256U;
constexpr uint32_t kOpsMax = 400U;
constexpr uint32_t kAnswersMax = 8U;
/* Chances in thousandths, chosen for this audit (no technical reason). */
constexpr uint32_t kShareUpload = 150U;
constexpr uint32_t kShareDangerous = 400U;
constexpr uint32_t kShareTestImage = 100U;
constexpr uint32_t kShareBackup = 50U;
constexpr uint32_t kChanceMutate = 600U;
constexpr uint32_t kChanceOddHeader = 50U;
constexpr uint32_t kChanceFlashFault = 30U;
constexpr uint32_t kChancePoll = 300U;
constexpr uint32_t kChanceWrapStart = 200U;
/* Files kept as image seeds. */
constexpr uint32_t kImagesMax = 24U;
constexpr uint32_t kImageBytesMax = SLOT_BYTES;

constexpr fz_field_t kCmdFields[] = {{1U, 2U}, {3U, 4U}, {1U, 1U}, {2U, 1U}, {3U, 1U}, {6U, 1U},
                                     {2U, 4U}};
constexpr uint32_t kCmdFieldCount = sizeof(kCmdFields) / sizeof(kCmdFields[0]);

/* ---- The controller under test ---------------------------------------------------------- */

/* A clock that main.cpp's functions read as core_clock.tickMs(). */
struct FakeClock {
    uint32_t now;
    uint32_t tickMs() const
    {
        return now;
    }
};

FakeClock core_clock;
UartLink uart_link;
UpdateReceiver updater;
FrameSender sender{uart_link};
TestImage test_image;
ModeManager mode_manager{sender, test_image};
ArmState arm_state;

/* main.cpp's constants and functions, extracted at build time (see the file comment). */
constexpr uint8_t kCmdGetStatus = 0x25U;
constexpr uint16_t kGetStatusBytes = 1U;
constexpr AnswerCode kReceivedCode = static_cast<AnswerCode>(0x00U);

#include "fz_route.inc"

/* ---- Image seeds ------------------------------------------------------------------------ */

struct ImageSeed {
    uint8_t bytes[kImageBytesMax];
    uint32_t size;
    Slot made_for;   /* the slot the file name says, kNone for any */
    bool build_firmware;   /* the build's own signed firmware-A.img or firmware-B.img */
};

ImageSeed g_images[kImagesMax];
uint32_t g_image_count;
fz_guarded_t g_guard;

/* ---- The model -------------------------------------------------------------------------- */

struct Model {
    bool armed;
    uint8_t arm_cmd;
    uint32_t arm_seq;
    uint32_t arm_ms;
    bool up_active;
    uint32_t up_len;
    uint32_t up_written;
    bool ti_active;
    uint32_t ti_len;
    uint32_t ti_received;
    uint8_t ti_bytes[kTestImageMaxBytes];
};

Model g_model;

/* One command to route. */
struct Op {
    FrameType type;
    Destination destination;
    uint8_t flags;
    uint32_t sequence;
    uint32_t advance_ms;
    uint8_t payload[kPayloadMax];
    uint32_t length;
};

/* One answer read back from the link. */
struct Answer {
    uint32_t answered;
    uint8_t kind;
    uint8_t code;
};

Slot g_running;
uint8_t g_reference[kFakeFlashBytes];
uint32_t g_now;
uint32_t g_next_seq;
uint64_t g_case_index;
uint64_t g_payloads;
uint64_t g_cases;
uint64_t g_accepted_received;
uint64_t g_switch_done;
uint64_t g_end_done;
uint64_t g_testimage_passed;
FrameParser g_reader;
uint8_t g_case_log[4096];
size_t g_case_log_len;

/**
 * @brief Writes a 32-bit value little-endian (Interfaces Sec. 4.0).
 *
 * @param[out]    p  four bytes
 * @param[in]     v  the value
 */
void putU32(uint8_t *p, uint32_t v)
{
    for (uint32_t i = 0U; i < 4U; i++) {
        p[i] = static_cast<uint8_t>(v >> (8U * i));
    }
}

/**
 * @brief Writes a 16-bit value little-endian (Interfaces Sec. 4.0).
 *
 * @param[out]    p  two bytes
 * @param[in]     v  the value
 */
void putU16(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

/**
 * @brief Reads a 32-bit little-endian value.
 *
 * @param[in] p  four bytes
 * @return the value
 */
uint32_t getU32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

/**
 * @brief Loads the image files of the given directories as seeds.
 *
 * @param[in]     options  run options (--dir paths)
 */
void loadImages(const fz_options_t *options)
{
    for (uint32_t d = 0U; d < options->dir_count; d++) {
        char list[512];
        (void)snprintf(list, sizeof list, "ls %s/*.img 2>/dev/null", options->dirs[d]);
        FILE *pipe = popen(list, "r");
        char path[512];

        while ((pipe != nullptr) && (fgets(path, sizeof path, pipe) != nullptr) &&
               (g_image_count < kImagesMax)) {
            path[strcspn(path, "\n")] = '\0';
            FILE *file = fopen(path, "rb");
            if (file == nullptr) {
                continue;
            }
            ImageSeed &seed = g_images[g_image_count];
            seed.size = static_cast<uint32_t>(fread(seed.bytes, 1U, kImageBytesMax, file));
            (void)fclose(file);
            seed.made_for = Slot::kNone;
            seed.build_firmware = (strstr(path, "/firmware-A.img") != nullptr) ||
                                  (strstr(path, "/firmware-B.img") != nullptr);
            if ((strstr(path, "slotA") != nullptr) || (strstr(path, "firmware-A") != nullptr)) {
                seed.made_for = Slot::kA;
            } else if ((strstr(path, "slotB") != nullptr)
                       || (strstr(path, "firmware-B") != nullptr)) {
                seed.made_for = Slot::kB;
            }
            if (seed.size > 0U) {
                g_image_count++;
            }
        }
        if (pipe != nullptr) {
            (void)pclose(pipe);
        }
    }
    if (g_image_count == 0U) {
        fz_log("no image seeds found in the --dir directories");
    }
}

/**
 * @brief Picks an image seed, preferring one made for `slot`.
 *
 * @param[in,out] rng   the generator
 * @param[in]     slot  the slot it will be written to
 * @return the seed, or nullptr if there is none
 */
const ImageSeed *pickImage(fz_rng_t *rng, Slot slot)
{
    if (g_image_count == 0U) {
        return nullptr;
    }
    for (uint32_t tries = 0U; tries < 8U; tries++) {
        const ImageSeed &s = g_images[fz_rng_below(rng, g_image_count)];
        if ((s.made_for == slot) || fz_rng_chance(rng, 150U)) {
            return &s;
        }
    }
    return &g_images[fz_rng_below(rng, g_image_count)];
}

/**
 * @brief Returns a slot's bytes in the fake flash.
 *
 * @param[in]     slot  Slot::kA or Slot::kB
 * @return pointer to the slot's first byte
 */
uint8_t *slotBytes(Slot slot)
{
    uint32_t start = (slot == Slot::kA) ? kSlotAStart : kSlotBStart;

    return &fake.flash[start - kFlashStart];
}

/* ---- Start state ------------------------------------------------------------------------ */

/**
 * @brief Sets the fakes for one case: flash patterns, boot state, the images, the controller's
 * start (main.cpp: updater.init, mode_manager.start).
 *
 * @param[in,out] rng  the generator
 */
void startCase(fz_rng_t *rng)
{
    uint32_t shape = fz_rng_below(rng, 100U);

    fakeReset();
    fakeLinkReset();
    memset(&g_model, 0, sizeof g_model);
    g_running = fz_rng_chance(rng, 500U) ? Slot::kA : Slot::kB;
    Slot other = otherSlot(g_running);
    fake.allowed_slot = other;
    fake.state.confirmed = stateCode(g_running);
    fake.state.trial = BOOT_SLOT_NONE;
    fake.state.failed_starts = 0U;
    fake.state.flags = 0U;
    fake.state.given_up_check = 0U;
    if (shape < 30U) {   /* TRIAL: the running slot is the trial */
        fake.state.confirmed = stateCode(other);
        fake.state.trial = stateCode(g_running);
        fake.state.failed_starts = static_cast<uint8_t>(1U + fz_rng_below(rng, 3U));
    } else if (shape < 50U) {   /* FALLBACK */
        fake.state.flags = BOOT_STATE_FLAG_FALLBACK;
        fake.state.given_up_check = fz_rng_chance(rng, 500U)
            ? static_cast<uint32_t>(fz_rng_next(rng)) : 0U;
    } else if (shape < 55U) {   /* no valid boot state */
        fake.state_valid = false;
    } else if (shape < 60U) {   /* running is neither confirmed nor trial */
        fake.state.confirmed = stateCode(other);
    } else if (shape < 72U) {   /* a SWITCH has stored a trial on the other slot; no restart yet */
        fake.state.trial = stateCode(other);
        fake.state.failed_starts = 0U;
    }
    for (uint32_t i = 0U; i < g_image_count; i++) {
        if (g_images[i].build_firmware && (g_images[i].made_for == g_running)) {
            memset(slotBytes(g_running), 0xFF, kSlotBytes);
            memcpy(slotBytes(g_running), g_images[i].bytes, g_images[i].size);
        }
    }
    if (fz_rng_chance(rng, 400U)) {   /* a good image already in the other slot */
        const ImageSeed *o = pickImage(rng, other);
        if (o != nullptr) {
            memset(slotBytes(other), 0xFF, kSlotBytes);
            memcpy(slotBytes(other), o->bytes, o->size);
        }
    } else if (fz_rng_chance(rng, 500U)) {
        memset(slotBytes(other), 0xFF, kSlotBytes);
    }
    if (fz_rng_chance(rng, kChanceFlashFault)) {
        fake.fail_erase = fz_rng_chance(rng, 400U);
        fake.fail_program = fz_rng_chance(rng, 400U);
        fake.ecc_error = fz_rng_chance(rng, 300U);
        fake.fail_state_write = fz_rng_chance(rng, 400U);
    }
    g_now = fz_rng_chance(rng, kChanceWrapStart) ? (0xFFFFFF00U + fz_rng_below(rng, 200U))
                                                  : fz_rng_below(rng, 1000000U);
    g_next_seq = static_cast<uint32_t>(fz_rng_next(rng));
    core_clock.now = g_now;
    new (&uart_link) UartLink();
    new (&sender) FrameSender(uart_link);
    new (&test_image) TestImage();
    new (&mode_manager) ModeManager(sender, test_image);
    arm_state = ArmState();
    updater.init(g_running);
    mode_manager.start(g_running, g_now, fz_rng_chance(rng, 50U));
    fakeLinkReset();
    fz_fault_clear();
    memcpy(g_reference, fake.flash.data(), kFakeFlashBytes);
}

/* ---- Building commands ------------------------------------------------------------------ */

/**
 * @brief Starts an op with a normal header and the next sequence number.
 *
 * @param[in,out] rng  the generator
 * @param[out]    op   the op
 */
void newOp(fz_rng_t *rng, Op *op)
{
    op->type = FrameType::kCommand;
    op->destination = Destination::kController;
    op->flags = 0U;
    op->sequence = g_next_seq++;
    op->advance_ms = fz_rng_below(rng, 40U);
    op->length = 0U;
    if (fz_rng_chance(rng, kChanceOddHeader)) {
        op->type = static_cast<FrameType>(fz_rng_below(rng, 6U));
        op->destination = static_cast<Destination>(fz_rng_below(rng, 6U));
        op->flags = static_cast<uint8_t>(fz_rng_next(rng));
    }
    if (fz_rng_chance(rng, 20U)) {
        op->advance_ms = fz_rng_chance(rng, 500U) ? (kArmLifeMs - 5U + fz_rng_below(rng, 12U))
                                                  : fz_rng_below(rng, 130000U);
    }
}

/**
 * @brief Builds an UPLOAD BEGIN payload [Interfaces Sec. 5.1].
 *
 * @param[out]    op      the op
 * @param[in]     number  image number
 * @param[in]     length  image length
 */
void buildBegin(Op *op, uint16_t number, uint32_t length)
{
    op->payload[0] = kIdBegin;
    putU16(&op->payload[1], number);
    putU32(&op->payload[3], length);
    op->length = 7U;
}

/**
 * @brief Builds an UPLOAD CHUNK payload [Interfaces Sec. 5.1].
 *
 * @param[out]    op      the op
 * @param[in]     number  image number
 * @param[in]     offset  offset of the data
 * @param[in]     data    the data
 * @param[in]     bytes   its size, 1 to 256
 */
void buildChunk(Op *op, uint16_t number, uint32_t offset, const uint8_t *data, uint32_t bytes)
{
    op->payload[0] = kIdChunk;
    putU16(&op->payload[1], number);
    putU32(&op->payload[3], offset);
    memcpy(&op->payload[7], data, bytes);
    op->length = 7U + bytes;
}

/**
 * @brief Builds an UPLOAD END or CANCEL payload.
 *
 * @param[out]    op      the op
 * @param[in]     id      kIdEnd or kIdCancel
 * @param[in]     number  image number
 */
void buildShort(Op *op, uint8_t id, uint16_t number)
{
    op->payload[0] = id;
    putU16(&op->payload[1], number);
    op->length = 3U;
}

/**
 * @brief Builds an ARM payload [Interfaces Sec. 5.1: command identifier (1), its sequence (4)].
 *
 * @param[out]    op        the op
 * @param[in]     command   the command it allows
 * @param[in]     sequence  that command's sequence number
 */
void buildArm(Op *op, uint8_t command, uint32_t sequence)
{
    op->payload[0] = kIdArm;
    op->payload[1] = command;
    putU32(&op->payload[2], sequence);
    op->length = 6U;
}

/**
 * @brief Builds a dangerous command payload.
 *
 * @param[in,out] rng  the generator
 * @param[out]    op   the op
 * @param[in]     id   kIdSwitch, kIdReturn, kIdStartFailed or kIdConfirm
 */
void buildDangerous(fz_rng_t *rng, Op *op, uint8_t id)
{
    op->payload[0] = id;
    op->length = 1U;
    if (id == kIdStartFailed) {
        op->payload[1] = static_cast<uint8_t>(1U + fz_rng_below(rng, 3U));
        op->length = 2U;
    }
}

/**
 * @brief Mutates an op's payload, and sometimes its header.
 *
 * @param[in,out] rng  the generator
 * @param[in,out] op   the op
 */
void mutateOp(fz_rng_t *rng, Op *op)
{
    static uint8_t spare[kPayloadMax];

    for (uint32_t i = 0U; i < kPayloadMax; i++) {
        spare[i] = static_cast<uint8_t>(fz_rng_next(rng));
    }
    op->length = static_cast<uint32_t>(fz_mutate(rng, op->payload, op->length, kPayloadMax,
                                                  kCmdFields, kCmdFieldCount, spare, sizeof spare));
    if (fz_rng_chance(rng, 100U)) {
        op->sequence = fz_rng_edge32(rng);
    }
}

/**
 * @brief Builds one op from the catalogue of commands, mutated with some probability.
 *
 * @param[in,out] rng  the generator
 * @param[out]    op   the op
 */
void buildRandomOp(fz_rng_t *rng, Op *op)
{
    static uint8_t data[kChunkData];
    uint32_t pick = fz_rng_below(rng, 16U);
    uint16_t number = fz_rng_chance(rng, 300U) ? kTestImageId
        : static_cast<uint16_t>(fz_rng_below(rng, 4U));
    uint32_t bytes = 1U + fz_rng_below(rng, kChunkData);

    newOp(rng, op);
    for (uint32_t i = 0U; i < kChunkData; i++) {
        data[i] = static_cast<uint8_t>(fz_rng_next(rng));
    }
    if (pick < 2U) {
        buildBegin(op, number, fz_rng_chance(rng, 500U) ? fz_rng_edge32(rng)
                   : (kImageMin + fz_rng_below(rng, 3000U)));
    } else if (pick < 4U) {
        buildChunk(op, number, fz_rng_chance(rng, 500U) ? 0U : fz_rng_edge32(rng), data, bytes);
    } else if (pick < 6U) {
        buildShort(op, kIdEnd, number);
    } else if (pick < 7U) {
        buildShort(op, kIdCancel, number);
    } else if (pick < 9U) {
        buildArm(op, static_cast<uint8_t>(fz_rng_chance(rng, 700U)
                                          ? (kIdSwitch + fz_rng_below(rng, 7U)) : fz_rng_next(rng)),
                 fz_rng_chance(rng, 500U) ? g_next_seq : fz_rng_edge32(rng));
    } else if (pick < 12U) {
        static const uint8_t ids[] = {kIdSwitch, kIdReturn, kIdStartFailed, kIdConfirm};
        buildDangerous(rng, op, ids[fz_rng_below(rng, 4U)]);
    } else if (pick < 13U) {
        op->payload[0] = kIdLeaveFallback;
        op->length = 1U;
    } else if (pick < 14U) {
        op->payload[0] = kIdGetStatus;
        op->length = 1U;
    } else {
        op->length = fz_rng_below(rng, 40U);
        for (uint32_t i = 0U; i < op->length; i++) {
            op->payload[i] = static_cast<uint8_t>(fz_rng_next(rng));
        }
    }
    if (fz_rng_chance(rng, kChanceMutate)) {
        mutateOp(rng, op);
    }
}

/* ---- Running a command and checking it ------------------------------------------------- */

/**
 * @brief Reads the answers the controller sent since byte `from` of the fake link.
 *
 * @param[in]     from     first byte to read
 * @param[out]    answers  room for kAnswersMax answers
 * @return number of answers read
 */
uint32_t readAnswers(size_t from, Answer *answers)
{
    uint32_t count = 0U;
    Frame f{};

    g_reader.reset();
    for (size_t i = from; i < fake_link.sent_bytes; i++) {
        if (g_reader.feed(fake_link.sent[i], 0U, f) && (f.type == FrameType::kAnswer) &&
            (f.length == kAnswerPayloadBytes) && (count < kAnswersMax)) {
            answers[count].answered = getU32(&f.payload[kAnswerOffsetSequence]);
            answers[count].kind = f.payload[kAnswerOffsetKind];
            answers[count].code = f.payload[kAnswerOffsetCode];
            count++;
        }
    }
    return count;
}

/**
 * @brief Records a failure of the case with a note.
 *
 * @param[in]     kind   failure kind
 * @param[in]     what   what was wrong
 * @param[in]     op     the op that was routed
 */
void failOp(const char *kind, const char *what, const Op &op)
{
    char note[240];

    (void)snprintf(note, sizeof note, "%s [op: type %u dest %u flags 0x%02X seq %u id 0x%02X "
                   "length %u running %u]", what, static_cast<unsigned>(op.type),
                   static_cast<unsigned>(op.destination), op.flags, op.sequence,
                   (op.length > 0U) ? op.payload[0] : 0U, op.length,
                   static_cast<unsigned>(g_running));
    fz_fail(kind, g_case_index, op.payload, op.length, note);
}

/**
 * @brief Checks that the image in a slot passes the bootloader's own, independent checks.
 *
 * @param[in]     slot  Slot::kA or Slot::kB
 * @return true if contents and both signatures pass
 */
bool blAccepts(Slot slot)
{
    uint32_t start = (slot == Slot::kA) ? kSlotAStart : kSlotBStart;
    uint32_t signed_bytes = 0U;
    image_result_t r = bl_image_check_contents(slotBytes(slot), start, kSlotBytes, IMAGE_KIND_MAIN,
                                               &signed_bytes);

    return (r == IMAGE_OK) && (bl_image_check_signatures(slotBytes(slot),
                                                         signed_bytes) == IMAGE_OK);
}

/**
 * @brief Checks the dangerous command rules on the answers to one op.
 *
 * @param[in]     op        the op
 * @param[in]     answers   the answers it produced
 * @param[in]     n         their number
 * @param[in]     mode      the mode before the op
 * @param[in]     tests_ok  trialTestsPassed() before the op
 * @param[in]     state     the boot state before the op
 */
void checkDangerous(const Op &op, const Answer *answers, uint32_t n, HealthMode mode, bool tests_ok,
                    const boot_state_t &state)
{
    uint8_t id = op.payload[0];
    bool received = false;
    bool done_ok = false;
    bool refused_tests = false;

    for (uint32_t i = 0U; i < n; i++) {
        received = received || ((answers[i].kind == kKindReceived)
                                && (answers[i].answered == op.sequence));
        done_ok = done_ok || ((answers[i].kind == kKindDone) && (answers[i].code == kCodeOk));
        refused_tests = refused_tests || (answers[i].code == kCodeTestsNotPassed);
    }
    if (received) {
        g_accepted_received++;
        bool size_ok = (id == kIdStartFailed) ? (op.length == 2U) : (op.length == 1U);
        bool trial_cmd = (id == kIdReturn) || (id == kIdConfirm);
        bool mode_ok = trial_cmd ? (mode == HealthMode::kTrial)
                                 : ((mode == HealthMode::kOperate)
                                    || (mode == HealthMode::kFallback));
        bool arm_ok = g_model.armed && (g_model.arm_cmd == id)
            && (g_model.arm_seq == op.sequence) &&
                      ((g_now - g_model.arm_ms) < kArmLifeMs);
        if (!arm_ok) {
            failOp("no-arm",
                   "dangerous command answered RECEIVED without a valid ARM (or ARM reused)", op);
        }
        if (!size_ok) {
            failOp("size", "dangerous command of the wrong size answered RECEIVED", op);
        }
        if (!mode_ok) {
            failOp("mode", "dangerous command answered RECEIVED in a mode the table does not allow",
                   op);
        }
        if ((id == kIdConfirm) && !tests_ok) {
            failOp("confirm-tests", "CONFIRM answered RECEIVED with trial tests not passed", op);
        }
        if ((id == kIdStartFailed) && (size_ok) &&
            ((op.payload[1] < 1U) || (op.payload[1] > 2U)
             || (op.payload[1] == stateCode(g_running)))) {
            failOp("start-failed-copy",
                   "START FAILED accepted with a copy that is not the other slot", op);
        }
        g_model.armed = false;
    } else if (refused_tests) {
        /* the ARM is used up before the tests are asked (acceptDangerous) */
        g_model.armed = false;
    }
    if (done_ok && ((id == kIdSwitch) || (id == kIdStartFailed))) {
        Slot other = otherSlot(g_running);
        uint32_t body = getU32(&slotBytes(other)[IMAGE_OFFSET_BODY_CHECK]);

        g_switch_done++;
        if (!blAccepts(other)) {
            failOp("switch-bad-image",
                   "SWITCH/START FAILED DONE but the bootloader's check refuses the other slot",
                   op);
        }
        if ((id == kIdSwitch) && state.flags != 0U && (state.given_up_check != 0U) &&
            (body == state.given_up_check)) {
            failOp("switch-given-up",
                   "SWITCH DONE for an image carrying the given-up mark (REQ-019)", op);
        }
    }
}

/**
 * @brief Checks the upload and test-image transfer rules on the answers to one op, and the flash
 * safety rules.
 *
 * @param[in]     op        the op
 * @param[in]     answers   the answers
 * @param[in]     n         their number
 * @param[in]     state     the boot state before the op
 * @param[in]     valid     whether it was valid
 * @param[in]     wrote     erases or programs changed during the op
 */
void checkUpload(const Op &op, const Answer *answers, uint32_t n, const boot_state_t &state,
                 bool valid, bool wrote)
{
    uint8_t id = op.payload[0];
    uint16_t number = static_cast<uint16_t>(op.payload[1] | (op.payload[2] << 8));
    bool done_ok = false;
    bool received = false;

    for (uint32_t i = 0U; i < n; i++) {
        done_ok = done_ok || ((answers[i].kind == kKindDone) && (answers[i].code == kCodeOk));
        received = received || (answers[i].kind == kKindReceived);
    }
    if (wrote) {
        if (number == kTestImageId) {
            failOp("testimage-wrote-flash", "an op with image number 0xFFFF erased or wrote flash",
                   op);
        }
        if (!valid || (state.trial != BOOT_SLOT_NONE)
            || (state.confirmed != stateCode(g_running))) {
            failOp("write-bad-state",
                   "flash written with no valid boot state, in a trial, or when the running slot "
                   "is not confirmed", op);
        }
    }
    /* The rules live in the checks on arrival (accept), so they are judged on RECEIVED; the
     * transfer state moves on DONE. A command refused on arrival (no RECEIVED) leaves the
     * transfer as it was; one that was received and then failed closes it, except CANCEL, which
     * stays as it was (runCancel). */
    if (number == kTestImageId) {
        uint32_t offset = (op.length >= 7U) ? getU32(&op.payload[3]) : 0U;
        uint32_t bytes = (op.length > 7U) ? (op.length - 7U) : 0U;

        if (id == kIdBegin) {
            if (received && ((offset < kTestImageMinBytes) || (offset > kTestImageMaxBytes))) {
                failOp("testimage-begin",
                       "test image BEGIN accepted with a length outside 5 to 1024", op);
            }
            if (done_ok) {
                g_model.ti_active = true;
                g_model.ti_len = offset;
                g_model.ti_received = 0U;
            } else if (received) {
                g_model.ti_active = false;
            }
        } else if (id == kIdChunk) {
            if (received && (!g_model.ti_active || (offset != g_model.ti_received) ||
                             (bytes > (g_model.ti_len - g_model.ti_received)))) {
                failOp("testimage-chunk",
                       "test image CHUNK accepted out of order or past the length", op);
            } else if (done_ok) {
                memcpy(&g_model.ti_bytes[offset], &op.payload[7], bytes);
                g_model.ti_received += bytes;
            }
        } else if (id == kIdEnd) {
            uint32_t covered = g_model.ti_len - kTestImageCheckBytes;
            if (received && (!g_model.ti_active || (g_model.ti_received != g_model.ti_len))) {
                failOp("testimage-end", "test image END accepted before the last byte", op);
            } else if (done_ok) {
                g_testimage_passed++;
                if (crc::crc32(g_model.ti_bytes, covered) != getU32(&g_model.ti_bytes[covered])) {
                    failOp("testimage-end", "test image END passed with a wrong check value", op);
                }
            }
            if (received) {
                g_model.ti_active = false;
            }
        } else if (id == kIdCancel && done_ok) {
            g_model.ti_active = false;
        }
        return;
    }
    if (id == kIdBegin) {
        uint32_t length = getU32(&op.payload[3]);
        if (received && ((length < kImageMin) || (length > kSlotBytes))) {
            failOp("begin-range", "UPLOAD BEGIN accepted with a length outside 192 to 40960", op);
        }
        if (done_ok) {
            g_model.up_active = true;
            g_model.up_len = length;
            g_model.up_written = 0U;
        } else if (received) {
            g_model.up_active = false;
        }
    } else if (id == kIdChunk) {
        uint32_t offset = getU32(&op.payload[3]);
        uint32_t bytes = op.length - 7U;
        if (received) {
            if (!g_model.up_active || (offset != g_model.up_written)
                || (bytes > (g_model.up_len - g_model.up_written)) ||
                (((bytes % 8U) != 0U) && (bytes != (g_model.up_len - g_model.up_written)))) {
                failOp("chunk-order",
                       "UPLOAD CHUNK accepted out of order, past the length, or not whole flash "
                       "words", op);
            }
            if (done_ok) {
                g_model.up_written += bytes;
            } else {
                g_model.up_active = false;
            }
        }
    } else if (id == kIdEnd) {
        if (received && (!g_model.up_active || (g_model.up_written != g_model.up_len))) {
            failOp("end-early", "UPLOAD END accepted before the last byte", op);
        }
        if (done_ok) {
            g_end_done++;
            if (!blAccepts(otherSlot(g_running))) {
                failOp("end-bad-image",
                       "UPLOAD END DONE but the bootloader's check refuses the image", op);
            }
        }
        if (received) {
            g_model.up_active = false;
        }
    } else if (id == kIdCancel && done_ok) {
        g_model.up_active = false;
    }
}

/**
 * @brief Routes one op through main.cpp's handle() and checks everything.
 *
 * @param[in,out] op  the op (its payload is copied to the guarded buffer)
 * @return false if the fault reaction was reached (the case ends)
 */
bool execute(const Op &op)
{
    Answer answers[kAnswersMax];
    uint8_t *guarded = fz_guarded_at(&g_guard, op.length);
    boot_state_t state = fake.state;
    bool valid = fake.state_valid;
    HealthMode mode = mode_manager.mode();
    bool tests_ok = mode_manager.trialTestsPassed();
    uint32_t erases = fake.erases;
    uint32_t programs = fake.programs;
    size_t from = fake_link.sent_bytes;
    bool faulted = false;
    bool cmd_frame = (op.destination == Destination::kController)
        && (op.type == FrameType::kCommand);
    Frame frame{};

    g_now += op.advance_ms;
    core_clock.now = g_now;
    arm_state.expireWhenDue(g_now);
    memcpy(guarded, op.payload, op.length);
    frame.type = op.type;
    frame.destination = op.destination;
    frame.flags = op.flags;
    frame.length = static_cast<uint16_t>(op.length);
    frame.sequence = op.sequence;
    frame.expiry = 0U;
    frame.payload = guarded;
    fz_fault_clear();
    g_payloads++;
    FZ_FAULT_GUARD(faulted, handle(frame));
    if (faulted || (fz_fault_calls != 0U)) {
        failOp("fault-reaction", "a command payload from the link reached the fault reaction", op);
        return false;
    }
    if (cmd_frame && (op.length >= 1U)) {
        uint32_t n = readAnswers(from, answers);
        bool wrote = (fake.erases != erases) || (fake.programs != programs);
        uint8_t id = op.payload[0];

        if ((id == kIdArm) && (op.length == 6U)) {
            g_model.armed = true;
            g_model.arm_cmd = op.payload[1];
            g_model.arm_seq = getU32(&op.payload[2]);
            g_model.arm_ms = g_now;
        }
        if ((id == kIdSwitch) || (id == kIdReturn) || (id == kIdStartFailed)
            || (id == kIdConfirm)) {
            checkDangerous(op, answers, n, mode, tests_ok, state);
        }
        if ((id >= kIdBegin) && (id <= kIdCancel) && (op.length >= 3U)) {
            checkUpload(op, answers, n, state, valid, wrote);
        }
        if (wrote && !((id >= kIdBegin) && (id <= kIdCancel))) {
            failOp("write-by-other", "flash written by a command that is not an upload", op);
        }
    }
    if (fake.outside != 0U) {
        failOp("write-outside", "erase or write outside the slot that may be written", op);
        fake.outside = 0U;
    }
    if (fake.bad_reads != 0U) {
        failOp("read-outside", "flash read outside the flash", op);
        fake.bad_reads = 0U;
    }
    if (fake_link.sent_bytes > (kFakeLinkBytes - 4096U)) {
        fakeLinkReset();
    }
    return true;
}

/**
 * @brief Runs mode_manager.poll() and checks that it does not fault.
 *
 * @param[in]     rng  the generator (advances time)
 * @return false if the fault reaction was reached
 */
bool pollOnce(fz_rng_t *rng)
{
    bool faulted = false;
    Op none{};

    g_now += fz_rng_below(rng, 70000U);
    core_clock.now = g_now;
    FZ_FAULT_GUARD(faulted, mode_manager.poll(g_now));
    if (faulted || (fz_fault_calls != 0U)) {
        failOp("fault-reaction-poll", "ModeManager::poll reached the fault reaction", none);
        return false;
    }
    return true;
}

/* ---- Scripts ---------------------------------------------------------------------------- */

/**
 * @brief Runs an upload of a real image: BEGIN, CHUNKs, END, with one random fault injected.
 *
 * @param[in,out] rng  the generator
 * @return false if a fault reaction ended the case
 */
bool scriptUpload(fz_rng_t *rng)
{
    static Op op;
    Slot target = otherSlot(g_running);
    const ImageSeed *image = pickImage(rng, target);
    uint16_t number = static_cast<uint16_t>(fz_rng_below(rng, 3U));
    uint32_t size = (image != nullptr) ? image->size : kImageMin;
    uint32_t step = fz_rng_chance(rng, 700U) ? kChunkData : (8U * (1U + fz_rng_below(rng, 32U)));
    uint32_t chunks = (size + step - 1U) / step;
    uint32_t bad = fz_rng_chance(rng, 700U) ? fz_rng_below(rng, chunks + 2U) : 0xFFFFFFFFU;
    uint32_t sent = 0U;
    uint32_t offset = 0U;

    if (image == nullptr) {
        return true;
    }
    newOp(rng, &op);
    buildBegin(&op, number, size);
    if (bad == 0U) {
        mutateOp(rng, &op);
    }
    if (!execute(op)) {
        return false;
    }
    while ((offset < size) && (sent < kOpsMax)) {
        uint32_t bytes = (size - offset) < step ? (size - offset) : step;
        newOp(rng, &op);
        buildChunk(&op, number, offset, &image->bytes[offset], bytes);
        if (bad == (sent + 1U)) {
            mutateOp(rng, &op);
        }
        if (!execute(op)) {
            return false;
        }
        offset += bytes;
        sent++;
    }
    newOp(rng, &op);
    buildShort(&op, kIdEnd, number);
    if (bad == (chunks + 1U)) {
        mutateOp(rng, &op);
    }
    return execute(op);
}

/**
 * @brief Runs a dangerous command with an ARM before it, with one random variation.
 *
 * @param[in,out] rng  the generator
 * @return false if a fault reaction ended the case
 */
bool scriptDangerous(fz_rng_t *rng)
{
    static Op arm;
    static Op cmd;
    static const uint8_t ids[] = {kIdSwitch, kIdReturn, kIdStartFailed, kIdConfirm};
    uint8_t id = ids[fz_rng_below(rng, 4U)];
    uint32_t variant = fz_rng_below(rng, 12U);

    newOp(rng, &arm);
    newOp(rng, &cmd);
    buildDangerous(rng, &cmd, id);
    buildArm(&arm, (variant == 1U) ? static_cast<uint8_t>(id ^ 1U) : id,
             (variant == 2U) ? cmd.sequence + 1U : cmd.sequence);
    if (variant == 3U) {
        cmd.advance_ms = kArmLifeMs - 1U + fz_rng_below(rng, 3U);   /* the edge of the 30 s */
    }
    if (variant == 4U) {
        cmd.advance_ms = kArmLifeMs + fz_rng_below(rng, 100U);
    }
    if (variant == 9U) {
        mutateOp(rng, &cmd);
    }
    if (variant == 10U) {
        mutateOp(rng, &arm);
    }
    if (!execute(arm)) {
        return false;
    }
    if (fz_rng_chance(rng, 150U)) {   /* a second ARM replaces the first */
        static Op second;
        newOp(rng, &second);
        buildArm(&second, id, cmd.sequence + (fz_rng_chance(rng, 500U) ? 0U : 1U));
        if (!execute(second)) {
            return false;
        }
    }
    if (!execute(cmd)) {
        return false;
    }
    if (variant == 5U) {   /* replay: the same command again with the same sequence number */
        if (!execute(cmd)) {
            return false;
        }
    }
    if (variant == 6U) {   /* a new sequence number, no new ARM */
        newOp(rng, &cmd);
        buildDangerous(rng, &cmd, id);
        if (!execute(cmd)) {
            return false;
        }
    }
    return fz_rng_chance(rng, kChancePoll) ? pollOnce(rng) : true;
}

/**
 * @brief Runs the test image of the trial tests: BEGIN, CHUNKs, END, number 0xFFFF.
 *
 * @param[in,out] rng  the generator
 * @return false if a fault reaction ended the case
 */
bool scriptTestImage(fz_rng_t *rng)
{
    static Op op;
    static uint8_t image[kTestImageMaxBytes];
    uint32_t size = kTestImageMinBytes + fz_rng_below(rng, 700U);
    uint32_t covered = size - kTestImageCheckBytes;
    uint32_t step = 1U + fz_rng_below(rng, kChunkData);
    uint32_t offset = 0U;
    uint32_t bad = fz_rng_chance(rng, 600U) ? fz_rng_below(rng, 6U) : 0xFFFFFFFFU;

    for (uint32_t i = 0U; i < covered; i++) {
        image[i] = static_cast<uint8_t>(fz_rng_next(rng));
    }
    putU32(&image[covered], crc::crc32(image, covered));
    if (fz_rng_chance(rng, 300U)) {
        image[fz_rng_below(rng, size)] ^= 0x10U;   /* a wrong check value */
    }
    newOp(rng, &op);
    buildBegin(&op, kTestImageId, size);
    if (bad == 0U) {
        mutateOp(rng, &op);
    }
    if (!execute(op)) {
        return false;
    }
    for (uint32_t k = 1U; offset < size; k++) {
        uint32_t bytes = (size - offset) < step ? (size - offset) : step;
        newOp(rng, &op);
        buildChunk(&op, kTestImageId, offset, &image[offset], bytes);
        if (bad == k) {
            mutateOp(rng, &op);
        }
        if (!execute(op)) {
            return false;
        }
        offset += bytes;
    }
    newOp(rng, &op);
    buildShort(&op, kIdEnd, kTestImageId);
    if (!execute(op)) {
        return false;
    }
    if (fz_rng_chance(rng, 500U)) {   /* then a CONFIRM, armed */
        return scriptDangerous(rng);
    }
    return true;
}

/**
 * @brief Fills the fake TAMP backup registers (the words the bootloader leaves for the firmware,
 * common/backup_layout.h) with matching pairs of every kind of value, broken pairs and zeros.
 *
 * @param[in,out] rng  the generator
 */
void fillBackup(fz_rng_t *rng)
{
    for (uint32_t pair = 0U; pair < BACKUP_PAIRS; pair++) {
        uint32_t how = fz_rng_below(rng, 10U);
        uint32_t value = fz_rng_chance(rng, 500U) ? fz_rng_edge32(rng)
            : static_cast<uint32_t>(fz_rng_next(rng));

        if (fz_rng_chance(rng, 500U)) {   /* a value the layout lists */
            if (pair == BACKUP_PAIR_FALLBACK) {
                value = fz_rng_below(rng, CHECK_LAST + 1U)
                    | (fz_rng_below(rng, CHECK_LAST + 1U) << 8) |
                        (fz_rng_below(rng, CHECK_LAST + 1U) << 16) | (fz_rng_below(rng, 4U) << 24);
            } else if (pair == BACKUP_PAIR_FAULT_LOCATION) {
                value = fz_rng_below(rng, 0x20U);
            } else if (pair == BACKUP_PAIR_FAULT_LATER) {
                value = fz_rng_below(rng, 0x100U);
            }
        }
        if (how < 6U) {
            fakeBackupSet(pair, value);
        } else if (how < 8U) {
            fakeBackupSet(pair, value);
            fake.backup[(2U * pair) + 1U] ^= static_cast<uint32_t>(1ULL << fz_rng_below(rng, 32U));
        } else if (how < 9U) {
            fake.backup[2U * pair] = 0U;
            fake.backup[(2U * pair) + 1U] = 0U;
        } else {
            fake.backup[2U * pair] = static_cast<uint32_t>(fz_rng_next(rng));
            fake.backup[(2U * pair) + 1U] = static_cast<uint32_t>(fz_rng_next(rng));
        }
    }
}

/**
 * @brief Checks the EVENT reports the firmware sent at its start: a fall-back event (0x0002) or a
 * bootloader fault event (0x0005) carries only values the layout lists (Interfaces Sec. 5.4).
 *
 * @param[in,out] rng    the generator
 * @param[in]     index  case index
 * @return false if the fault reaction was reached
 */
bool scriptBackup(fz_rng_t *rng, uint64_t index)
{
    Frame f{};
    bool faulted = false;
    Op none{};

    fillBackup(rng);
    new (&uart_link) UartLink();
    new (&sender) FrameSender(uart_link);
    new (&test_image) TestImage();
    new (&mode_manager) ModeManager(sender, test_image);
    fakeLinkReset();
    fz_fault_clear();
    FZ_FAULT_GUARD(faulted, mode_manager.start(g_running, g_now, false));
    if (!faulted) {
        FZ_FAULT_GUARD(faulted, mode_manager.poll(g_now + 1U));
    }
    if (faulted || (fz_fault_calls != 0U)) {
        failOp("fault-reaction-backup",
               "a backup-register value reached the fault reaction in the firmware", none);
        return false;
    }
    g_reader.reset();
    for (size_t i = 0U; i < fake_link.sent_bytes; i++) {
        if (g_reader.feed(fake_link.sent[i], 0U, f) && (f.type == FrameType::kReport)
            && (f.length >= 15U) &&
            (f.payload[12] == 0x03U)) {
            uint16_t code = static_cast<uint16_t>(f.payload[13] | (f.payload[14] << 8));
            uint32_t detail = f.length - 15U;

            if ((code == 0x0002U) && ((detail != 2U) || (f.payload[15] < 1U)
                                      || (f.payload[15] > 3U) ||
                                     (f.payload[16] < 1U) || (f.payload[16] > CHECK_LAST))) {
                fz_fail("event-garbage", index,
                        reinterpret_cast<const uint8_t *>(fake.backup.data()),
                        sizeof(fake.backup),
                            "fall-back EVENT 0x0002 with a copy or check code outside the table");
            }
            if ((code == 0x0005U) && ((detail != 2U)
                                      || ((f.payload[15] & 0x7FU) < FAULT_RANGE_BOOTLOADER_FIRST) ||
                                     ((f.payload[15] & 0x7FU) > FAULT_RANGE_BOOTLOADER_LAST))) {
                fz_fail("event-garbage", index,
                        reinterpret_cast<const uint8_t *>(fake.backup.data()),
                        sizeof(fake.backup),
                            "EVENT 0x0005 with a fault location outside the bootloader's range");
            }
        }
    }
    return true;
}

/**
 * @brief Checks at the end of a case that nothing outside the slot that may be written changed:
 * the bootloader, the running slot, the last-resort region and the boot-state pages (REQ-007,
 * REQ-008).
 */
void checkProtectedFlash(void)
{
    uint32_t start = (fake.allowed_slot == Slot::kA) ? kSlotAStart : kSlotBStart;
    uint32_t from = start - kFlashStart;
    Op none{};

    if ((memcmp(g_reference, fake.flash.data(), from) != 0) ||
        (memcmp(&g_reference[from + kSlotBytes], &fake.flash[from + kSlotBytes],
                kFakeFlashBytes - from - kSlotBytes) != 0)) {
        failOp("protected-flash", "flash outside the writable slot changed during the case", none);
    }
}

/**
 * @brief Observation, not a check: the same ARM and SWITCH frames (same sequence numbers) sent a
 * second time in a row. REQ-031 asks the controller firmware to run a repeated command only once;
 * main.cpp says the sequence-number check is not built yet (step 2b). The log line records what
 * the second SWITCH was answered.
 */
void probeReplay(void)
{
    static Op arm;
    static Op sw;
    Answer answers[kAnswersMax];
    fz_rng_t rng;
    uint32_t n[2] = {0U, 0U};
    uint8_t last_code[2] = {0xEEU, 0xEEU};
    uint8_t last_kind[2] = {0xEEU, 0xEEU};

    fz_rng_seed(&rng, 1U, 0U);
    startCase(&rng);
    fake.state_valid = true;
    fake.state.confirmed = stateCode(g_running);
    fake.state.trial = BOOT_SLOT_NONE;
    fake.state.flags = 0U;
    fake.fail_erase = fake.fail_program = fake.ecc_error = fake.fail_state_write = false;
    new (&mode_manager) ModeManager(sender, test_image);
    mode_manager.start(g_running, g_now, false);
    {
        Slot other = otherSlot(g_running);
        for (uint32_t i = 0U; i < g_image_count; i++) {
            if (g_images[i].build_firmware && (g_images[i].made_for == g_running)) {
                memcpy(slotBytes(g_running), g_images[i].bytes, g_images[i].size);
            }
            if (g_images[i].build_firmware && (g_images[i].made_for == other)) {
                memset(slotBytes(other), 0xFF, kSlotBytes);
                memcpy(slotBytes(other), g_images[i].bytes, g_images[i].size);
            }
        }
    }
    for (uint32_t round = 0U; round < 2U; round++) {
        size_t from;
        newOp(&rng, &arm);
        newOp(&rng, &sw);
        arm.advance_ms = 1U;
        sw.advance_ms = 1U;
        sw.sequence = 4242U;
        buildDangerous(&rng, &sw, kIdSwitch);
        buildArm(&arm, kIdSwitch, 4242U);
        fakeLinkReset();
        (void)execute(arm);
        from = fake_link.sent_bytes;
        (void)execute(sw);
        n[round] = readAnswers(from, answers);
        if (n[round] > 0U) {
            last_kind[round] = answers[n[round] - 1U].kind;
            last_code[round] = answers[n[round] - 1U].code;
        }
    }
    fz_log("observation: ARM+SWITCH with sequence number 4242 sent twice in a row (no restart in "
           "between): "
           "last answer of the first SWITCH kind 0x%02X code 0x%02X, of the second kind 0x%02X "
               "code 0x%02X "
           "(kind 0x02 DONE, 0x03 REFUSED, 0x04 ALREADY DONE; the controller as built has no "
           "repeated-sequence check, main.cpp comment on step 2b)", last_kind[0], last_code[0],
               last_kind[1],
           last_code[1]);
    fakeReset();
}

/**
 * @brief Runs case `index`: a start state, then a script.
 *
 * @param[in]     seed   run seed
 * @param[in]     index  case index
 */
void runCase(uint64_t seed, uint64_t index)
{
    fz_rng_t rng;
    uint32_t kind;
    bool ok = true;

    g_case_index = index;
    fz_rng_seed(&rng, seed, index);
    g_case_log_len = 0U;
    fz_input_begin(index, g_case_log, 0U);
    startCase(&rng);
    kind = fz_rng_below(&rng, 1000U);
    if (kind < kShareUpload) {
        ok = scriptUpload(&rng);
    } else if (kind < (kShareUpload + kShareDangerous)) {
        ok = scriptDangerous(&rng);
    } else if (kind < (kShareUpload + kShareDangerous + kShareTestImage)) {
        ok = scriptTestImage(&rng);
    } else if (kind < (kShareUpload + kShareDangerous + kShareTestImage + kShareBackup)) {
        ok = scriptBackup(&rng, index);
        g_payloads++;
    } else {
        static Op op;
        for (uint32_t k = 1U + fz_rng_below(&rng, 12U); (k > 0U) && ok; k--) {
            buildRandomOp(&rng, &op);
            ok = execute(op);
            if (ok && fz_rng_chance(&rng, kChancePoll)) {
                ok = pollOnce(&rng);
            }
        }
    }
    if (ok && fz_rng_chance(&rng, kChancePoll)) {
        ok = pollOnce(&rng);
    }
    checkProtectedFlash();
    g_cases++;
    fz_input_end();
}

}  // namespace

/**
 * @brief Runs the command-payload fuzz target.
 *
 * @param[in]     argc  argument count
 * @param[in]     argv  arguments: see fz_options_parse
 * @return 0 if no input failed, 1 otherwise
 */
int main(int argc, char **argv)
{
    fz_options_t options;
    uint64_t index = 0U;

    fz_options_parse(&options, argc, argv);
    fz_run_open("fz_command", &options, argc, argv);
    fz_guarded_init(&g_guard, kPayloadMax);
    loadImages(&options);
    fz_log("image seeds: %u", g_image_count);
    if (!options.one) {
        probeReplay();
    }
    if (options.one) {
        index = options.one_index;
        runCase(options.seed, index);
        index++;
    } else {
        while (g_payloads < options.count) {
            runCase(options.seed, index);
            index++;
        }
    }
    fz_log("cases: %llu; command payloads routed: %llu", static_cast<unsigned long long>(g_cases),
           static_cast<unsigned long long>(g_payloads));
    fz_log("dangerous commands answered RECEIVED: %llu; SWITCH/START FAILED DONE: %llu; UPLOAD END "
           "DONE: %llu; test image END passed: %llu",
           static_cast<unsigned long long>(g_accepted_received),
               static_cast<unsigned long long>(g_switch_done),
           static_cast<unsigned long long>(g_end_done),
               static_cast<unsigned long long>(g_testimage_passed));
    return fz_run_finish(g_payloads);
}
