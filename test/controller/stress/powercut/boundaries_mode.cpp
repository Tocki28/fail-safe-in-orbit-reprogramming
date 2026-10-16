/*
 * boundaries_mode.cpp — host stress test of the controller's time boundaries (stress audit,
 * Part A.4, controller half): the ARM expiry at kArmExpiryMs, the trial length at kTrialMs, both
 * at their exact edge and across the wrap of the 32-bit millisecond count, and the ARM's named
 * sequence number at 0 and 0xFFFFFFFF.
 *
 * What it is: a program that drives the real ArmState (firmware/command-checker/arm_state.cpp)
 * and the real ModeManager (firmware/mode-manager/mode_manager.cpp) with the unit tests' fakes
 * for the chip (test/controller/unit, used read-only). It prints a table for every boundary: the
 * time, the answer the code gave, and the answer the code's own comment asks for.
 *
 * Why: the edge is where an off-by-one hides. The requirement text does not say whether the edge
 * itself is inside or outside (REQ-030 "expire when the dangerous command does not follow
 * within", REQ-013 "not judged good within the trial time", Interfaces Sec. 5.2 "expires
 * after"), so the "expected" column is the code's own comment (arm_state.cpp, ArmState::check:
 * "kArmExpiryMs or more since the ARM arrived"; mode_manager.cpp, acceptDangerous: "from
 * kTrialMs after the start on, the trial is over"). A match with the comment is a PASS; the
 * question whether the comment is what Tom wants is printed as a QUESTION line.
 *
 * Built with the Mac's clang++, the board's warning set, the undefined-behaviour sanitizer and
 * libc++'s debug hardening (rule E5). Exit status 1 if a check failed.
 */
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

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
/* Body of the synthetic image put in the running slot: any length that fits; the self-test only
 * compares the body with the header's check value (as test_mode.cpp). */
constexpr uint32_t kBodyBytes = 600U;
/* Command identifiers (Interfaces Sec. 5.1, command table), written here from the document and
 * not taken from mode_manager.hpp, so a wrong value there shows. */
constexpr uint8_t kConfirm = 0x26U;
constexpr uint8_t kReturn = 0x21U;
constexpr uint8_t kSwitch = 0x20U;
/* The edges, from the constants under test, named once. */
constexpr uint32_t kArmMs = kArmExpiryMs;
constexpr uint32_t kTrial = kTrialMs;
/* 2^32 - 1: the largest millisecond count and the largest sequence number. */
constexpr uint32_t kMax32 = 0xFFFFFFFFU;
/* An ARM 10 s before the wrap (the brief: ARM at 0xFFFFFFFF - 10 000 ms). */
constexpr uint32_t kTenSeconds = 10000U;
constexpr uint32_t kReceivedCode = 0U;
/* Test image size: a pattern and its CRC-32 (test_image.hpp). */
constexpr uint32_t kTestImageSize = 100U;
constexpr uint32_t kChunkMax = 256U;

uint32_t g_failures;
uint32_t g_checks;

/**
 * @brief Counts and prints one check.
 *
 * @param[in]     ok    true if the check passed
 * @param[in]     name  what was checked
 */
void report(bool ok, const char *name)
{
    g_checks++;
    if (!ok) {
        g_failures++;
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

/**
 * @brief Returns the answer code as its byte, for the tables.
 *
 * @param[in]     code  the answer code
 * @return the code's value
 */
unsigned codeOf(AnswerCode code)
{
    return static_cast<unsigned>(static_cast<uint8_t>(code));
}

/* ---- The controller under test, as test_mode.cpp wires it ------------------------------- */

struct Rig {
    UartLink uart;
    FrameSender sender{uart};
    TestImage image;
    ModeManager mm{sender, image};
    ArmState arm;
    UpdateReceiver updater;
    uint32_t seq = 100U;   /* the next sequence number; no technical reason for 100 */
};

/**
 * @brief Puts a synthetic image in a slot: a header whose length and body check value describe a
 * body of kBodyBytes, and that body (as test_mode.cpp, putImage).
 *
 * @param[in]     slot  Slot::kA or Slot::kB
 */
void putImage(Slot slot)
{
    uint32_t start = (slot == Slot::kA) ? kSlotAStart : kSlotBStart;
    uint8_t *image = &fake.flash[start - kFlashStart];

    std::memset(image, 0, kHeader);
    for (uint32_t i = 0U; i < kBodyBytes; i++) {
        image[kHeader + i] = static_cast<uint8_t>((i * 13U) + 5U);
    }
    putU32(&image[IMAGE_OFFSET_LENGTH], kBodyBytes);
    putU32(&image[IMAGE_OFFSET_BODY_CHECK], crc32_compute(&image[kHeader], kBodyBytes));
}

/**
 * @brief Sets the fakes and starts the controller in slot B on trial of B (confirmed A).
 *
 * @param[in,out] rig  the controller
 * @param[in]     now  milliseconds since start at which the trial starts
 */
void startTrial(Rig &rig, uint32_t now)
{
    fakeReset();
    fakeLinkReset();
    fake.state.confirmed = BOOT_SLOT_A;
    fake.state.trial = BOOT_SLOT_B;
    fake.state.failed_starts = 1U;
    putImage(Slot::kB);
    rig.updater.init(Slot::kB);
    rig.mm.start(Slot::kB, now, false);
}

/**
 * @brief Sends one UPLOAD command as main.cpp's routeUpload does and returns true if DONE 0x00.
 *
 * @param[in,out] rig      the controller
 * @param[in]     payload  command payload
 * @param[in]     length   payload bytes
 * @return true if the command was accepted and ran
 */
bool upload(Rig &rig, const uint8_t *payload, uint16_t length)
{
    uint32_t seq = rig.seq++;
    UpdateCommand cmd{};
    bool test = UpdateReceiver::parse(payload, length, cmd) && TestImage::isTestImage(cmd);
    AnswerCode code = test ? rig.mm.acceptTestImage(cmd) : rig.updater.accept(payload, length, cmd);

    if (code != AnswerCode::kSuccess) {
        rig.sender.sendAnswer(seq, AnswerKind::kRefused, code);
        return false;
    }
    rig.sender.sendAnswer(seq, AnswerKind::kReceived, static_cast<AnswerCode>(kReceivedCode));
    code = test ? rig.mm.runTestImage(cmd) : rig.updater.run(cmd);
    rig.sender.sendAnswer(seq,
                          (code == AnswerCode::kSuccess) ? AnswerKind::kDone : AnswerKind::kRefused,
                          code);
    return code == AnswerCode::kSuccess;
}

/**
 * @brief Sends a good test image (BEGIN, CHUNKs, END), the way the operator's console does, so
 * the trial's data-link test passes (mode_manager.hpp; test_image.hpp).
 *
 * @param[in,out] rig  the controller
 * @return true if every command was DONE
 */
bool sendGoodTestImage(Rig &rig)
{
    std::array<uint8_t, kTestImageMaxBytes> image{};
    std::array<uint8_t, 7U + kChunkMax> chunk{};
    std::array<uint8_t, 7> begin{};
    std::array<uint8_t, 3> end{};
    uint32_t covered = kTestImageSize - kTestImageCheckBytes;

    for (uint32_t i = 0U; i < covered; i++) {
        image[i] = static_cast<uint8_t>((i * 31U) + 7U);
    }
    putU32(&image[covered], crc32_compute(image.data(), covered));
    begin[0] = static_cast<uint8_t>(UploadId::kBegin);
    begin[1] = static_cast<uint8_t>(kTestImageNumber);
    begin[2] = static_cast<uint8_t>(kTestImageNumber >> 8);
    putU32(&begin[3], kTestImageSize);
    bool ok = upload(rig, begin.data(), 7U);
    chunk[0] = static_cast<uint8_t>(UploadId::kChunk);
    chunk[1] = begin[1];
    chunk[2] = begin[2];
    putU32(&chunk[3], 0U);
    std::memcpy(&chunk[7], image.data(), kTestImageSize);
    ok = ok && upload(rig, chunk.data(), static_cast<uint16_t>(7U + kTestImageSize));
    end[0] = static_cast<uint8_t>(UploadId::kEnd);
    end[1] = begin[1];
    end[2] = begin[2];
    return ok && upload(rig, end.data(), 3U);
}

/**
 * @brief A trial in slot B, started at `start`, whose three trial tests have passed.
 *
 * @param[in,out] rig    the controller
 * @param[in]     start  milliseconds since start at which the trial starts
 * @return true if the tests passed
 */
bool passedTrial(Rig &rig, uint32_t start)
{
    startTrial(rig, start);
    rig.mm.poll(start);
    bool ok = sendGoodTestImage(rig);

    rig.mm.poll(start + 1U);
    return ok && rig.mm.trialTestsPassed();
}

/**
 * @brief ARM naming the next frame at arm_now, as main.cpp's handleArm stores it.
 *
 * @param[in,out] rig      the controller
 * @param[in]     command  identifier named
 * @param[in]     named    sequence number named
 * @param[in]     arm_now  arrival time of the ARM, milliseconds since start
 */
void sendArm(Rig &rig, uint8_t command, uint32_t named, uint32_t arm_now)
{
    std::array<uint8_t, kArmPayloadBytes> p{};
    ArmRequest request{};

    rig.seq++;
    p[0] = kCmdArm;
    p[1] = command;
    putU32(&p[2], named);
    if (ArmState::parse(p.data(), kArmPayloadBytes, request)) {
        rig.arm.arm(request, arm_now);
    }
}

/**
 * @brief The arrival check of a dangerous command (ModeManager::acceptDangerous), at a time.
 *
 * @param[in,out] rig      the controller
 * @param[in]     command  identifier
 * @param[in]     seq      the command frame's sequence number
 * @param[in]     now      arrival time, milliseconds since start
 * @return the code acceptDangerous answered
 */
AnswerCode accept(Rig &rig, uint8_t command, uint32_t seq, uint32_t now)
{
    const std::array<uint8_t, 1> p = {command};

    return rig.mm.acceptDangerous(p.data(), 1U, seq, now, rig.arm);
}

/* ---- ArmState at its edge ----------------------------------------------------------------- */

/**
 * @brief Prints the ArmState table for one ARM arrival time and checks it against the code's
 * comment: expired from kArmExpiryMs on, valid before.
 *
 * Each distance is tried twice: with expireWhenDue() called first (as the main loop does on every
 * pass, main.cpp) and without.
 *
 * @param[in]     t0  arrival time of the ARM, milliseconds since start
 */
void armEdgeAt(uint32_t t0)
{
    const uint32_t distances[] = {0U, 1U, kArmMs - kTenSeconds, kArmMs - 1U, kArmMs, kArmMs + 1U,
                                  kArmMs + kTenSeconds};
    bool allOk = true;

    std::printf("  ARM at %10u ms (0x%08X):", t0, t0);
    for (uint32_t d : distances) {
        for (uint32_t pass = 0U; pass < 2U; pass++) {
            ArmState arm;
            ArmRequest request{kConfirm, 77U};
            uint32_t now = t0 + d;

            arm.arm(request, t0);
            if (pass == 1U) {
                arm.expireWhenDue(now);
            }
            AnswerCode got = arm.check(kConfirm, 77U, now);
            AnswerCode want = (d >= kArmMs) ? AnswerCode::kArmExpired : AnswerCode::kSuccess;

            allOk = allOk && (got == want);
            if (pass == 1U) {
                std::printf(" +%u:0x%02X", d, codeOf(got));
            }
        }
    }
    std::printf("\n");
    report(allOk, "ArmState: valid up to 29 999 ms after the ARM, expired (0x02) from 30 000 ms, "
                  "with and without expireWhenDue, for this ARM time");
}

/**
 * @brief The ARM expiry at its edge, without a wrap and across the wrap of the millisecond count.
 */
void armEdge()
{
    std::printf("ARM expiry (kArmExpiryMs = %u ms); columns: distance in ms : answer "
                "(0x00 allowed, 0x02 expired)\n", kArmMs);
    armEdgeAt(1000U);
    armEdgeAt(kMax32 - kTenSeconds);
    armEdgeAt(kMax32);
    armEdgeAt(0U);
    armEdgeAt(kMax32 - kArmMs);
    armEdgeAt(kMax32 - kArmMs + 1U);
}

/**
 * @brief The brief's two wrap cases by name: ARM at 0xFFFFFFFF - 10 000 ms, the command 20 000 ms
 * later (allowed), and 40 000 ms later (expired).
 */
void armWrapNamed()
{
    ArmState a;
    ArmState b;
    uint32_t t0 = kMax32 - kTenSeconds;
    ArmRequest request{kConfirm, 5U};

    a.arm(request, t0);
    b.arm(request, t0);
    uint32_t after20 = t0 + 20000U;
    uint32_t after40 = t0 + 40000U;

    b.expireWhenDue(t0 + kArmMs);
    std::printf("  ARM at 0x%08X; +20 000 ms is now = %u (wrapped), +40 000 ms is now = %u\n", t0,
                after20, after40);
    report((after20 < t0) && (a.check(kConfirm, 5U, after20) == AnswerCode::kSuccess),
           "ArmState: ARM at 0xFFFFFFFF - 10 000 ms, command 20 000 ms later (across the wrap): "
           "allowed");
    report((after40 < t0) && (a.check(kConfirm, 5U, after40) == AnswerCode::kArmExpired) &&
               (b.check(kConfirm, 5U, after40) == AnswerCode::kArmExpired),
           "ArmState: the same ARM, command 40 000 ms later: expired 0x02");
}

/**
 * @brief What the expired mark is for: without expireWhenDue(), an ARM looks fresh again one full
 * wrap (2^32 ms = 49.7 days) after its arrival; with the mark it stays expired.
 *
 * Observation only. arm_state.cpp (check) says the mark exists for this case. main.cpp calls
 * expireWhenDue() on every main-loop pass, and a pass is shorter than the watchdog's 1.88 s
 * (watchdog.hpp), so the unmarked case needs the main loop to stop for 30 s without a restart.
 */
void armStaleAcrossFullWrap()
{
    ArmState marked;
    ArmState unmarked;
    ArmRequest request{kConfirm, 9U};
    uint32_t t0 = 4000U;
    uint32_t again = t0 + 5U;   /* the same value, 2^32 ms + 5 ms after the ARM */

    marked.arm(request, t0);
    unmarked.arm(request, t0);
    marked.expireWhenDue(t0 + kArmMs);
    AnswerCode m = marked.check(kConfirm, 9U, again);
    AnswerCode u = unmarked.check(kConfirm, 9U, again);

    std::printf("  ARM at %u ms, command at the same count one full wrap later (+5 ms mod 2^32):"
                " marked by expireWhenDue -> 0x%02X, never marked -> 0x%02X\n", t0, codeOf(m),
                codeOf(u));
    report(m == AnswerCode::kArmExpired,
           "ArmState: an ARM marked expired stays expired after a full wrap of the count");
    std::printf("OBSERVATION  an ARM that expireWhenDue() never saw is allowed again one full wrap "
                "later; the main loop calls it every pass (main.cpp), so this needs the loop to "
                "stop for 30 s without a restart\n");
}

/**
 * @brief An ARM that names sequence number 0xFFFFFFFF or 0 allows exactly that number.
 *
 * Also parses the ARM payload with those numbers (little-endian, Interfaces Sec. 4.0).
 */
void armSequenceEdges()
{
    const uint32_t named[] = {kMax32, 0U, 1U, kMax32 - 1U};
    bool allOk = true;

    for (uint32_t n : named) {
        std::array<uint8_t, kArmPayloadBytes> p{};
        ArmRequest request{};
        ArmState arm;

        p[0] = kCmdArm;
        p[1] = kConfirm;
        putU32(&p[2], n);
        allOk = allOk && ArmState::parse(p.data(), kArmPayloadBytes, request) &&
                (request.sequence == n) && (request.command == kConfirm);
        arm.arm(request, 0U);
        for (uint32_t probe : named) {
            AnswerCode got = arm.check(kConfirm, probe, 1U);
            AnswerCode want = (probe == n) ? AnswerCode::kSuccess : AnswerCode::kNotArmed;

            allOk = allOk && (got == want);
        }
        allOk = allOk && (arm.check(kSwitch, n, 1U) == AnswerCode::kNotArmed);
        arm.disarm();
        allOk = allOk && (arm.check(kConfirm, n, 1U) == AnswerCode::kNotArmed);
    }
    report(allOk, "ArmState: an ARM naming sequence 0xFFFFFFFF, 0, 1 or 0xFFFFFFFE allows that "
                  "number and no other; another command identifier is 0x01; used up is 0x01");
    /* The cleared fields of a disarmed state are command 0 and sequence 0: an ARM naming exactly
     * those must still be a real ARM, and a disarmed state must not match them. */
    ArmState zero;
    ArmRequest zeros{0U, 0U};

    bool disarmedOk = (zero.check(0U, 0U, 0U) == AnswerCode::kNotArmed);

    zero.arm(zeros, 0U);
    bool armedOk = (zero.check(0U, 0U, 1U) == AnswerCode::kSuccess);

    zero.disarm();
    report(disarmedOk && armedOk && (zero.check(0U, 0U, 1U) == AnswerCode::kNotArmed),
           "ArmState: a disarmed state (fields 0, 0) is not armed for command 0 sequence 0; an "
           "ARM naming 0, 0 is armed; after use it is not");
}

/**
 * @brief The same edges through ModeManager::acceptDangerous, with RETURN TO PREVIOUS in TRIAL:
 * the ARM's expiry at the edge, across the wrap, and the sequence numbers 0 and 0xFFFFFFFF.
 */
void armThroughModeManager()
{
    const uint32_t t0s[] = {50U, kMax32 - kTenSeconds};
    const uint32_t seqs[] = {0U, kMax32, 4321U};
    bool allOk = true;

    for (uint32_t t0 : t0s) {
        for (uint32_t s : seqs) {
            for (uint32_t d : {kArmMs - 1U, kArmMs, kArmMs + 1U}) {
                Rig rig;

                startTrial(rig, 0U);
                sendArm(rig, kReturn, s, t0);
                AnswerCode got = accept(rig, kReturn, s, t0 + d);
                AnswerCode want = (d >= kArmMs) ? AnswerCode::kArmExpired : AnswerCode::kSuccess;

                allOk = allOk && (got == want);
            }
        }
    }
    report(allOk, "acceptDangerous (RETURN TO PREVIOUS in TRIAL): ARM at 50 ms and at 0xFFFFFFFF - "
                  "10 000 ms, sequence 0, 0xFFFFFFFF and 4321: accepted at +29 999 ms, 0x02 at "
                  "+30 000 and +30 001 ms");
}

/* ---- The trial length at its edge ----------------------------------------------------------- */

/**
 * @brief The last moment CONFIRM is accepted and the first it is refused, for a trial that starts
 * at `start`.
 *
 * A fresh controller each time. The ARM arrives 10 ms before the CONFIRM, so the ARM's own expiry
 * is never the reason for a refusal. Expected by the code's comment (mode_manager.cpp,
 * acceptDangerous): accepted up to kTrialMs - 1 ms after the start, refused 0x05 from kTrialMs.
 * The refused CONFIRM must not use the ARM up and must not write the boot state.
 *
 * @param[in]     start  milliseconds since start at which the trial starts
 */
void confirmEdgeAt(uint32_t start)
{
    bool allOk = true;
    unsigned codes[3] = {0U, 0U, 0U};
    const uint32_t offsets[3] = {kTrial - 1U, kTrial, kTrial + 1U};

    for (uint32_t i = 0U; i < 3U; i++) {
        Rig rig;

        allOk = allOk && passedTrial(rig, start);
        uint32_t now = start + offsets[i];
        uint32_t writes = fake.state_writes;
        uint32_t cmdSeq = rig.seq + 1U;

        sendArm(rig, kConfirm, cmdSeq, now - 10U);
        AnswerCode got = accept(rig, kConfirm, cmdSeq, now);

        codes[i] = codeOf(got);
        if (i == 0U) {
            allOk = allOk && (got == AnswerCode::kSuccess);
        } else {
            allOk = allOk && (got == AnswerCode::kMode) && (fake.state_writes == writes) &&
                    (rig.arm.check(kConfirm, cmdSeq, now) == AnswerCode::kSuccess);
        }
    }
    std::printf("  trial starts at %10u ms (0x%08X), ends at 0x%08X: CONFIRM at +%u -> 0x%02X, "
                "+%u -> 0x%02X, +%u -> 0x%02X\n", start, start, start + kTrial, offsets[0],
                codes[0], offsets[1], codes[1], offsets[2], codes[2]);
    report(allOk, "CONFIRM: accepted at 119 999 ms after the trial start, refused 0x05 at 120 000 "
                  "and 120 001 ms (ARM kept, boot state untouched), for this start time");
}

/**
 * @brief What poll() does at the edge: nothing at kTrialMs - 1, the give-up at kTrialMs.
 *
 * Also trialSecondsLeft() at the edge (HEALTH's trial seconds): 1 at the last millisecond, 0 from
 * kTrialMs. The give-up is the boot state confirmed A, trial none, FALLBACK flag, and one restart
 * (mode_manager.cpp, giveUp).
 *
 * @param[in]     start  milliseconds since start at which the trial starts
 */
void pollEdgeAt(uint32_t start)
{
    Rig rig;
    bool ok = passedTrial(rig, start);
    uint16_t last = rig.mm.trialSecondsLeft(start + kTrial - 1U);
    uint16_t over = rig.mm.trialSecondsLeft(start + kTrial);

    rig.mm.poll(start + kTrial - 1U);
    bool quiet = (fake.restarts == 0U) && (fake.state_writes == 0U);
    rig.mm.poll(start + kTrial);
    bool gaveUp = (fake.restarts == 1U) && (fake.state_writes == 1U) &&
                  (fake.state.confirmed == BOOT_SLOT_A) && (fake.state.trial == BOOT_SLOT_NONE) &&
                  ((fake.state.flags & BOOT_STATE_FLAG_FALLBACK) != 0U);

    std::printf("  trial starts at 0x%08X: poll at +119 999 ms quiet %d, at +120 000 ms gave up "
                "%d; trialSecondsLeft %u then %u\n", start, static_cast<int>(quiet),
                static_cast<int>(gaveUp), static_cast<unsigned>(last),
                static_cast<unsigned>(over));
    report(ok && quiet && gaveUp && (last == 1U) && (over == 0U),
           "trial end: poll() does nothing at 119 999 ms and gives the trial up at 120 000 ms "
           "(boot state confirmed A, FALLBACK, one restart); seconds left 1 then 0");
}

/**
 * @brief The trial edge for start times without a wrap and with the end before, on, and after
 * the wrap.
 */
void trialEdge()
{
    const uint32_t starts[] = {0U, 1000U, kMax32 - kTrial + 1U,       /* ends at 0 */
                               kMax32 - (kTrial / 2U),                /* ends after the wrap */
                               kMax32,                                /* starts on the wrap */
                               0U - kTrial - 1U};                     /* ends at 0xFFFFFFFF */

    std::printf("Trial length (kTrialMs = %u ms); CONFIRM answers (0x00 accepted, 0x05 refused)\n",
                kTrial);
    for (uint32_t s : starts) {
        confirmEdgeAt(s);
    }
    std::printf("Trial end seen by poll()\n");
    for (uint32_t s : starts) {
        pollEdgeAt(s);
    }
}

}  // namespace

/**
 * @brief Runs the boundary checks and prints the questions for the owner.
 *
 * @return 0 if every check passed, 1 otherwise
 */
int main()
{
    armEdge();
    armWrapNamed();
    armStaleAcrossFullWrap();
    armSequenceEdges();
    armThroughModeManager();
    trialEdge();
    std::printf("QUESTION ARM-EDGE  at exactly kArmExpiryMs (30 000 ms) after the ARM the code "
                "answers 0x02 (>=). REQ-030 says \"expire when the dangerous command does not "
                "follow within TBD-arm\" and Interfaces Sec. 5.2 (Arming) says \"expires after "
                "TBD-arm\"; neither says whether the edge itself is inside. Expected behaviour "
                "not specified. Code comment: arm_state.cpp, ArmState::check.\n");
    std::printf("QUESTION TRIAL-EDGE  at exactly kTrialMs (120 000 ms) after the start CONFIRM is "
                "refused 0x05 and poll() gives the trial up (>=). REQ-013 says \"not judged good "
                "within the trial time\"; REQ-011 says \"a trial of set length TBD-trial\"; "
                "neither says whether the edge itself is inside. Expected behaviour not "
                "specified. Code comment: mode_manager.cpp, acceptDangerous.\n");
    std::printf("%s  %u checks, %u failed\n", (g_failures == 0U) ? "PASS" : "FAIL", g_checks,
                g_failures);
    return (g_failures == 0U) ? 0 : 1;
}
