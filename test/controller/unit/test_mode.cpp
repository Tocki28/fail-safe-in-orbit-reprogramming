/*
 * test_mode.cpp — host unit tests of the mode manager and the test image
 * (src/controller/firmware/mode-manager/) against the decided design of step 2d, Interfaces
 * Sec. 4.2 (answers, codes), Sec. 5.1 (commands, test image), Sec. 5.4 (EVENT layout and
 * codes), REQ-011 to REQ-021, REQ-039, REQ-139 and REQ-140.
 *
 * The chip is replaced by fake_port.cpp and fake_mode_port.cpp (RAM flash, boot state,
 * restart) and the link by fake_uart_link.cpp, which keeps every byte "sent". The commands go
 * through the same steps as in main.cpp (checks on arrival, RECEIVED, the work, DONE or
 * REFUSED), sent through the real FrameSender, and every frame the controller sent is read
 * back through the real FrameParser, so a test checks the order of answers and events on the
 * wire.
 *
 * Built with the Mac's clang++, the undefined-behaviour sanitizer and libc++'s debug hardening.
 * Run: make -C test/controller unit. Prints PASS or FAIL per test; exit status 1 on a failure.
 * Usage: test_mode <dir with firmware-A.img, firmware-B.img>
 */
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "arm_state.hpp"
#include "boot_slot.hpp"
#include "crc32.h"
#include "fake_port.hpp"
#include "fake_uart_link.hpp"
#include "frame.hpp"
#include "frame_sender.hpp"
#include "health_monitor.hpp"
#include "image_format.h"
#include "layout.h"
#include "mode_manager.hpp"
#include "test_image.hpp"
#include "uart_link.hpp"
#include "update_receiver.hpp"

namespace {

constexpr uint32_t kHeader = IMAGE_HEADER_BYTES;
constexpr uint32_t kBodyMax = SLOT_BYTES - IMAGE_HEADER_BYTES - IMAGE_SIGNATURES_BYTES;
/* Body of the synthetic image the tests put in the running slot. */
constexpr uint32_t kBodyBytes = 600U;
/* Same code byte as the bench's damaged image (test/controller/Makefile, FLIP_OFFSET). */
constexpr uint32_t kFlipOffset = 0x600U;
constexpr uint8_t kSwitch = 0x20U;
constexpr uint8_t kConfirm = 0x26U;
constexpr uint32_t kChunkMax = 256U;
constexpr AnswerCode kReceivedCode = static_cast<AnswerCode>(0x00U);
constexpr uint32_t kLogFrames = 128U;
constexpr uint32_t kLogPayload = 64U;
constexpr int16_t kNoDetail = -1;
/* The standard CRC-32 check value: CRC-32 of the ASCII bytes "123456789" is 0xCBF43926, the
 * same parameters as Interfaces Sec. 4.0 (zlib.crc32; tools/check_crc32.py). An anchor that
 * does not come from the code under test. */
constexpr uint32_t kCheckValue123456789 = 0xCBF43926U;

using SlotImage = std::array<uint8_t, SLOT_BYTES>;
using TestBytes = std::array<uint8_t, kTestImageMaxBytes>;

uint32_t failures;
uint32_t tests;

/**
 * @brief Counts and prints one test result.
 *
 * @param[in]     ok    true if the test passed
 * @param[in]     name  what was tested
 */
void report(bool ok, const char *name)
{
    tests++;
    if (!ok) {
        failures++;
    }
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
}

/**
 * @brief Writes a 32-bit value little-endian (Interfaces Sec. 4.0).
 *
 * @param[out]    p  first of the 4 bytes
 * @param[in]     v  the value
 */
void putU32(uint8_t *p, uint32_t v)
{
    for (uint32_t i = 0U; i < 4U; i++) {
        p[i] = static_cast<uint8_t>(v >> (8U * i));
    }
}

/* ---- The controller under test ---------------------------------------------------------- */

/* The objects main.cpp owns, wired the same way. */
struct Rig {
    UartLink uart;
    FrameSender sender{uart};
    TestImage image;
    ModeManager mm{sender, image};
    ArmState arm;
    UpdateReceiver updater;
    uint32_t seq = 100U;   /* the onboard computer's next sequence number */
};

/**
 * @brief Returns the address of a slot's first byte in the fake flash.
 *
 * @param[in]     slot  Slot::kA or Slot::kB
 * @return pointer into fake.flash
 */
uint8_t *slotBytes(Slot slot)
{
    uint32_t start = (slot == Slot::kA) ? kSlotAStart : kSlotBStart;
    return &fake.flash[start - kFlashStart];
}

/**
 * @brief Puts a synthetic image in a slot: a 64-byte header whose length and body check value
 * (Interfaces Sec. 5.5, Figure 6) describe a body of `body` bytes, and that body.
 *
 * @param[in]     slot  Slot::kA or Slot::kB
 * @param[in]     body  body bytes, at most kBodyMax
 */
void putImage(Slot slot, uint32_t body)
{
    uint8_t *image = slotBytes(slot);

    std::memset(image, 0, kHeader);
    for (uint32_t i = 0U; i < body; i++) {
        image[kHeader + i] = static_cast<uint8_t>((i * 13U) + 5U);
    }
    putU32(&image[IMAGE_OFFSET_LENGTH], body);
    putU32(&image[IMAGE_OFFSET_BODY_CHECK], crc32_compute(&image[kHeader], body));
}

/**
 * @brief Sets the fakes: flash patterns, the boot state, a good image in the running slot.
 *
 * @param[in]     running    the slot the firmware runs from
 * @param[in]     confirmed  boot state's confirmed slot code
 * @param[in]     trial      boot state's trial slot code
 */
void prepare(Slot running, uint8_t confirmed, uint8_t trial)
{
    fakeReset();
    fakeLinkReset();
    fake.state.confirmed = confirmed;
    fake.state.trial = trial;
    fake.state.failed_starts = (trial == BOOT_SLOT_NONE) ? uint8_t{0U} : uint8_t{1U};
    if ((running == Slot::kA) || (running == Slot::kB)) {
        putImage(running, kBodyBytes);
    }
}

/**
 * @brief prepare(), then the controller's start (main.cpp: updater.init, mode_manager.start).
 *
 * @param[in,out] rig        the controller
 * @param[in]     running    the slot the firmware runs from
 * @param[in]     confirmed  boot state's confirmed slot code
 * @param[in]     trial      boot state's trial slot code
 * @param[in]     now        milliseconds since start
 */
void startRig(Rig &rig, Slot running, uint8_t confirmed, uint8_t trial, uint32_t now)
{
    prepare(running, confirmed, trial);
    rig.updater.init(running);
    rig.mm.start(running, now);
}

/* ---- Reading back what was sent --------------------------------------------------------- */

enum class Kind : uint8_t { kAnswer, kEvent, kOther };

/* One frame as the tests compare it. */
struct Item {
    Kind kind;
    uint8_t answer_kind;   /* answers: AnswerKind */
    uint8_t code;          /* answers: AnswerCode */
    uint16_t event;        /* events: EventCode */
    int16_t detail;        /* events: first detail byte, or kNoDetail */
};

struct Sent {
    FrameType type;
    uint32_t seq;
    uint16_t length;
    std::array<uint8_t, kLogPayload> payload;
};

using SentLog = std::array<Sent, kLogFrames>;

/**
 * @brief An expected answer.
 *
 * @param[in]     kind  RECEIVED, DONE or REFUSED
 * @param[in]     code  its code
 * @return the item
 */
Item ans(AnswerKind kind, AnswerCode code)
{
    return Item{Kind::kAnswer, static_cast<uint8_t>(kind), static_cast<uint8_t>(code), 0U,
                kNoDetail};
}

/**
 * @brief An expected EVENT report without detail.
 *
 * @param[in]     code  the event code
 * @return the item
 */
Item ev(EventCode code)
{
    return Item{Kind::kEvent, 0U, 0U, static_cast<uint16_t>(code), kNoDetail};
}

/**
 * @brief An expected EVENT report with a one-byte detail.
 *
 * @param[in]     code    the event code
 * @param[in]     detail  the detail byte
 * @return the item
 */
Item ev(EventCode code, uint8_t detail)
{
    return Item{Kind::kEvent, 0U, 0U, static_cast<uint16_t>(code), detail};
}

/* RECEIVED then DONE 0x00, the answers of a command that ran. */
const Item kRecv = Item{Kind::kAnswer, 0x01U, 0x00U, 0U, kNoDetail};
const Item kDone = Item{Kind::kAnswer, 0x02U, 0x00U, 0U, kNoDetail};

/**
 * @brief Decodes the frames sent in bytes [from, to) of the fake link with the frame parser.
 *
 * @param[in]     from  first byte
 * @param[in]     to    one past the last byte
 * @param[out]    out   the frames, in order
 * @return the number of frames
 */
uint32_t decodeRange(uint32_t from, uint32_t to, SentLog &out)
{
    static FrameParser parser;
    uint32_t n = 0U;

    parser.reset();
    for (uint32_t i = from; i < to; i++) {
        Frame f{};
        if (parser.feed(fake_link.sent[i], 0U, f) && (n < kLogFrames)) {
            out[n].type = f.type;
            out[n].seq = f.sequence;
            out[n].length = f.length;
            std::memcpy(out[n].payload.data(), f.payload,
                        (f.length < kLogPayload) ? f.length : kLogPayload);
            n++;
        }
    }
    return n;
}

/**
 * @brief Classifies one decoded frame as an answer, an EVENT report, or something else.
 *
 * @param[in]     s  the frame
 * @return its item
 */
Item itemOf(const Sent &s)
{
    if ((s.type == FrameType::kAnswer) && (s.length == kAnswerPayloadBytes)) {
        return Item{Kind::kAnswer, s.payload[4], s.payload[5], 0U, kNoDetail};
    }
    if ((s.type == FrameType::kReport) && (s.length >= kEventHeadBytes) &&
        (s.payload[12] == kReportKindEvent)) {
        auto code = static_cast<uint16_t>(s.payload[13] | (s.payload[14] << 8));
        int16_t detail = (s.length > kEventHeadBytes) ? int16_t{s.payload[15]} : kNoDetail;
        return Item{Kind::kEvent, 0U, 0U, code, detail};
    }
    return Item{Kind::kOther, 0U, 0U, 0U, kNoDetail};
}

/**
 * @brief Prints one item, for a failed comparison.
 *
 * @param[in]     tag  "got" or "want"
 * @param[in]     it   the item
 */
void printItem(const char *tag, const Item &it)
{
    if (it.kind == Kind::kAnswer) {
        std::printf("      %s answer kind 0x%02x code 0x%02x\n", tag, it.answer_kind, it.code);
    } else if (it.kind == Kind::kEvent) {
        std::printf("      %s event 0x%04x detail %d\n", tag, it.event, it.detail);
    } else {
        std::printf("      %s other frame\n", tag);
    }
}

/**
 * @brief Returns true if the frames sent from byte `from` on are exactly `want`, in order.
 *
 * Prints both lists when they differ.
 *
 * @param[in]     from  first byte of the fake link to read
 * @param[in]     want  the expected answers and events
 * @return true if they match
 */
bool sentIs(uint32_t from, std::initializer_list<Item> want)
{
    static SentLog log;
    uint32_t n = decodeRange(from, fake_link.sent_bytes, log);
    bool same = (static_cast<size_t>(n) == want.size());
    uint32_t i = 0U;

    for (const Item &w : want) {
        if (!same || (i >= n)) {
            break;
        }
        Item g = itemOf(log[i]);
        same = (g.kind == w.kind) && (g.answer_kind == w.answer_kind) && (g.code == w.code) &&
               (g.event == w.event) && (g.detail == w.detail);
        i++;
    }
    if (!same) {
        for (uint32_t k = 0U; k < n; k++) {
            printItem("got ", itemOf(log[k]));
        }
        for (const Item &w : want) {
            printItem("want", w);
        }
    }
    return same;
}

/**
 * @brief Returns the number of frames sent in bytes [0, to).
 *
 * @param[in]     to  one past the last byte
 * @return the number of frames
 */
uint32_t framesBefore(uint32_t to)
{
    static SentLog log;
    return decodeRange(0U, to, log);
}

/* ---- Commands, as main.cpp handles them ------------------------------------------------- */

struct Answer {
    bool received;
    AnswerCode code;
};

/**
 * @brief Returns true for a command that ran: RECEIVED, then DONE 0x00.
 *
 * @param[in]     a  the answer
 * @return true if received and the code is 0x00
 */
bool done(Answer a)
{
    return a.received && (a.code == AnswerCode::kSuccess);
}

/**
 * @brief Returns true for a refusal on arrival: REFUSED only, with this code.
 *
 * @param[in]     a     the answer
 * @param[in]     code  the code expected
 * @return true if not received and the code matches
 */
bool refusedOnly(Answer a, AnswerCode code)
{
    return !a.received && (a.code == code);
}

/**
 * @brief Returns true for RECEIVED, then REFUSED with this code.
 *
 * @param[in]     a     the answer
 * @param[in]     code  the code expected
 * @return true if received and the code matches
 */
bool receivedThenRefused(Answer a, AnswerCode code)
{
    return a.received && (a.code == code);
}

/**
 * @brief SWITCH or CONFIRM as main.cpp's handleDangerous handles it.
 *
 * @param[in,out] rig      the controller
 * @param[in]     command  kSwitch or kConfirm
 * @param[in]     seq      the frame's sequence number
 * @param[in]     now      milliseconds since start
 * @return the answers given
 */
Answer dangerous(Rig &rig, uint8_t command, uint32_t seq, uint32_t now)
{
    const std::array<uint8_t, 1> p = {command};
    AnswerCode code = rig.mm.acceptDangerous(p.data(), 1U, seq, now, rig.arm);

    if (code != AnswerCode::kSuccess) {
        rig.sender.sendAnswer(seq, AnswerKind::kRefused, code);
        return Answer{false, code};
    }
    rig.sender.sendAnswer(seq, AnswerKind::kReceived, kReceivedCode);
    code = rig.mm.runDangerous(command, now);
    rig.sender.sendAnswer(seq, (code == AnswerCode::kSuccess) ? AnswerKind::kDone
                                                              : AnswerKind::kRefused,
                          code);
    return Answer{true, code};
}

/**
 * @brief ARM as main.cpp's handleArm handles it; takes the next sequence number itself.
 *
 * @param[in,out] rig      the controller
 * @param[in]     command  identifier named
 * @param[in]     named    sequence number named
 * @param[in]     now      milliseconds since start
 */
void sendArm(Rig &rig, uint8_t command, uint32_t named, uint32_t now)
{
    std::array<uint8_t, kArmPayloadBytes> p{};
    ArmRequest request{};
    uint32_t seq = rig.seq++;

    p[0] = kCmdArm;
    p[1] = command;
    putU32(&p[2], named);
    if (!ArmState::parse(p.data(), kArmPayloadBytes, request)) {
        rig.sender.sendAnswer(seq, AnswerKind::kRefused, AnswerCode::kImpossible);
        return;
    }
    rig.sender.sendAnswer(seq, AnswerKind::kReceived, kReceivedCode);
    rig.arm.arm(request, now);
    rig.sender.sendAnswer(seq, AnswerKind::kDone, AnswerCode::kSuccess);
}

/**
 * @brief ARM naming the next frame, then that dangerous command, as the operator sends them.
 *
 * @param[in,out] rig      the controller
 * @param[in]     command  kSwitch or kConfirm
 * @param[in]     now      milliseconds since start
 * @return the dangerous command's answers
 */
Answer armedCommand(Rig &rig, uint8_t command, uint32_t now)
{
    sendArm(rig, command, rig.seq + 1U, now);
    return dangerous(rig, command, rig.seq++, now);
}

/**
 * @brief An UPLOAD command as main.cpp's routeUpload handles it: 0xFFFF to the test image,
 * anything else to the update receiver.
 *
 * @param[in,out] rig      the controller
 * @param[in]     payload  the command payload
 * @param[in]     length   payload bytes
 * @return the answers given
 */
Answer upload(Rig &rig, const uint8_t *payload, uint16_t length)
{
    uint32_t seq = rig.seq++;
    UpdateCommand cmd{};
    bool test = UpdateReceiver::parse(payload, length, cmd) && TestImage::isTestImage(cmd);
    AnswerCode code = test ? rig.mm.acceptTestImage(cmd) : rig.updater.accept(payload, length, cmd);

    if (code != AnswerCode::kSuccess) {
        rig.sender.sendAnswer(seq, AnswerKind::kRefused, code);
        return Answer{false, code};
    }
    rig.sender.sendAnswer(seq, AnswerKind::kReceived, kReceivedCode);
    code = test ? rig.mm.runTestImage(cmd) : rig.updater.run(cmd);
    rig.sender.sendAnswer(seq, (code == AnswerCode::kSuccess) ? AnswerKind::kDone
                                                              : AnswerKind::kRefused,
                          code);
    return Answer{true, code};
}

/**
 * @brief UPLOAD BEGIN.
 *
 * @param[in,out] rig     the controller
 * @param[in]     number  image number
 * @param[in]     length  image length
 * @return the answers given
 */
Answer begin(Rig &rig, uint16_t number, uint32_t length)
{
    std::array<uint8_t, 7> p{};

    p[0] = static_cast<uint8_t>(UploadId::kBegin);
    p[1] = static_cast<uint8_t>(number);
    p[2] = static_cast<uint8_t>(number >> 8);
    putU32(&p[3], length);
    return upload(rig, p.data(), 7U);
}

/**
 * @brief UPLOAD CHUNK.
 *
 * @param[in,out] rig     the controller
 * @param[in]     number  image number
 * @param[in]     offset  offset of the data
 * @param[in]     data    the data
 * @param[in]     n       data bytes, 1 to 256
 * @return the answers given
 */
Answer chunk(Rig &rig, uint16_t number, uint32_t offset, const uint8_t *data, uint32_t n)
{
    std::array<uint8_t, 7U + kChunkMax> p{};

    p[0] = static_cast<uint8_t>(UploadId::kChunk);
    p[1] = static_cast<uint8_t>(number);
    p[2] = static_cast<uint8_t>(number >> 8);
    putU32(&p[3], offset);
    std::memcpy(&p[7], data, n);
    return upload(rig, p.data(), static_cast<uint16_t>(7U + n));
}

/**
 * @brief UPLOAD END or CANCEL.
 *
 * @param[in,out] rig     the controller
 * @param[in]     id      UploadId::kEnd or kCancel
 * @param[in]     number  image number
 * @return the answers given
 */
Answer shortCommand(Rig &rig, UploadId id, uint16_t number)
{
    std::array<uint8_t, 3> p{};

    p[0] = static_cast<uint8_t>(id);
    p[1] = static_cast<uint8_t>(number);
    p[2] = static_cast<uint8_t>(number >> 8);
    return upload(rig, p.data(), 3U);
}

/**
 * @brief A test image of `size` bytes: a pattern, then the CRC-32 of the pattern, little-endian;
 * with good false, that check value with its lowest bit inverted.
 *
 * @param[out]    image  the bytes
 * @param[in]     size   5 to 1,024
 * @param[in]     good   false for a wrong check value
 */
void makeTestImage(TestBytes &image, uint32_t size, bool good)
{
    uint32_t covered = size - kTestImageCheckBytes;

    for (uint32_t i = 0U; i < covered; i++) {
        image[i] = static_cast<uint8_t>((i * 31U) + 7U);
    }
    putU32(&image[covered], crc32_compute(image.data(), covered) ^ (good ? 0U : 1U));
}

/**
 * @brief Sends a test image: BEGIN, CHUNKs of up to `step` bytes, END, stop-and-wait.
 *
 * @param[in,out] rig    the controller
 * @param[in]     image  the bytes
 * @param[in]     size   image length
 * @param[in]     step   chunk size, 1 to 256
 * @return END's answers, or the first answers that were not DONE
 */
Answer sendTestImage(Rig &rig, const uint8_t *image, uint32_t size, uint32_t step)
{
    Answer a = begin(rig, kTestImageNumber, size);

    for (uint32_t offset = 0U; done(a) && (offset < size); offset += step) {
        uint32_t n = ((size - offset) < step) ? (size - offset) : step;
        a = chunk(rig, kTestImageNumber, offset, &image[offset], n);
    }
    return done(a) ? shortCommand(rig, UploadId::kEnd, kTestImageNumber) : a;
}

/**
 * @brief A trial in slot B whose three tests have passed: started, EVENT 0x0006, a good test
 * image (its answers pass the data-link test), EVENT 0x000A.
 *
 * @param[in,out] rig  the controller
 * @return true if every step went as expected
 */
bool passedTrial(Rig &rig)
{
    static TestBytes image;

    startRig(rig, Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B, 0U);
    rig.mm.poll(0U);
    makeTestImage(image, 100U, true);
    bool ok = done(sendTestImage(rig, image.data(), 100U, kChunkMax));
    rig.mm.poll(1U);
    return ok && rig.mm.trialTestsPassed();
}

/**
 * @brief Returns true if the boot state is the given one.
 *
 * @param[in]     confirmed      confirmed slot code
 * @param[in]     trial          trial slot code
 * @param[in]     failed_starts  failed starts
 * @return true if fake.state holds these values
 */
bool stateIs(uint8_t confirmed, uint8_t trial, uint8_t failed_starts)
{
    return fake.state_valid && (fake.state.confirmed == confirmed) &&
           (fake.state.trial == trial) && (fake.state.failed_starts == failed_starts);
}

/**
 * @brief Returns true if the trial was given up as the design says: boot state confirmed A,
 * trial none, failed starts 0; then a drain of the link and one restart, after every frame.
 *
 * @return true if so
 */
bool givenUpToA()
{
    return stateIs(BOOT_SLOT_A, BOOT_SLOT_NONE, 0U) && (fake.restarts == 1U) &&
           (fake.drains_at_restart >= 1U) && (fake.sent_at_restart == fake_link.sent_bytes);
}

/* ---- Mode at start ----------------------------------------------------------------------- */

/**
 * @brief Mode at start: TRIAL only if the running slot is the boot state's trial slot.
 */
void testModeAtStart()
{
    {
        Rig rig;
        startRig(rig, Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B, 0U);
        report((rig.mm.mode() == HealthMode::kTrial) && (rig.mm.trialSecondsLeft(0U) == 120U) &&
                   rig.mm.selfTestPassed(),
               "start: running B, boot state confirmed A trial B -> TRIAL, 120 s left, "
               "self-test passed");
    }
    {
        Rig rig;
        startRig(rig, Slot::kA, BOOT_SLOT_A, BOOT_SLOT_B, 0U);
        report((rig.mm.mode() == HealthMode::kOperate) && (rig.mm.trialSecondsLeft(0U) == 0U),
               "start: running A while the boot state names B the trial -> OPERATE, 0 s left");
    }
    {
        Rig rig;
        startRig(rig, Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
        report(rig.mm.mode() == HealthMode::kOperate,
               "start: running A, confirmed A, no trial -> OPERATE");
    }
    {
        Rig rig;
        prepare(Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B);
        fake.state_valid = false;
        rig.mm.start(Slot::kB, 0U);
        report(rig.mm.mode() == HealthMode::kOperate,
               "start: no valid boot state (the record says trial B) -> OPERATE: no trial known");
    }
    {
        Rig rig;
        startRig(rig, Slot::kNone, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
        report((rig.mm.mode() == HealthMode::kOperate) && !rig.mm.selfTestPassed(),
               "start: running slot not known -> OPERATE, self-test FAILED");
    }
}

/* ---- Events and trial time --------------------------------------------------------------- */

/**
 * @brief EVENT 0x0006 once after start in TRIAL, its bytes, and none in OPERATE.
 */
void testTrialStarted()
{
    Rig rig;
    static SentLog log;

    startRig(rig, Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B, 0U);
    rig.mm.poll(12345U);
    rig.mm.poll(12346U);
    uint32_t n = decodeRange(0U, fake_link.sent_bytes, log);
    /* report no. | onboard time | controller time 12 s | kind EVENT | code 0x0006 */
    const std::array<uint8_t, 15> want = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0x0C, 0x00, 0x00, 0x00, 0x02, 0x06, 0x00};
    report((n == 1U) && (log[0].type == FrameType::kReport) && (log[0].seq == 1U) &&
               (log[0].length == 15U) && (std::memcmp(log[0].payload.data(), want.data(), 15U) == 0),
           "TRIAL: EVENT 0x0006 once over two passes; 15-byte payload in Interfaces Sec. 5.4 "
           "order, Type 0x03, kind 0x02, controller time 12 s");

    Rig operate;
    startRig(operate, Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
    operate.mm.poll(10U);
    operate.mm.poll(kTrialMs + 10U);
    report((fake_link.sent_bytes == 0U) && (fake.restarts == 0U) && (fake.state_writes == 0U),
           "OPERATE: no event, no boot-state write, no restart, also after 120 s");
}

/**
 * @brief HEALTH's trial time left: 120 s, counting down, rounded up, 0 at the end.
 */
void testTrialTime()
{
    Rig rig;
    constexpr uint32_t kStart = 5000U;

    startRig(rig, Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B, kStart);
    bool ok = (rig.mm.trialSecondsLeft(kStart) == 120U) &&
              (rig.mm.trialSecondsLeft(kStart + 999U) == 120U) &&
              (rig.mm.trialSecondsLeft(kStart + 1000U) == 119U) &&
              (rig.mm.trialSecondsLeft(kStart + 119000U) == 1U) &&
              (rig.mm.trialSecondsLeft(kStart + 119999U) == 1U) &&
              (rig.mm.trialSecondsLeft(kStart + 120000U) == 0U) &&
              (rig.mm.trialSecondsLeft(kStart + 500000U) == 0U);
    report(ok, "trial time left: 120 at start, 119 after 1 s, 1 in the last second, 0 at 120 s");

    Rig wrap;
    startRig(wrap, Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B, 0xFFFFFC18U);   /* 1 s before the wrap */
    report(wrap.mm.trialSecondsLeft(0x000003E8U) == 118U,
           "trial time left across the millisecond count's wrap: 2 s in -> 118");
}

/* ---- Self-test ---------------------------------------------------------------------------- */

/**
 * @brief The self-test against synthetic images in the fake flash.
 */
void testSelfTest()
{
    fakeReset();
    putImage(Slot::kB, kBodyBytes);
    bool good = ModeManager::selfTest(Slot::kB);
    uint8_t *image = slotBytes(Slot::kB);

    image[kHeader + 10U] = static_cast<uint8_t>(image[kHeader + 10U] ^ 0x01U);
    bool body_flip = ModeManager::selfTest(Slot::kB);
    image[kHeader + 10U] = static_cast<uint8_t>(image[kHeader + 10U] ^ 0x01U);
    image[IMAGE_OFFSET_BODY_CHECK] = static_cast<uint8_t>(image[IMAGE_OFFSET_BODY_CHECK] ^ 0x80U);
    bool check_flip = ModeManager::selfTest(Slot::kB);
    report(good && !body_flip && !check_flip,
           "self-test: good body passes; one body bit or one check-value bit flipped fails");

    putImage(Slot::kB, kBodyMax);
    bool longest = ModeManager::selfTest(Slot::kB);
    putU32(&image[IMAGE_OFFSET_LENGTH], kBodyMax + 1U);
    bool too_long = ModeManager::selfTest(Slot::kB);
    putU32(&image[IMAGE_OFFSET_LENGTH], 0xFFFFFFFFU);
    bool huge = ModeManager::selfTest(Slot::kB);
    report(longest && !too_long && !huge && (fake.bad_reads == 0U),
           "self-test: the longest body a slot holds (40 768 bytes) passes; length + 1 and "
           "2^32 - 1 fail before any body byte is read");

    putImage(Slot::kB, kBodyBytes);
    fake.ecc_error = true;
    bool ecc = ModeManager::selfTest(Slot::kB);
    fake.ecc_error = false;
    report(!ecc && !ModeManager::selfTest(Slot::kNone) && !ModeManager::selfTest(toSlot(3U)),
           "self-test: a flash read error (ECC) fails; slot none or 3 fails without a read");
}

/**
 * @brief Loads an image file.
 *
 * @param[in]     dir   directory
 * @param[in]     name  file name
 * @param[out]    buf   the bytes
 * @return bytes read; 0 if the file could not be read
 */
uint32_t load(const char *dir, const char *name, SlotImage &buf)
{
    char path[1024];
    size_t n = 0U;

    std::snprintf(path, sizeof path, "%s/%s", dir, name);
    std::FILE *f = std::fopen(path, "rb");
    if (f != nullptr) {
        n = std::fread(buf.data(), 1U, buf.size(), f);
        (void)std::fclose(f);   /* read-only file; nothing to lose on a close error */
    }
    if (n == 0U) {
        std::printf("cannot read %s\n", path);
    }
    return static_cast<uint32_t>(n);
}

/**
 * @brief The self-test on the build's real signed images (tools/make_image.py wrote their
 * headers with zlib's CRC-32).
 *
 * @param[in]     dir  directory with firmware-A.img and firmware-B.img
 */
void testSelfTestRealImages(const char *dir)
{
    static SlotImage a;
    static SlotImage b;
    uint32_t a_size = load(dir, "firmware-A.img", a);
    uint32_t b_size = load(dir, "firmware-B.img", b);

    if ((a_size == 0U) || (b_size == 0U)) {
        report(false, "self-test on real images: could not read the image files");
        return;
    }
    fakeReset();
    std::memcpy(slotBytes(Slot::kA), a.data(), a_size);
    std::memcpy(slotBytes(Slot::kB), b.data(), b_size);
    bool a_ok = ModeManager::selfTest(Slot::kA);
    bool b_ok = ModeManager::selfTest(Slot::kB);
    uint8_t *flip = &slotBytes(Slot::kB)[kFlipOffset];
    *flip = static_cast<uint8_t>(*flip ^ 0xFFU);
    report(a_ok && b_ok && !ModeManager::selfTest(Slot::kB),
           "self-test on firmware-A.img in slot A and firmware-B.img in slot B: passes; "
           "byte 0x600 of B inverted: fails");
}

/**
 * @brief A failed self-test gives a trial up (detail 0x01); in OPERATE it is only reported.
 */
void testSelfTestFailure()
{
    Rig rig;

    prepare(Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B);
    slotBytes(Slot::kB)[kHeader] = static_cast<uint8_t>(slotBytes(Slot::kB)[kHeader] ^ 0x01U);
    rig.mm.start(Slot::kB, 0U);
    bool first = !rig.mm.selfTestPassed() && (rig.mm.firstFailedTest() == TrialTest::kSelfTest);
    rig.mm.poll(50U);
    bool sent = sentIs(0U, {ev(EventCode::kTrialStarted), ev(EventCode::kTestsFailed, 0x01U),
                            ev(EventCode::kGivenUp)});
    report(first && sent && (framesBefore(fake.sent_at_state_write) == 2U) && givenUpToA() &&
               (fake.state_writes == 1U),
           "TRIAL, self-test fails: EVENT 0x0006, 0x000B detail 0x01, boot state written "
           "(confirmed A, trial none, 0 starts), EVENT 0x0008, link drained, one restart");

    Rig operate;
    prepare(Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE);
    slotBytes(Slot::kA)[kHeader] = static_cast<uint8_t>(slotBytes(Slot::kA)[kHeader] ^ 0x01U);
    operate.mm.start(Slot::kA, 0U);
    operate.mm.poll(50U);
    report(!operate.mm.selfTestPassed() && (fake_link.sent_bytes == 0U) &&
               (fake.state_writes == 0U) && (fake.restarts == 0U),
           "OPERATE, self-test fails: reported in HEALTH only (no event, no write, no restart)");
}

/**
 * @brief The test-image rules for an identifier outside UploadId: accept() and run() refuse.
 *
 * parse() never produces such a command; a corrupted UpdateCommand could. Passes if both
 * answer 0x06 (Interfaces Sec. 4.2) and the transfer state is not changed (a following
 * BEGIN is still accepted).
 */
void testTestImageOutsideEnum()
{
    Rig rig;
    UpdateCommand cmd{};

    startRig(rig, Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B, 0U);
    rig.mm.poll(0U);
    cmd.id = static_cast<UploadId>(0x7FU);
    cmd.image_number = kTestImageNumber;
    cmd.value = 40U;
    bool refused = (rig.mm.acceptTestImage(cmd) == AnswerCode::kImpossible) &&
                   (rig.mm.runTestImage(cmd) == AnswerCode::kImpossible);
    cmd.id = UploadId::kBegin;
    report(refused && (rig.mm.acceptTestImage(cmd) == AnswerCode::kSuccess),
           "test image: an identifier outside UploadId (0x7F): accept and run give 0x06, "
           "state unchanged");
}

/* ---- Data-link test ----------------------------------------------------------------------- */

/**
 * @brief The data-link test: passes on the first answer sent whole, fails on any failed send.
 */
void testDataLink()
{
    static TestBytes image;
    makeTestImage(image, 40U, true);
    {
        Rig rig;
        startRig(rig, Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B, 0U);
        rig.mm.poll(0U);
        /* The test image through the mode manager directly, without answers. */
        UpdateCommand cmd{};
        std::array<uint8_t, 7U + 40U> p{};
        p[0] = static_cast<uint8_t>(UploadId::kBegin);
        p[1] = 0xFFU;
        p[2] = 0xFFU;
        putU32(&p[3], 40U);
        bool ok = UpdateReceiver::parse(p.data(), 7U, cmd) &&
                  (rig.mm.acceptTestImage(cmd) == AnswerCode::kSuccess) &&
                  (rig.mm.runTestImage(cmd) == AnswerCode::kSuccess);
        p[0] = static_cast<uint8_t>(UploadId::kChunk);
        putU32(&p[3], 0U);
        std::memcpy(&p[7], image.data(), 40U);
        ok = ok && UpdateReceiver::parse(p.data(), 47U, cmd) &&
             (rig.mm.acceptTestImage(cmd) == AnswerCode::kSuccess) &&
             (rig.mm.runTestImage(cmd) == AnswerCode::kSuccess);
        p[0] = static_cast<uint8_t>(UploadId::kEnd);
        ok = ok && UpdateReceiver::parse(p.data(), 3U, cmd) &&
             (rig.mm.acceptTestImage(cmd) == AnswerCode::kSuccess) &&
             (rig.mm.runTestImage(cmd) == AnswerCode::kSuccess);
        bool before = !rig.mm.trialTestsPassed() &&
                      (rig.mm.firstFailedTest() == TrialTest::kNone);
        rig.sender.sendAnswer(1U, AnswerKind::kReceived, kReceivedCode);
        report(ok && before && rig.mm.trialTestsPassed(),
               "data-link test: self-test and test image passed, no answer sent yet -> not all "
               "passed; one answer sent whole -> all passed");
    }
    {
        Rig rig;
        startRig(rig, Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B, 0U);
        rig.mm.poll(0U);
        fake_link.fail_sends = 1U;
        rig.sender.sendAnswer(1U, AnswerKind::kReceived, kReceivedCode);
        bool failed = rig.mm.firstFailedTest() == TrialTest::kDataLink;
        rig.mm.poll(10U);
        report(failed && sentIs(0U, {ev(EventCode::kTrialStarted),
                                     ev(EventCode::kTestsFailed, 0x02U), ev(EventCode::kGivenUp)}) &&
                   givenUpToA(),
               "data-link test: an answer whose send fails (stalled transmitter) -> EVENT 0x000B "
               "detail 0x02, given up, restart");
    }
    {
        Rig rig;
        bool passed = passedTrial(rig);
        uint32_t mark = fake_link.sent_bytes;
        fake_link.fail_sends = 1U;
        rig.sender.sendAnswer(1U, AnswerKind::kReceived, kReceivedCode);
        rig.mm.poll(20U);
        report(passed && sentIs(mark, {ev(EventCode::kTestsFailed, 0x02U), ev(EventCode::kGivenUp)}) &&
                   givenUpToA(),
               "data-link test: a send fails after EVENT 0x000A, before CONFIRM -> 0x000B detail "
               "0x02, given up");
    }
}

/* ---- Test image ---------------------------------------------------------------------------- */

/**
 * @brief The test image's rules: modes, lengths, offsets, the check value, no flash touched.
 */
void testTestImageRules()
{
    static TestBytes image;
    {
        Rig rig;
        startRig(rig, Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
        report(refusedOnly(begin(rig, kTestImageNumber, 100U), AnswerCode::kImpossible) &&
                   (fake.erases == 0U),
               "OPERATE: UPLOAD BEGIN image 0xFFFF REFUSED 0x06 only, nothing erased");
    }
    Rig rig;
    startRig(rig, Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B, 0U);
    uint8_t one = 0x11U;

    report(refusedOnly(chunk(rig, kTestImageNumber, 0U, &one, 1U), AnswerCode::kImpossible) &&
               refusedOnly(shortCommand(rig, UploadId::kEnd, kTestImageNumber),
                           AnswerCode::kImpossible) &&
               refusedOnly(shortCommand(rig, UploadId::kCancel, kTestImageNumber),
                           AnswerCode::kImpossible),
           "TRIAL: test-image CHUNK, END, CANCEL before BEGIN: REFUSED 0x06 only");
    report(refusedOnly(begin(rig, kTestImageNumber, 0U), AnswerCode::kImpossible) &&
               refusedOnly(begin(rig, kTestImageNumber, 4U), AnswerCode::kImpossible) &&
               refusedOnly(begin(rig, kTestImageNumber, 1025U), AnswerCode::kImpossible) &&
               done(begin(rig, kTestImageNumber, 5U)) && done(begin(rig, kTestImageNumber, 1024U)),
           "TRIAL: test-image BEGIN length 0, 4, 1025 REFUSED 0x06; 5 and 1024 DONE");

    makeTestImage(image, 300U, true);
    bool ok = done(begin(rig, kTestImageNumber, 300U));
    ok = ok && refusedOnly(chunk(rig, kTestImageNumber, 8U, &image[8], 8U), AnswerCode::kImpossible);
    ok = ok && done(chunk(rig, kTestImageNumber, 0U, image.data(), 3U));
    ok = ok && refusedOnly(chunk(rig, kTestImageNumber, 0U, image.data(), 3U),
                           AnswerCode::kImpossible);
    ok = ok && done(chunk(rig, kTestImageNumber, 3U, &image[3], kChunkMax));
    ok = ok && refusedOnly(chunk(rig, kTestImageNumber, 259U, &image[259], 42U),
                           AnswerCode::kImpossible);
    ok = ok && refusedOnly(shortCommand(rig, UploadId::kEnd, kTestImageNumber),
                           AnswerCode::kImpossible);
    report(ok, "TRIAL: CHUNK at 8 where 0 is next, a repeated CHUNK, a CHUNK past the length, "
               "END before the last byte: REFUSED 0x06; a 3-byte CHUNK is fine (RAM, no flash "
               "words)");

    ok = done(chunk(rig, kTestImageNumber, 259U, &image[259], 41U));
    ok = ok && done(shortCommand(rig, UploadId::kEnd, kTestImageNumber));
    ok = ok && (rig.image.result() == TestImageResult::kPassed);
    ok = ok && refusedOnly(chunk(rig, kTestImageNumber, 300U, &one, 1U), AnswerCode::kImpossible);
    ok = ok && refusedOnly(shortCommand(rig, UploadId::kEnd, kTestImageNumber),
                           AnswerCode::kImpossible);
    ok = ok && done(shortCommand(rig, UploadId::kCancel, kTestImageNumber));
    ok = ok && refusedOnly(shortCommand(rig, UploadId::kCancel, kTestImageNumber),
                           AnswerCode::kImpossible);
    report(ok, "TRIAL: 300-byte test image, right check value: END DONE 0x00, test passed; then "
               "CHUNK and END REFUSED 0x06, CANCEL DONE, a second CANCEL REFUSED 0x06");

    makeTestImage(image, kTestImageMaxBytes, true);
    ok = done(sendTestImage(rig, image.data(), kTestImageMaxBytes, kChunkMax));
    const uint8_t anchor[13] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', 0, 0, 0, 0};
    std::array<uint8_t, 13> anchored{};
    std::memcpy(anchored.data(), anchor, 13U);
    putU32(&anchored[9], kCheckValue123456789);
    ok = ok && done(sendTestImage(rig, anchored.data(), 13U, kChunkMax));
    report(ok && (rig.image.result() == TestImageResult::kPassed),
           "TRIAL: 1 024-byte test image in four chunks, and \"123456789\" + 0xCBF43926 (the "
           "standard CRC-32 check value): END DONE 0x00");

    report(refusedOnly(begin(rig, 1U, 1000U), AnswerCode::kTrial) &&
               refusedOnly(chunk(rig, 1U, 0U, &one, 1U), AnswerCode::kTrial),
           "TRIAL: UPLOAD BEGIN and CHUNK with image number 1: REFUSED 0x07 only (update "
           "receiver, unchanged)");

    UpdateCommand other{};
    other.id = UploadId::kBegin;
    other.image_number = 1U;
    other.value = 100U;
    report(rig.image.accept(other) == AnswerCode::kImpossible,
           "test image: a command with another image number reaching it directly: 0x06");

    report((fake.erases == 0U) && (fake.programs == 0U) && (fake.outside == 0U) &&
               (fake.reloads == 0U),
           "TRIAL: after every test-image command above, no flash page erased or written");
}

/**
 * @brief A test image with a wrong check value: REFUSED 0x08 at END, then 0x000B detail 0x03,
 * the give-up and the restart, in that order on the wire.
 */
void testTestImageFails()
{
    static TestBytes image;
    Rig rig;

    startRig(rig, Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B, 0U);
    rig.mm.poll(0U);
    uint32_t mark = fake_link.sent_bytes;
    makeTestImage(image, 40U, false);
    Answer end = sendTestImage(rig, image.data(), 40U, kChunkMax);
    rig.mm.poll(10U);
    report(receivedThenRefused(end, AnswerCode::kDamaged) &&
               sentIs(mark, {kRecv, kDone, kRecv, kDone, kRecv,
                             ans(AnswerKind::kRefused, AnswerCode::kDamaged),
                             ev(EventCode::kTestsFailed, 0x03U), ev(EventCode::kGivenUp)}) &&
               givenUpToA(),
           "TRIAL, test image with a wrong check value: END RECEIVED, REFUSED 0x08, then EVENT "
           "0x000B detail 0x03, boot state given up, EVENT 0x0008, restart");
}

/**
 * @brief All three tests passed: EVENT 0x000A once; trial time over without CONFIRM: give up.
 */
void testPassedAndTimeout()
{
    static TestBytes image;
    Rig rig;
    constexpr uint32_t kStart = 1000U;

    startRig(rig, Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B, kStart);
    rig.mm.poll(kStart);
    uint32_t mark = fake_link.sent_bytes;
    makeTestImage(image, 40U, true);
    bool ok = done(sendTestImage(rig, image.data(), 40U, kChunkMax));
    rig.mm.poll(kStart + 10U);
    rig.mm.poll(kStart + 20U);
    report(ok && rig.mm.trialTestsPassed() &&
               sentIs(mark, {kRecv, kDone, kRecv, kDone, kRecv, kDone,
                             ev(EventCode::kTestsPassed)}),
           "TRIAL, good test image: END DONE 0x00, then EVENT 0x000A once over two passes");

    mark = fake_link.sent_bytes;
    rig.mm.poll(kStart + kTrialMs - 1U);
    bool early = (fake.restarts == 0U) && (fake_link.sent_bytes == mark);
    rig.mm.poll(kStart + kTrialMs);
    report(early && sentIs(mark, {ev(EventCode::kGivenUp)}) && givenUpToA(),
           "TRIAL, tests passed, no CONFIRM: nothing at 119.999 s; at 120 s EVENT 0x0008 (no "
           "0x000B), boot state given up, restart");

    Rig fails;
    startRig(fails, Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B, 0U);
    fails.mm.poll(0U);
    fake.fail_state_write = true;
    mark = fake_link.sent_bytes;
    fails.mm.poll(kTrialMs);
    report((fake_link.sent_bytes == mark) && (fake.restarts == 1U) && (fake.state_writes == 1U) &&
               stateIs(BOOT_SLOT_A, BOOT_SLOT_B, 1U),
           "give-up whose boot-state write fails: no EVENT 0x0008, restart anyway (the "
           "bootloader's start count ends the trial, REQ-015)");
}

/* ---- CONFIRM -------------------------------------------------------------------------------- */

/**
 * @brief CONFIRM's checks on arrival, in order, and their refusal codes.
 */
void testConfirmRefusals()
{
    {
        Rig rig;
        startRig(rig, Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
        sendArm(rig, kConfirm, rig.seq + 1U, 0U);
        uint32_t seq = rig.seq++;
        Answer a = dangerous(rig, kConfirm, seq, 0U);
        report(refusedOnly(a, AnswerCode::kMode) &&
                   (rig.arm.check(kConfirm, seq, 0U) == AnswerCode::kSuccess),
               "OPERATE, ARM + CONFIRM: REFUSED 0x05 only; the ARM is not used up (mode first)");
    }
    Rig rig;
    bool passed = passedTrial(rig);
    report(passed && refusedOnly(dangerous(rig, kConfirm, rig.seq++, 10U), AnswerCode::kNotArmed),
           "TRIAL, tests passed, CONFIRM without ARM: REFUSED 0x01 only");

    sendArm(rig, kSwitch, rig.seq + 1U, 10U);
    report(refusedOnly(dangerous(rig, kConfirm, rig.seq++, 10U), AnswerCode::kNotArmed),
           "ARM naming SWITCH, then CONFIRM: REFUSED 0x01");

    sendArm(rig, kConfirm, rig.seq + 5U, 10U);
    report(refusedOnly(dangerous(rig, kConfirm, rig.seq++, 10U), AnswerCode::kNotArmed),
           "ARM naming CONFIRM with another sequence number, then CONFIRM: REFUSED 0x01");

    sendArm(rig, kConfirm, rig.seq + 1U, 10U);
    report(refusedOnly(dangerous(rig, kConfirm, rig.seq++, 10U + kArmExpiryMs),
                       AnswerCode::kArmExpired),
           "ARM, then CONFIRM 30 s later: REFUSED 0x02");

    const std::array<uint8_t, 2> two = {kConfirm, 0x00U};
    report(rig.mm.acceptDangerous(two.data(), 2U, 1U, 10U, rig.arm) == AnswerCode::kImpossible,
           "CONFIRM of 2 bytes: REFUSED 0x06 (no fields, Interfaces Sec. 5.1)");

    Rig untested;
    startRig(untested, Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B, 0U);
    untested.mm.poll(0U);
    report(refusedOnly(dangerous(untested, kConfirm, untested.seq++, 10U), AnswerCode::kNotArmed),
           "TRIAL, tests not passed, CONFIRM without ARM: REFUSED 0x01 (arming checked before the "
           "tests)");
    sendArm(untested, kConfirm, untested.seq + 1U, 10U);
    uint32_t seq = untested.seq++;
    Answer first = dangerous(untested, kConfirm, seq, 10U);
    Answer again = dangerous(untested, kConfirm, seq, 11U);
    report(refusedOnly(first, AnswerCode::kTestsNotPassed) &&
               refusedOnly(again, AnswerCode::kNotArmed) && (fake.state_writes == 0U),
           "TRIAL, tests not passed, ARM + CONFIRM: REFUSED 0x0F; the ARM is used up (the same "
           "CONFIRM again: 0x01); boot state untouched");
}

/**
 * @brief CONFIRM accepted: boot state, EVENT 0x0007 before DONE, mode OPERATE; and a failed
 * boot-state write.
 */
void testConfirmRuns()
{
    {
        Rig rig;
        bool passed = passedTrial(rig);
        uint32_t mark = fake_link.sent_bytes;
        Answer a = armedCommand(rig, kConfirm, 50U);
        bool sent = sentIs(mark, {kRecv, kDone, kRecv, ev(EventCode::kConfirmed), kDone});
        bool state = stateIs(BOOT_SLOT_B, BOOT_SLOT_NONE, 0U);
        bool operate = (rig.mm.mode() == HealthMode::kOperate) && (rig.mm.trialSecondsLeft(60U) == 0U);
        uint32_t writes = fake.state_writes;
        rig.mm.poll(kTrialMs + 1000U);
        report(passed && done(a) && sent && state && operate && (fake.restarts == 0U) &&
                   (fake.state_writes == writes),
               "TRIAL, tests passed, ARM + CONFIRM: RECEIVED, EVENT 0x0007, DONE 0x00; boot state "
               "confirmed B, trial none, 0 starts; OPERATE; no time-out after 121 s");
        report(refusedOnly(armedCommand(rig, kConfirm, 70U), AnswerCode::kMode),
               "after CONFIRM: a second ARM + CONFIRM REFUSED 0x05 (OPERATE)");
    }
    {
        Rig rig;
        bool passed = passedTrial(rig);
        fake.fail_state_write = true;
        uint32_t mark = fake_link.sent_bytes;
        Answer a = armedCommand(rig, kConfirm, 50U);
        bool sent = sentIs(mark, {kRecv, kDone, kRecv,
                                  ans(AnswerKind::kRefused, AnswerCode::kImpossible)});
        bool trial = (rig.mm.mode() == HealthMode::kTrial) && stateIs(BOOT_SLOT_A, BOOT_SLOT_B, 1U);
        fake.fail_state_write = false;
        Answer retry = armedCommand(rig, kConfirm, 60U);
        report(passed && receivedThenRefused(a, AnswerCode::kImpossible) && sent && trial &&
                   done(retry) && stateIs(BOOT_SLOT_B, BOOT_SLOT_NONE, 0U),
               "CONFIRM whose boot-state write fails: RECEIVED, REFUSED 0x06, no EVENT 0x0007, "
               "still TRIAL; a new ARM + CONFIRM then succeeds");
    }
}

/* ---- SWITCH --------------------------------------------------------------------------------- */

/**
 * @brief SWITCH: mode, arming, the boot state, the restart after DONE, a failed write.
 */
void testSwitch()
{
    {
        Rig rig;
        startRig(rig, Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B, 0U);
        report(refusedOnly(armedCommand(rig, kSwitch, 0U), AnswerCode::kMode),
               "TRIAL: ARM + SWITCH REFUSED 0x05 only");
    }
    {
        Rig rig;
        startRig(rig, Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
        bool no_arm = refusedOnly(dangerous(rig, kSwitch, rig.seq++, 0U), AnswerCode::kNotArmed);
        sendArm(rig, kConfirm, rig.seq + 1U, 0U);
        bool wrong = refusedOnly(dangerous(rig, kSwitch, rig.seq++, 0U), AnswerCode::kNotArmed);
        report(no_arm && wrong && (fake.state_writes == 0U),
               "OPERATE: SWITCH without ARM, or after an ARM naming CONFIRM: REFUSED 0x01 only");

        uint32_t mark = fake_link.sent_bytes;
        Answer a = armedCommand(rig, kSwitch, 10U);
        bool before = (fake.restarts == 0U) && stateIs(BOOT_SLOT_A, BOOT_SLOT_B, 0U) &&
                      sentIs(mark, {kRecv, kDone, kRecv, kDone});
        rig.mm.poll(11U);
        report(done(a) && before && (fake.restarts == 1U) && (fake.drains_at_restart >= 1U) &&
                   (fake.sent_at_restart == fake_link.sent_bytes),
               "OPERATE, running A, ARM + SWITCH: RECEIVED, DONE 0x00; boot state confirmed A, "
               "trial B, 0 starts; on the next pass the link is drained and the chip restarts");
    }
    {
        Rig rig;
        startRig(rig, Slot::kB, BOOT_SLOT_B, BOOT_SLOT_NONE, 0U);
        report(done(armedCommand(rig, kSwitch, 0U)) && stateIs(BOOT_SLOT_B, BOOT_SLOT_A, 0U),
               "OPERATE, running B: ARM + SWITCH writes confirmed B, trial A");
    }
    {
        Rig rig;
        startRig(rig, Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
        fake.fail_state_write = true;
        Answer a = armedCommand(rig, kSwitch, 0U);
        rig.mm.poll(1U);
        report(receivedThenRefused(a, AnswerCode::kImpossible) && (fake.restarts == 0U) &&
                   stateIs(BOOT_SLOT_A, BOOT_SLOT_NONE, 0U),
               "SWITCH whose boot-state write fails: RECEIVED, REFUSED 0x06, no restart");
    }
}

}  // namespace

/**
 * @brief Runs every mode-manager test.
 *
 * @param[in]     argc  2
 * @param[in]     argv  argv[1]: directory with firmware-A.img and firmware-B.img
 * @return 0 if all passed, 1 if any failed, 2 on a usage error
 */
int main(int argc, char **argv)
{
    if (argc != 2) {
        std::printf("usage: %s <src image dir>\n", argv[0]);
        return 2;
    }
    testModeAtStart();
    testTrialStarted();
    testTrialTime();
    testSelfTest();
    testSelfTestRealImages(argv[1]);
    testSelfTestFailure();
    testDataLink();
    testTestImageRules();
    testTestImageOutsideEnum();
    testTestImageFails();
    testPassedAndTimeout();
    testConfirmRefusals();
    testConfirmRuns();
    testSwitch();
    if (failures == 0U) {
        std::printf("mode manager unit tests: all %u passed\n", tests);
        return 0;
    }
    std::printf("mode manager unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
