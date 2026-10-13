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
 * Owner's decisions of 13 Oct 2026 under test here (letters of the controller audit): B (restart
 * count cleared after kNormalRunMs), C (EVENT 0x0001 after a clock fault), E (SWITCH reads the
 * boot state again), F(a) (SWITCH checks the other slot's image), G (give-up request), L (failed
 * self-test in OPERATE restarts, counted), N (0x11 when the boot state cannot be stored), the
 * fall-back EVENT 0x0002, and the fault rule (a corrupted value reaches the fault reaction,
 * caught by fake_fault.cpp); and the decisions numbered 2 to 5: the fault kind of EVENT 0x0001,
 * the bootloader's fault location sent as EVENT 0x0005, one EVENT 0x0002 per refused or skipped
 * copy, EVENT 0x0008 for a give-up handed to the bootloader; each fault site passes its own
 * fault-location code (common/fault_codes.h). Each test's name or comment says what the old code
 * did.
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
#include "backup_layout.h"
#include "boot_slot.hpp"
#include "crc32.h"
#include "fake_fault.hpp"
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

/* The build's signed images (firmware-A.img, firmware-B.img), loaded once by main(), for the
 * SWITCH tests: decision F(a) checks the other slot's image with the bootloader's checks. */
SlotImage g_image_a;
SlotImage g_image_b;
uint32_t g_size_a;
uint32_t g_size_b;

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
    rig.mm.start(running, now, false);
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
        rig.mm.start(Slot::kB, 0U, false);
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
    report(!ecc && !ModeManager::selfTest(Slot::kNone),
           "self-test: a flash read error (ECC) fails; slot none fails without a read");

    volatile int got = -1;
    bool faulted = false;
    faultClear();
    FAULT_CATCH(faulted, got = ModeManager::selfTest(static_cast<Slot>(3U)) ? 1 : 0);
    report(faulted && (got == -1) && (fault_calls == 1U) && (fake.bad_reads == 0U) &&
               (fault_last_location == FAULT_AT_UPDATE_RECEIVER_SLOT_START),
           "self-test: running slot 3 (outside Slot) -> fault reaction in slotStart, fault "
           "location 0x26, no read (was: failed without a read)");
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
    rig.mm.start(Slot::kB, 0U, false);
    bool first = !rig.mm.selfTestPassed() && (rig.mm.firstFailedTest() == TrialTest::kSelfTest);
    rig.mm.poll(50U);
    bool sent = sentIs(0U, {ev(EventCode::kTrialStarted), ev(EventCode::kTestsFailed, 0x01U),
                            ev(EventCode::kGivenUp)});
    report(first && sent && (framesBefore(fake.sent_at_state_write) == 2U) && givenUpToA() &&
               (fake.state_writes == 1U),
           "TRIAL, self-test fails: EVENT 0x0006, 0x000B detail 0x01, boot state written "
           "(confirmed A, trial none, 0 starts), EVENT 0x0008, link drained, one restart");

    Rig operate;
    uint32_t mark = 0U;
    prepare(Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE);
    slotBytes(Slot::kA)[kHeader] = static_cast<uint8_t>(slotBytes(Slot::kA)[kHeader] ^ 0x01U);
    operate.mm.start(Slot::kA, 0U, false);
    operate.mm.poll(50U);
    report(!operate.mm.selfTestPassed() && sentIs(0U, {ev(EventCode::kSelfTestFailed)}) &&
               (fake.state_writes == 0U) && (fake.restarts == 0U) &&
               (fake.counted_restarts == 1U) && (fake.fault_reason == BACKUP_FAULT_SELF_TEST) &&
               fakeBackupGet(BACKUP_PAIR_FAULT, mark) && (mark == BACKUP_FAULT_SELF_TEST) &&
               (fake.drains_at_restart >= 1U) && (fake.sent_at_restart == fake_link.sent_bytes),
           "L: OPERATE, self-test fails -> EVENT 0x0004, link drained, restart with the fault mark "
           "(the bootloader counts it, decision B); no boot-state write (was: HEALTH only, no "
           "restart)");
}

/**
 * @brief The test-image rules for an identifier outside UploadId: accept() and run() are the
 * fault reaction.
 *
 * parse() never produces such a command; only a corrupted UpdateCommand could. Before, both
 * answered 0x06 (fails on the old code: no fault reaction). The transfer state is not changed:
 * a following BEGIN is still accepted.
 */
void testTestImageOutsideEnum()
{
    Rig rig;
    UpdateCommand cmd{};
    volatile AnswerCode code = AnswerCode::kDamaged;
    bool accept_fault = false;
    bool run_fault = false;

    startRig(rig, Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B, 0U);
    rig.mm.poll(0U);
    cmd.id = static_cast<UploadId>(0x7FU);
    cmd.image_number = kTestImageNumber;
    cmd.value = 40U;
    faultClear();
    FAULT_CATCH(accept_fault, code = rig.mm.acceptTestImage(cmd));
    const uint8_t accept_location = fault_last_location;
    FAULT_CATCH(run_fault, code = rig.mm.runTestImage(cmd));
    cmd.id = UploadId::kBegin;
    report(accept_fault && run_fault && (code == AnswerCode::kDamaged) && (fault_calls == 2U) &&
               (accept_location == FAULT_AT_TEST_IMAGE_ACCEPT) &&
               (fault_last_location == FAULT_AT_TEST_IMAGE_RUN) &&
               (rig.mm.acceptTestImage(cmd) == AnswerCode::kSuccess),
           "test image: an identifier outside UploadId (0x7F): accept and run -> fault reaction, "
           "fault locations 0x2B and 0x2C, state unchanged (was: 0x06)");
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
    uint32_t request = 0U;
    bool stored = fakeBackupGet(BACKUP_PAIR_GIVE_UP, request) && (request == BOOT_SLOT_B);
    report(sentIs(mark, {ev(EventCode::kGivenUp)}) &&
               (fake.sent_at_restart == fake_link.sent_bytes) && (fake.restarts == 1U) &&
               (fake.state_writes == 1U) && stateIs(BOOT_SLOT_A, BOOT_SLOT_B, 1U) && stored,
           "G/5: give-up whose boot-state write fails: give-up request for B left in the backup "
           "registers, EVENT 0x0008 before the restart (decision 5; was: no EVENT 0x0008)");

    Rig nowhere;
    startRig(nowhere, Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B, 0U);
    nowhere.mm.poll(0U);
    fake.fail_state_write = true;
    fake.backup_writable = false;
    mark = fake_link.sent_bytes;
    nowhere.mm.poll(kTrialMs);
    report((fake.restarts == 1U) && !fakeBackupGet(BACKUP_PAIR_GIVE_UP, request) &&
               (fake_link.sent_bytes == mark),
           "G/5: neither the boot state nor the request can be stored: no EVENT 0x0008 (not given "
           "up yet), restart anyway (the bootloader's start count ends the trial, REQ-015)");
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

    /* The trial time: CONFIRM is accepted up to 1 ms before kTrialMs and refused 0x05 from
     * kTrialMs on, also while poll() has not yet given the trial up (audit finding #22). Before
     * the check, the late CONFIRM was accepted. */
    {
        Rig early;
        bool earlyPassed = passedTrial(early);
        sendArm(early, kConfirm, early.seq + 1U, kTrialMs - 10U);
        Answer a = dangerous(early, kConfirm, early.seq++, kTrialMs - 1U);
        report(earlyPassed && done(a) && stateIs(BOOT_SLOT_B, BOOT_SLOT_NONE, 0U),
               "TRIAL, tests passed, ARM + CONFIRM at 119.999 s: accepted, boot state confirmed");
    }
    {
        Rig late;
        bool latePassed = passedTrial(late);
        uint32_t writes = fake.state_writes;
        sendArm(late, kConfirm, late.seq + 1U, kTrialMs - 10U);
        uint32_t lateSeq = late.seq++;
        Answer a = dangerous(late, kConfirm, lateSeq, kTrialMs);
        report(latePassed && refusedOnly(a, AnswerCode::kMode) && (fake.state_writes == writes) &&
                   (late.arm.check(kConfirm, lateSeq, kTrialMs) == AnswerCode::kSuccess),
               "TRIAL, tests passed, ARM + CONFIRM at 120 s: REFUSED 0x05 only; boot state "
               "untouched; the ARM is not used up (mode checks first)");
    }
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
                                  ans(AnswerKind::kRefused, AnswerCode::kStateNotStored)});
        bool trial = (rig.mm.mode() == HealthMode::kTrial) && stateIs(BOOT_SLOT_A, BOOT_SLOT_B, 1U);
        fake.fail_state_write = false;
        Answer retry = armedCommand(rig, kConfirm, 60U);
        report(passed && receivedThenRefused(a, AnswerCode::kStateNotStored) && sent && trial &&
                   done(retry) && stateIs(BOOT_SLOT_B, BOOT_SLOT_NONE, 0U),
               "N: CONFIRM whose boot-state write fails: RECEIVED, REFUSED 0x11, no EVENT 0x0007, "
               "still TRIAL; a new ARM + CONFIRM then succeeds (was: 0x06)");
    }
}

/* ---- SWITCH ---------------------------------------------------------------------------- */

/**
 * @brief Puts the build's signed image for a slot into that slot of the fake flash.
 *
 * @param[in]     slot  Slot::kA (firmware-A.img) or Slot::kB (firmware-B.img)
 */
void putSigned(Slot slot)
{
    if (slot == Slot::kA) {
        std::memcpy(slotBytes(Slot::kA), g_image_a.data(), g_size_a);
        return;
    }
    std::memcpy(slotBytes(Slot::kB), g_image_b.data(), g_size_b);
}

/**
 * @brief SWITCH: mode, arming, the boot state, the restart after DONE.
 *
 * The other slot holds the build's signed image, so decision F(a)'s check passes.
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
        putSigned(Slot::kB);
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
               "OPERATE, running A, firmware-B.img in B, ARM + SWITCH: RECEIVED, DONE 0x00; boot "
               "state confirmed A, trial B, 0 starts; on the next pass the link is drained and the "
               "chip restarts");
    }
    {
        Rig rig;
        startRig(rig, Slot::kB, BOOT_SLOT_B, BOOT_SLOT_NONE, 0U);
        putSigned(Slot::kA);
        report(done(armedCommand(rig, kSwitch, 0U)) && stateIs(BOOT_SLOT_B, BOOT_SLOT_A, 0U),
               "OPERATE, running B, firmware-A.img in A: ARM + SWITCH writes confirmed B, trial A");
    }
}

/**
 * @brief Decision E: SWITCH reads the boot state again and runs only from the confirmed slot with
 * no trial set; otherwise RECEIVED, REFUSED 0x05, nothing written.
 *
 * Fails on the old code: it wrote "confirmed = running, trial = other" without looking, so in the
 * second case it would have made A the confirmed slot over B.
 */
void testSwitchStateCheck()
{
    {
        Rig rig;
        startRig(rig, Slot::kA, BOOT_SLOT_A, BOOT_SLOT_B, 0U);
        putSigned(Slot::kB);
        Answer a = armedCommand(rig, kSwitch, 0U);
        rig.mm.poll(1U);
        report((rig.mm.mode() == HealthMode::kOperate) &&
                   receivedThenRefused(a, AnswerCode::kMode) && (fake.state_writes == 0U) &&
                   (fake.restarts == 0U) && stateIs(BOOT_SLOT_A, BOOT_SLOT_B, 1U),
               "E: running A in OPERATE, boot state already has trial B: SWITCH RECEIVED, "
               "REFUSED 0x05, nothing written, no restart");
    }
    {
        Rig rig;
        startRig(rig, Slot::kA, BOOT_SLOT_B, BOOT_SLOT_NONE, 0U);
        putSigned(Slot::kB);
        report(receivedThenRefused(armedCommand(rig, kSwitch, 0U), AnswerCode::kMode) &&
                   (fake.state_writes == 0U) && stateIs(BOOT_SLOT_B, BOOT_SLOT_NONE, 0U),
               "E: running A while the boot state names B confirmed: REFUSED 0x05, B stays "
               "confirmed");
    }
    {
        Rig rig;
        startRig(rig, Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
        putSigned(Slot::kB);
        fake.state.trial = BOOT_SLOT_B;   /* set after start, before SWITCH */
        bool late_trial = receivedThenRefused(armedCommand(rig, kSwitch, 0U), AnswerCode::kMode);
        fake.state.trial = BOOT_SLOT_NONE;
        fake.state_valid = false;          /* the boot state cannot be read at SWITCH */
        bool unreadable = receivedThenRefused(armedCommand(rig, kSwitch, 1U), AnswerCode::kMode);
        report(late_trial && unreadable && (fake.state_writes == 0U),
               "E: a trial set after start, or a boot state that cannot be read at SWITCH: "
               "REFUSED 0x05, nothing written");
    }
}

/**
 * @brief Decision F(a): SWITCH checks the other slot's image with UPLOAD END's checks before it
 * writes; a refused image answers that check's code, nothing written, no restart.
 *
 * Fails on the old code: SWITCH wrote the boot state without checking the image (DONE 0x00).
 */
void testSwitchImageCheck()
{
    struct Case {
        const char *name;
        AnswerCode code;
    };
    const Case cases[] = {
        {"F(a): slot B holds no image (the fake flash's pattern): REFUSED 0x08",
         AnswerCode::kDamaged},
        {"F(a): firmware-B.img with byte 0x600 inverted in slot B: REFUSED 0x08",
         AnswerCode::kDamaged},
        {"F(a): firmware-A.img (linked for slot A) in slot B: REFUSED 0x0E",
         AnswerCode::kOtherSlot},
        {"F(a): firmware-B.img in slot B, a flash read error during the check: REFUSED 0x08",
         AnswerCode::kDamaged},
    };

    for (uint32_t i = 0U; i < 4U; i++) {
        Rig rig;
        startRig(rig, Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
        if (i == 1U) {
            putSigned(Slot::kB);
            slotBytes(Slot::kB)[kFlipOffset] =
                static_cast<uint8_t>(slotBytes(Slot::kB)[kFlipOffset] ^ 0xFFU);
        } else if (i == 2U) {
            std::memcpy(slotBytes(Slot::kB), g_image_a.data(), g_size_a);
        } else if (i == 3U) {
            putSigned(Slot::kB);
            fake.ecc_error = true;
        }
        Answer a = armedCommand(rig, kSwitch, 0U);
        fake.ecc_error = false;
        rig.mm.poll(1U);
        report(receivedThenRefused(a, cases[i].code) && (fake.state_writes == 0U) &&
                   (fake.restarts == 0U) && stateIs(BOOT_SLOT_A, BOOT_SLOT_NONE, 0U),
               cases[i].name);
    }
}

/**
 * @brief Decision N: a SWITCH whose boot-state write fails answers 0x11 and does not restart.
 *
 * Fails on the old code: it answered 0x06.
 */
void testSwitchWriteFails()
{
    Rig rig;
    startRig(rig, Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
    putSigned(Slot::kB);
    fake.fail_state_write = true;
    Answer a = armedCommand(rig, kSwitch, 0U);
    rig.mm.poll(1U);
    report(receivedThenRefused(a, AnswerCode::kStateNotStored) && (fake.restarts == 0U) &&
               stateIs(BOOT_SLOT_A, BOOT_SLOT_NONE, 0U),
           "N: SWITCH whose boot-state write fails: RECEIVED, REFUSED 0x11, no restart (was: "
           "0x06)");
}

/* ---- Events of the start, restart count ------------------------------------------------- */

using FallbackDetail = std::array<uint8_t, 2>;

/**
 * @brief Returns true if the frames sent are exactly EVENT 0x0002 reports with these details
 * (refused copy, check result), in this order.
 *
 * @param[in]     want  the 2-byte details expected, in order
 * @return true if so
 */
bool fallbackEventsAre(std::initializer_list<FallbackDetail> want)
{
    static SentLog log;
    uint32_t n = decodeRange(0U, fake_link.sent_bytes, log);
    bool same = (static_cast<size_t>(n) == want.size());
    uint32_t i = 0U;

    for (const FallbackDetail &w : want) {
        if (!same) {
            break;
        }
        const Sent &s = log[i];
        same = (s.type == FrameType::kReport) && (s.length == 17U) &&
               (s.payload[12] == kReportKindEvent) && (s.payload[13] == 0x02U) &&
               (s.payload[14] == 0x00U) && (s.payload[15] == w[0]) && (s.payload[16] == w[1]);
        i++;
    }
    return same;
}

/**
 * @brief Returns true if the frames sent are exactly EVENT 0x0005 reports with these 2-byte
 * details (where, later faults), in this order, each 17 bytes long.
 *
 * @param[in]     want  the 2-byte details expected, in order
 * @return true if so
 */
bool memoryEventsAre(std::initializer_list<FallbackDetail> want)
{
    static SentLog log;
    uint32_t n = decodeRange(0U, fake_link.sent_bytes, log);
    bool same = (static_cast<size_t>(n) == want.size());
    uint32_t i = 0U;

    for (const FallbackDetail &w : want) {
        if (!same) {
            break;
        }
        const Sent &s = log[i];
        same = (s.type == FrameType::kReport) && (s.length == kEventHeadBytes + 2U) &&
               (s.payload[12] == kReportKindEvent) && (s.payload[13] == 0x05U) &&
               (s.payload[14] == 0x00U) && (s.payload[15] == w[0]) && (s.payload[16] == w[1]);
        i++;
    }
    return same;
}

/**
 * @brief Returns a FALLBACK pair value in the layout of decision 4 (common/backup_layout.h): one
 * byte per copy with its check result, the copy started in the top byte.
 *
 * @param[in]     a        slot A's check result; 0 when not refused
 * @param[in]     b        slot B's check result
 * @param[in]     lr       the last-resort firmware's check result
 * @param[in]     started  the copy started (BACKUP_COPY_...)
 * @return the pair value
 */
uint32_t recordOf(uint32_t a, uint32_t b, uint32_t lr, uint32_t started)
{
    return (started << BACKUP_FALLBACK_STARTED_SHIFT) |
           (lr << BACKUP_FALLBACK_LAST_RESORT_SHIFT) | (b << BACKUP_FALLBACK_B_SHIFT) |
           (a << BACKUP_FALLBACK_A_SHIFT);
}

/**
 * @brief Starts a rig running `running` (OPERATE, no trial) with a FALLBACK record set first.
 *
 * @param[in,out] rig      the controller
 * @param[in]     running  the running slot
 * @param[in]     record   FALLBACK pair value (common/backup_layout.h)
 */
void startWithRecord(Rig &rig, Slot running, uint32_t record)
{
    prepare(running, (running == Slot::kA) ? uint8_t{BOOT_SLOT_A} : uint8_t{BOOT_SLOT_B},
            BOOT_SLOT_NONE);
    fakeBackupSet(BACKUP_PAIR_FALLBACK, record);
    rig.updater.init(running);
    rig.mm.start(running, 0U, false);
}

/**
 * @brief The events of a start: EVENT 0x0001 with the fault kind after a clock fault (decisions
 * C and 2), one EVENT 0x0002 per copy in the bootloader's fall-back record (decision 4), once,
 * then the record is cleared.
 *
 * Fails on the old code: 0x0001 went out with no detail; the record held one refused copy in
 * another layout, so a record of two copies gave one event, and the check results 0x0B to 0x0D
 * were dropped (scratch-copy mutation runs).
 */
void testStartEvents()
{
    {
        Rig rig;
        prepare(Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B);
        rig.updater.init(Slot::kB);
        rig.mm.start(Slot::kB, 0U, true);
        rig.mm.poll(0U);
        rig.mm.poll(1U);
        report(sentIs(0U, {ev(EventCode::kFaultDetected, FAULT_KIND_CORE_CLOCK),
                           ev(EventCode::kTrialStarted)}),
               "2/C: clock fell back to 16 MHz -> EVENT 0x0001 once at start with fault kind 0x01 "
               "(was: no detail), then the trial's 0x0006");
    }
    {
        Rig rig;
        uint32_t left = 0xFFU;
        startWithRecord(rig, Slot::kA, recordOf(0U, CHECK_SIGNATURE_A, 0U, BACKUP_COPY_A));
        rig.mm.poll(0U);
        rig.mm.poll(1U);
        report(fallbackEventsAre({{0x02U, 0x09U}}) &&
                   fakeBackupGet(BACKUP_PAIR_FALLBACK, left) && (left == 0U),
               "record: slot B refused (signature A), A started -> EVENT 0x0002 detail 02 09, "
               "once over two passes; record cleared");
    }
    {
        bool all = true;
        for (uint8_t check : {uint8_t{CHECK_FLASH_READ}, uint8_t{CHECK_TRIAL_STARTS},
                              uint8_t{CHECK_RESTARTS}}) {
            Rig rig;
            startWithRecord(rig, Slot::kA, recordOf(0U, check, 0U, BACKUP_COPY_A));
            rig.mm.poll(0U);
            all = all && fallbackEventsAre({{0x02U, check}});
        }
        report(all, "4: slot B refused with 0x0B (flash read error), 0x0C (trial started too "
                    "often), 0x0D (restarted too often) -> EVENT 0x0002 02 0B, 02 0C, 02 0D (was: "
                    "dropped, highest code 0x0A)");
    }
    {
        Rig rig;
        uint32_t left = 0xFFU;
        startWithRecord(rig, Slot::kA,
                        recordOf(0U, CHECK_TRIAL_STARTS, CHECK_BODY, BACKUP_COPY_A));
        rig.mm.poll(0U);
        rig.mm.poll(1U);
        report(fallbackEventsAre({{0x02U, 0x0CU}, {0x03U, 0x08U}}) &&
                   fakeBackupGet(BACKUP_PAIR_FALLBACK, left) && (left == 0U),
               "4: record of two copies (B 0x0C, last-resort 0x08), A started -> two EVENT 0x0002, "
               "02 0C then 03 08, once; record cleared (the decoder takes every record; a real "
               "start of a main slot leaves at most the trial's)");
    }
    {
        Rig rig;
        uint32_t left = 0xFFU;
        faultClear();
        startWithRecord(rig, Slot::kA,
                        recordOf(0U, CHECK_SIGNATURE_A, 0U, BACKUP_COPY_LAST_RESORT));
        rig.mm.poll(0U);
        bool other = (fake_link.sent_bytes == 0U) && fakeBackupGet(BACKUP_PAIR_FALLBACK, left) &&
                     (left == 0U);
        Rig same;
        startWithRecord(same, Slot::kA, recordOf(CHECK_SIGNATURE_A, 0U, 0U, BACKUP_COPY_A));
        same.mm.poll(0U);
        bool self = (fake_link.sent_bytes == 0U);
        Rig check;
        startWithRecord(check, Slot::kA, recordOf(0U, CHECK_LAST + 1U, 0U, BACKUP_COPY_A));
        check.mm.poll(0U);
        bool past = (fake_link.sent_bytes == 0U);
        Rig half;
        startWithRecord(half, Slot::kA,
                        recordOf(0U, CHECK_SIGNATURE_A, CHECK_LAST + 1U, BACKUP_COPY_A));
        half.mm.poll(0U);
        bool whole = (fake_link.sent_bytes == 0U);
        Rig none;
        startWithRecord(none, Slot::kA, recordOf(0U, 0U, 0U, BACKUP_COPY_A));
        none.mm.poll(0U);
        report(other && self && past && whole && (fake_link.sent_bytes == 0U) &&
                   (fault_calls == 0U),
               "record naming another started copy, the running copy as refused, a check result "
               "past the table (0x11), one good and one bad copy, or no refused copy: dropped "
               "whole and cleared, no event, no fault (stored data, not memory)");
    }
}

/**
 * @brief Decision 3: a fault location the bootloader left goes out once as EVENT 0x0005 with a
 * 2-byte detail, 0x80 added to the code (found by the bootloader) and the number of faults after
 * the first; then it is cleared. A value outside the bootloader's and common/'s ranges (0x01 to
 * 0x1F) is dropped and cleared, never a fault. The events of a start go in the order 0x0005,
 * 0x0001, 0x0002.
 *
 * Fails on the old code: the code went out as it was (bit 7 clear) with one detail byte and no
 * count (scratch-copy mutation run: the 0x80 mark and the second byte removed).
 */
void testBootloaderFault()
{
    bool sent = true;
    for (uint8_t location : {uint8_t{FAULT_AT_SLOT_REGION}, uint8_t{FAULT_AT_IMAGE_RESULT_TEXT}}) {
        Rig rig;
        uint32_t left = 0xFFU;
        uint32_t left_later = 0xFFU;
        prepare(Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE);
        fakeBackupSet(BACKUP_PAIR_FAULT_LOCATION, location);
        fakeBackupSet(BACKUP_PAIR_FAULT_LATER, 3U);
        rig.updater.init(Slot::kA);
        rig.mm.start(Slot::kA, 0U, false);
        rig.mm.poll(0U);
        rig.mm.poll(1U);
        sent = sent && memoryEventsAre({{static_cast<uint8_t>(location | 0x80U), 3U}}) &&
               fakeBackupGet(BACKUP_PAIR_FAULT_LOCATION, left) && (left == 0U) &&
               fakeBackupGet(BACKUP_PAIR_FAULT_LATER, left_later) && (left_later == 0U) &&
               (fake.restarts == 0U) && (fake.counted_restarts == 0U);
    }
    report(sent, "3: bootloader left fault location 0x05 (slot_region) or 0x10 (common/, "
                 "image_result_text) and 3 later faults -> EVENT 0x0005 detail 85 03 or 90 03, "
                 "once over two passes; both cleared; no restart (was: the code as it is, "
                 "1 byte)");

    {
        Rig none;
        prepare(Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE);
        fakeBackupSet(BACKUP_PAIR_FAULT_LOCATION, FAULT_AT_SLOT_REGION);
        none.updater.init(Slot::kA);
        none.mm.start(Slot::kA, 0U, false);
        none.mm.poll(0U);
        bool zero = memoryEventsAre({{0x85U, 0U}});
        Rig big;
        prepare(Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE);
        fakeBackupSet(BACKUP_PAIR_FAULT_LOCATION, FAULT_AT_SLOT_REGION);
        fakeBackupSet(BACKUP_PAIR_FAULT_LATER, 300U);
        big.updater.init(Slot::kA);
        big.mm.start(Slot::kA, 0U, false);
        big.mm.poll(0U);
        bool cut = memoryEventsAre({{0x85U, 0xFFU}});
        report(zero && cut,
               "3: no count stored -> later faults 0 (85 00); a stored count of 300, which the "
               "bootloader cannot write, is cut to 255 (85 FF)");
    }

    {
        /* The firmware's own fault: the code as it is, bit 7 clear, no later faults. Every code
         * of the table has bit 7 clear, so the ground can tell the two apart. */
        bool own = true;
        for (uint8_t location : {uint8_t{FAULT_RANGE_FIRMWARE_FIRST},
                                 uint8_t{FAULT_AT_MODE_MANAGER_RUN_DANGEROUS},
                                 uint8_t{FAULT_RANGE_FIRMWARE_LAST}}) {
            const auto detail = HealthMonitor::memoryChangedDetailOwn(location);
            own = own && (detail[0] == location) && ((detail[0] & FAULT_FOUND_BY_BOOTLOADER) == 0U) &&
                  (detail[1] == 0U);
        }
        const auto boot = HealthMonitor::memoryChangedDetailBootloader(FAULT_AT_SLOT_REGION, 9U);
        report(own && (boot[0] == 0x85U) && (boot[1] == 9U),
               "3: the firmware's own fault 0x20, 0x2F, 0x4F -> detail as it is, 0 later faults "
               "(bit 7 clear); a bootloader fault 0x05 with 9 later -> 85 09");
    }

    bool dropped = true;
    faultClear();
    for (uint32_t bad : {uint32_t{FAULT_RANGE_FIRMWARE_FIRST}, uint32_t{0x55U},
                         uint32_t{0xFFFF0001U}}) {
        Rig rig;
        uint32_t left = 0xFFU;
        prepare(Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE);
        fakeBackupSet(BACKUP_PAIR_FAULT_LOCATION, bad);
        rig.updater.init(Slot::kA);
        rig.mm.start(Slot::kA, 0U, false);
        rig.mm.poll(0U);
        dropped = dropped && (fake_link.sent_bytes == 0U) &&
                  fakeBackupGet(BACKUP_PAIR_FAULT_LOCATION, left) && (left == 0U);
    }
    report(dropped && (fault_calls == 0U),
           "3: stored fault location 0x20 (a firmware code), 0x55 or 0xFFFF0001 -> dropped and "
           "cleared, no event, no fault (stored data, not memory)");

    Rig rig;
    prepare(Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE);
    fakeBackupSet(BACKUP_PAIR_FAULT_LOCATION, FAULT_AT_SLOT_LETTER);
    fakeBackupSet(BACKUP_PAIR_FALLBACK, recordOf(0U, CHECK_SIGNATURE_A, 0U, BACKUP_COPY_A));
    rig.updater.init(Slot::kA);
    rig.mm.start(Slot::kA, 0U, true);
    rig.mm.poll(0U);
    report(sentIs(0U, {ev(EventCode::kMemoryChanged, FAULT_AT_SLOT_LETTER | 0x80U),
                       ev(EventCode::kFaultDetected, FAULT_KIND_CORE_CLOCK),
                       ev(EventCode::kFallbackPrevious, 0x02U)}),
           "start events in order: 0x0005 (bootloader fault 0x02), 0x0001 (fault kind 0x01), "
           "0x0002 (02 09)");
}

/**
 * @brief Decision B: the firmware clears the bootloader's restart count of its own slot after
 * kNormalRunMs (60 s bench value) of running, not before, and leaves another slot's count.
 *
 * Fails on the old code: nothing was ever cleared.
 */
void testRestartCountClear()
{
    constexpr uint32_t kStart = 1000U;
    uint32_t value = 0U;
    {
        Rig rig;
        prepare(Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE);
        fakeBackupSet(BACKUP_PAIR_RESTARTS, (BOOT_SLOT_A << BACKUP_RESTARTS_SLOT_SHIFT) | 2U);
        rig.updater.init(Slot::kA);
        rig.mm.start(Slot::kA, kStart, false);
        rig.mm.poll(kStart + kNormalRunMs - 1U);
        bool before = fakeBackupGet(BACKUP_PAIR_RESTARTS, value) &&
                      (value == ((BOOT_SLOT_A << BACKUP_RESTARTS_SLOT_SHIFT) | 2U));
        rig.mm.poll(kStart + kNormalRunMs);
        bool after = fakeBackupGet(BACKUP_PAIR_RESTARTS, value) && (value == 0U);
        report(before && after && (kNormalRunMs == 60000U),
               "B: restart count of slot A (2) kept at 59.999 s of running, cleared at 60 s "
               "(kNormalRunMs, bench value)");
    }
    {
        Rig rig;
        prepare(Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE);
        fakeBackupSet(BACKUP_PAIR_RESTARTS, (BOOT_SLOT_B << BACKUP_RESTARTS_SLOT_SHIFT) | 2U);
        rig.updater.init(Slot::kA);
        rig.mm.start(Slot::kA, kStart, false);
        rig.mm.poll(kStart + kNormalRunMs);
        report(fakeBackupGet(BACKUP_PAIR_RESTARTS, value) &&
                   (value == ((BOOT_SLOT_B << BACKUP_RESTARTS_SLOT_SHIFT) | 2U)),
               "B: a count of slot B is left as it is by firmware running from A");
    }
}

/* ---- Fault paths of the mode manager ------------------------------------------------------ */

using MmBytes = std::array<uint8_t, sizeof(ModeManager)>;

/**
 * @brief Copies the bytes of a mode manager.
 *
 * @param[in]     mm  the mode manager
 * @return its bytes
 */
MmBytes bytesOf(const ModeManager &mm)
{
    MmBytes b{};
    std::memcpy(b.data(), &mm, sizeof(ModeManager));
    return b;
}

/**
 * @brief Returns the one offset where two snapshots differ.
 *
 * Why: the mode and the running slot are private; a test finds the byte that holds one by
 * starting the same object twice with one input changed (layout-independent), then writes an
 * impossible value there, as a flipped bit in RAM would.
 *
 * @param[in]     x  first snapshot
 * @param[in]     y  second snapshot
 * @return the offset; sizeof(ModeManager) if they differ in no byte or in more than one
 */
size_t onlyDifference(const MmBytes &x, const MmBytes &y)
{
    size_t at = sizeof(ModeManager);
    uint32_t count = 0U;

    for (size_t i = 0U; i < sizeof(ModeManager); i++) {
        if (x[i] != y[i]) {
            at = i;
            count++;
        }
    }
    return (count == 1U) ? at : sizeof(ModeManager);
}

/**
 * @brief Writes one byte into a mode manager's memory, as a flipped bit in RAM would.
 *
 * @param[in,out] mm      the mode manager
 * @param[in]     offset  byte offset (onlyDifference)
 * @param[in]     value   the byte
 */
void corrupt(ModeManager &mm, size_t offset, uint8_t value)
{
    reinterpret_cast<unsigned char *>(&mm)[offset] = value;
}

/**
 * @brief The mode manager's fault paths: runDangerous with an identifier acceptDangerous never
 * passes; poll with a corrupted mode, and with FALLBACK, which this build never sets; a corrupted
 * running slot reaching stateCode and copyCode.
 *
 * Fails on the old code: runDangerous answered 0x06; poll treated any mode but TRIAL as OPERATE;
 * stateCode was a cast; copyCode did not exist.
 */
void testModeFaults()
{
    {
        Rig rig;
        volatile AnswerCode code = AnswerCode::kSuccess;
        bool faulted = false;
        startRig(rig, Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
        faultClear();
        FAULT_CATCH(faulted, code = rig.mm.runDangerous(0x55U, 0U));
        report(faulted && (code == AnswerCode::kSuccess) && (fault_calls == 1U) &&
                   (fault_last_location == FAULT_AT_MODE_MANAGER_RUN_DANGEROUS) &&
                   (fake.state_writes == 0U) && (fake_link.sent_bytes == 0U),
               "runDangerous(0x55): fault reaction, fault location 0x2F, nothing written or sent "
               "(was: 0x06)");
    }
    static Rig rig;   /* static: zero-filled storage, so padding bytes compare equal */
    startRig(rig, Slot::kB, BOOT_SLOT_A, BOOT_SLOT_B, 0U);
    MmBytes trial = bytesOf(rig.mm);
    startRig(rig, Slot::kB, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
    size_t mode_at = onlyDifference(trial, bytesOf(rig.mm));
    bool found = (mode_at < sizeof(ModeManager));
    bool all = found;
    for (uint8_t value : {uint8_t{0x07U}, static_cast<uint8_t>(HealthMode::kFallback)}) {
        bool faulted = false;
        startRig(rig, Slot::kB, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
        if (found) {
            corrupt(rig.mm, mode_at, value);
        }
        faultClear();
        FAULT_CATCH(faulted, rig.mm.poll(10U));
        all = all && faulted && (fault_calls == 1U) &&
              (fault_last_location == FAULT_AT_MODE_MANAGER_POLL) &&
              (fake_link.sent_bytes == 0U) && (fake.restarts == 0U);
    }
    report(all, "poll with the mode byte set to 7, and to FALLBACK (never set by this build): "
                "fault reaction, fault location 0x2E (one code for both sites), nothing sent "
                "(was: run as OPERATE)");

    startRig(rig, Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
    MmBytes run_a = bytesOf(rig.mm);
    startRig(rig, Slot::kB, BOOT_SLOT_B, BOOT_SLOT_NONE, 0U);
    size_t running_at = onlyDifference(run_a, bytesOf(rig.mm));
    found = (running_at < sizeof(ModeManager));
    bool state_fault = false;
    bool copy_fault = false;
    startRig(rig, Slot::kA, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
    if (found) {
        corrupt(rig.mm, running_at, 0x07U);
    }
    faultClear();
    FAULT_CATCH(state_fault, rig.mm.poll(kNormalRunMs));
    const char *state_where = fault_last_where;
    const uint8_t state_location = fault_last_location;
    startWithRecord(rig, Slot::kA, recordOf(0U, CHECK_SIGNATURE_A, 0U, BACKUP_COPY_A));
    if (found) {
        corrupt(rig.mm, running_at, 0x07U);
    }
    FAULT_CATCH(copy_fault, rig.mm.poll(1U));
    report(found && state_fault && copy_fault && (std::strcmp(state_where, "stateCode") == 0) &&
               (state_location == FAULT_AT_STATE_CODE) &&
               (std::strcmp(fault_last_where, "copyCode") == 0) &&
               (fault_last_location == FAULT_AT_COPY_CODE) && (fake_link.sent_bytes == 0U),
           "running slot byte set to 7: the restart-count clear faults in stateCode (0x22), the "
           "fall-back record in copyCode (0x2D); nothing sent");
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
    g_size_a = load(argv[1], "firmware-A.img", g_image_a);
    g_size_b = load(argv[1], "firmware-B.img", g_image_b);
    if ((g_size_a == 0U) || (g_size_b == 0U)) {
        report(false, "could not read firmware-A.img and firmware-B.img");
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
    testSwitchStateCheck();
    testSwitchImageCheck();
    testSwitchWriteFails();
    testStartEvents();
    testBootloaderFault();
    testRestartCountClear();
    testModeFaults();
    if (failures == 0U) {
        std::printf("mode manager unit tests: all %u passed\n", tests);
        return 0;
    }
    std::printf("mode manager unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
