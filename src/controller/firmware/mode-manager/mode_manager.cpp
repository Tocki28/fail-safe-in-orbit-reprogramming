/*
 * mode_manager.cpp — see mode_manager.hpp.
 *
 * Open items left here (each marked where it applies):
 *   - CONFIRM and SWITCH whose boot-state write fails are refused 0x06: the Interfaces document
 *     has no refusal code for a failed boot-state write.
 *   - A give-up whose boot-state write fails sends no EVENT 0x0008 and restarts anyway.
 *   - In OPERATE a failed self-test is reported in HEALTH only (last self-test 0x01); EVENT
 *     0x0004 and the response to it (REQ-038, REQ-041) come with the health monitor (step 2e).
 */
#include "mode_manager.hpp"

#include <array>

#include "crc32.h"
#include "image_check.h"
#include "image_format.h"
#include "layout.h"
#include "mode_port.hpp"

namespace {

/* layout.h's values have no U suffix (the linker script reads the same file); typed once. */
constexpr uint32_t kHeaderBytes = IMAGE_HEADER_BYTES;
/* The longest body a slot can hold: the slot less the header and the two signatures
 * (Interfaces Sec. 5.5, Figure 5). */
constexpr uint32_t kBodyMaxBytes = SLOT_BYTES - IMAGE_HEADER_BYTES - IMAGE_SIGNATURES_BYTES;
constexpr uint32_t kMsPerSecond = 1000U;

static_assert(kEventReportMaxBytes <= kSendPayloadMax, "an EVENT report must fit FrameSender");

/**
 * @brief Returns the slot code the boot state stores for a slot (common/boot_state.h).
 *
 * The Slot enumerators carry the boot state's codes (boot_slot.hpp), so this is the stored byte.
 *
 * @param[in]     slot  Slot::kNone, kA or kB
 * @return BOOT_SLOT_NONE, BOOT_SLOT_A or BOOT_SLOT_B
 */
uint8_t stateCode(Slot slot)
{
    return static_cast<uint8_t>(slot);
}

/**
 * @brief Returns the other slot of the pair: B for A, A for B.
 *
 * @param[in]     slot  Slot::kA or Slot::kB
 * @return Slot::kB for Slot::kA; Slot::kA for any other value
 */
Slot otherSlot(Slot slot)
{
    return (slot == Slot::kA) ? Slot::kB : Slot::kA;
}

}  // namespace

/**
 * @brief Decides the mode from the boot state and runs the self-test; called once, at start,
 * after the link is up.
 *
 * TRIAL if the running slot is the boot state's trial slot, otherwise OPERATE (decided design,
 * step 2d). A boot state that cannot be read gives OPERATE: no trial is known. FALLBACK is a
 * later step. The trial time starts now: the firmware's start, not the bootloader's.
 *
 * The self-test runs at every start, in every mode (REQ-039); HEALTH reports its result.
 *
 * @param[in]     running  the slot this firmware runs from (UpdateReceiver::slotOf)
 * @param[in]     now_ms   milliseconds since start
 */
void ModeManager::start(Slot running, uint32_t now_ms)
{
    boot_state_t state{};
    bool valid = ModePort::readBootState(state);
    bool in_slot = (running == Slot::kA) || (running == Slot::kB);

    running_ = running;
    trial_start_ms_ = now_ms;
    trial_started_sent_ = false;
    tests_passed_sent_ = false;
    restart_due_ = false;
    mode_ = (valid && in_slot && (toSlot(state.trial) == running)) ? HealthMode::kTrial
                                                                   : HealthMode::kOperate;
    self_test_passed_ = selfTest(running);
}

/**
 * @brief The self-test (REQ-039, trial test a of REQ-139): recomputes the CRC-32 of the
 * running image's body and compares it with the body check value in the image's own header.
 *
 * Proposal (TBR), awaiting the owner's acceptance. Header layout: common/image_format.h
 * (Interfaces Sec. 5.5, Figure 6); the same CRC-32 the bootloader checks before it starts the
 * image (common/image_check.c). The body length comes from the header, so it is bounded by the
 * slot before any body byte is read: a damaged length cannot make the check read past the slot.
 * A flash read error (two wrong bits in one word, common/flash.h) also fails the test.
 * Not checked here: the signatures (the bootloader checked them before it started this image),
 * RAM, and the CPU.
 *
 * @param[in]     running  the slot this firmware runs from
 * @return true if the body matches its check value and no flash read error was seen; false
 *         otherwise, and for a running slot that is not A or B
 */
bool ModeManager::selfTest(Slot running)
{
    uint32_t start = UpdateReceiver::slotStart(running);

    if (start == 0U) {
        return false;
    }
    const uint8_t *image = ModePort::flash(start);
    ModePort::eccClear();
    uint32_t body = image_read_u32(&image[IMAGE_OFFSET_LENGTH]);
    if (body > kBodyMaxBytes) {
        return false;
    }
    uint32_t stored = image_read_u32(&image[IMAGE_OFFSET_BODY_CHECK]);
    bool same = (crc32_compute(&image[kHeaderBytes], body) == stored);
    return same && !ModePort::eccErrorSeen();
}

/**
 * @brief Returns true if all three trial tests have passed (REQ-139): self-test, data link,
 * test image.
 *
 * Data link: at least one answer went out whole (UartLink::send returned true) and no send
 * since start returned false. "Since start" is "during this trial": a trial starts with the
 * firmware.
 *
 * @return true if all three passed, false otherwise
 */
bool ModeManager::trialTestsPassed() const
{
    bool link_passed = sender_.answerSent() && !sender_.sendFailed();
    return self_test_passed_ && link_passed && (test_image_.result() == TestImageResult::kPassed);
}

/**
 * @brief Returns the first failed trial test, in the order self-test, data link, test image.
 *
 * A failed test stays failed: a later pass of the same test does not undo it (the trial is
 * given up at once, REQ-140). Data link: any send that returned false fails it, also after
 * EVENT 0x000A; a stalled transmitter during the trial is a failure whenever it happens.
 *
 * @return the failed test, or TrialTest::kNone if none has failed
 */
TrialTest ModeManager::firstFailedTest() const
{
    if (!self_test_passed_) {
        return TrialTest::kSelfTest;
    }
    if (sender_.sendFailed()) {
        return TrialTest::kDataLink;
    }
    if (test_image_.result() == TestImageResult::kFailed) {
        return TrialTest::kTestImage;
    }
    return TrialTest::kNone;
}

/**
 * @brief Seconds of trial left, for HEALTH's "trial time left" (Interfaces Sec. 5.4).
 *
 * Rounded up, so it reads 0 only once the trial time is over: 120 at start, 1 in the last
 * second. The millisecond tick stands still while the flash is erased (update_receiver.hpp);
 * in a trial only the boot-state write erases (one page, up to 24.47 ms [DS12589 Rev 6,
 * Table 46, p. 107]), so the trial runs late by at most that.
 *
 * @param[in]     now_ms  milliseconds since start
 * @return seconds left, 1 to 120 in TRIAL; 0 outside a trial or once the time is over
 */
uint16_t ModeManager::trialSecondsLeft(uint32_t now_ms) const
{
    if (mode_ != HealthMode::kTrial) {
        return 0U;
    }
    /* Unsigned subtraction stays right when the millisecond count wraps. */
    uint32_t elapsed = now_ms - trial_start_ms_;
    if (elapsed >= kTrialMs) {
        return 0U;
    }
    return static_cast<uint16_t>((kTrialMs - elapsed + kMsPerSecond - 1U) / kMsPerSecond);
}

/**
 * @brief Sends one EVENT report (Interfaces Sec. 5.4) as soon as the event happens (REQ-037).
 *
 * @param[in]     code          the event code
 * @param[in]     detail        detail bytes; nullptr when detail_bytes is 0
 * @param[in]     detail_bytes  0 or 1 in this step; at most 32
 * @param[in]     now_ms        milliseconds since start (the report's controller clock time)
 */
void ModeManager::sendEvent(EventCode code, const uint8_t *detail, uint32_t detail_bytes,
                            uint32_t now_ms)
{
    std::array<uint8_t, kEventReportMaxBytes> payload{};
    uint16_t length = HealthMonitor::eventReport(now_ms, code, detail, detail_bytes, payload);

    /* 0 only for a detail above 32 bytes; every call here passes 0 or 1 byte. */
    if (length == 0U) {
        return;
    }
    sender_.send(FrameType::kReport, payload.data(), length);
}

/**
 * @brief Waits, bounded, until the UART has sent everything, then restarts the chip.
 *
 * The wait lets the last answer or event leave the chip before the reset. Its result changes
 * nothing: the restart follows either way, and a frame cut short by it is dropped by the
 * onboard computer on its check value (Interfaces Sec. 4.0).
 *
 * @note Does not return on the board (ModePort::restart); the host fake returns.
 */
void ModeManager::restartWhenSent()
{
    (void)sender_.waitSent();   /* bounded (UartLink::waitSent); restart either way */
    ModePort::restart();
}

/**
 * @brief Gives the trial up (REQ-013, REQ-140): boot state trial = none, failed starts = 0,
 * EVENT 0x0008 "firmware given up", then the restart.
 *
 * The confirmed slot is the other one: in TRIAL the boot state names the running slot as the
 * trial, and a valid boot state's trial slot is always the one not confirmed
 * (common/boot_state.c, fields_valid). After the restart the bootloader starts the confirmed
 * slot, and never the given-up one again unless the operator orders it (bootloader/main.c,
 * REQ-019).
 *
 * Open: if the write fails, no EVENT 0x0008 is sent (the trial is not given up in the boot
 * state) and the chip restarts anyway. The bootloader counted this start before it ran
 * (bootloader/main.c, count_trial_start), so it starts the trial at most 3 times in all and
 * then gives it up itself (REQ-015). The Interfaces document has no event for "give-up could
 * not be stored".
 *
 * @param[in]     now_ms  milliseconds since start
 * @note Does not return on the board.
 */
void ModeManager::giveUp(uint32_t now_ms)
{
    boot_state_t state{};

    state.confirmed = stateCode(otherSlot(running_));
    state.trial = BOOT_SLOT_NONE;
    state.failed_starts = 0U;
    if (ModePort::writeBootState(state)) {
        sendEvent(EventCode::kGivenUp, nullptr, 0U, now_ms);
    }
    restartWhenSent();
}

/**
 * @brief The work of one main-loop pass: a restart after SWITCH, then in TRIAL the trial's
 * events, a failed test, and the time-out.
 *
 * Called after the frame of this pass was handled, so an answer always goes out before the
 * events and the restart it causes: UPLOAD END of a bad test image is answered REFUSED 0x08,
 * then EVENT 0x000B and 0x0008 follow on this pass.
 *
 * TRIAL, in order: EVENT 0x0006 once; a failed test: EVENT 0x000B with the test's number
 * (TrialTest), then the give-up (REQ-140); all tests passed: EVENT 0x000A once; trial time over
 * without CONFIRM: the give-up (REQ-013), with EVENT 0x0008 but no 0x000B.
 *
 * @param[in]     now_ms  milliseconds since start
 */
void ModeManager::poll(uint32_t now_ms)
{
    if (restart_due_) {
        restartWhenSent();
        return;
    }
    if (mode_ != HealthMode::kTrial) {
        return;
    }
    if (!trial_started_sent_) {
        trial_started_sent_ = true;
        sendEvent(EventCode::kTrialStarted, nullptr, 0U, now_ms);
    }
    TrialTest failed = firstFailedTest();
    if (failed != TrialTest::kNone) {
        const uint8_t detail = static_cast<uint8_t>(failed);
        sendEvent(EventCode::kTestsFailed, &detail, 1U, now_ms);
        giveUp(now_ms);
        return;
    }
    if (!tests_passed_sent_ && trialTestsPassed()) {
        tests_passed_sent_ = true;
        sendEvent(EventCode::kTestsPassed, nullptr, 0U, now_ms);
    }
    /* Unsigned subtraction stays right when the millisecond count wraps. */
    if ((now_ms - trial_start_ms_) >= kTrialMs) {
        giveUp(now_ms);
    }
}

/**
 * @brief Returns true if the payload's identifier is SWITCH (0x20) or CONFIRM (0x26).
 *
 * The size is checked by acceptDangerous(), so a SWITCH or CONFIRM of the wrong size is refused
 * 0x06 there.
 *
 * @param[in]     payload  command payload
 * @param[in]     length   payload bytes
 * @return true for SWITCH or CONFIRM, false otherwise or for an empty payload
 */
bool ModeManager::isDangerous(const uint8_t *payload, uint16_t length)
{
    return (length >= 1U) && ((payload[0] == kCmdSwitch) || (payload[0] == kCmdConfirm));
}

/**
 * @brief The checks on arrival of SWITCH or CONFIRM, in the order of the decided design; each
 * refusal is answered REFUSED only.
 *
 * 0. the command itself: exactly 1 byte (0x06, REQ-028);
 * 1. mode (REQ-027): SWITCH only in OPERATE, CONFIRM only in TRIAL (Interfaces Sec. 5.1, command
 *    table; FALLBACK is a later step); else 0x05;
 * 2. arming (REQ-029, REQ-030): ArmState::check, 0x01 or 0x02. A command that passes uses the
 *    ARM up, also if a later check refuses it;
 * 3. CONFIRM only: all trial tests passed (REQ-012, REQ-020); else 0x0F.
 * The mode check comes first, so a command refused for its mode leaves the ARM in place.
 * In step 2b the command checker takes over steps 0 to 2 for every command.
 *
 * @param[in]     payload   command payload
 * @param[in]     length    payload bytes
 * @param[in]     sequence  the frame's sequence number, which the ARM must name
 * @param[in]     now_ms    arrival time, milliseconds since start
 * @param[in,out] arm       the arm state; disarmed when the arming check passes
 * @return AnswerCode::kSuccess if the command may run; otherwise the refusal code
 */
AnswerCode ModeManager::acceptDangerous(const uint8_t *payload, uint16_t length,
                                        uint32_t sequence, uint32_t now_ms, ArmState &arm)
{
    if ((length != kDangerousBytes) || !isDangerous(payload, length)) {
        return AnswerCode::kImpossible;
    }
    uint8_t command = payload[0];
    HealthMode allowed = (command == kCmdSwitch) ? HealthMode::kOperate : HealthMode::kTrial;
    if (mode_ != allowed) {
        return AnswerCode::kMode;
    }
    AnswerCode armed = arm.check(command, sequence, now_ms);
    if (armed != AnswerCode::kSuccess) {
        return armed;
    }
    arm.disarm();
    if ((command == kCmdConfirm) && !trialTestsPassed()) {
        return AnswerCode::kTestsNotPassed;
    }
    return AnswerCode::kSuccess;
}

/**
 * @brief SWITCH, after RECEIVED (REQ-021): boot state confirmed = running slot, trial = the
 * other slot, failed starts = 0; the restart follows on the next pass (poll), after DONE.
 *
 * No other precondition, on purpose: the bootloader checks the image in the other slot at the
 * next start (origin, damage, target, slot; bootloader/main.c, image_passes) and gives the trial
 * up at once if it refuses the image, then starts the confirmed slot, this one. That check sees
 * the slot as it is at the start, after anything that happened since UPLOAD END, so
 * UpdateReceiver::imagePassed() is not asked: it would only repeat a check that is repeated
 * anyway, and it is forgotten at every restart.
 *
 * Open (Interfaces gap): no refusal code for a failed boot-state write. 0x06 is sent, and the
 * chip does not restart: nothing changed in the boot state.
 *
 * @return AnswerCode::kSuccess if the boot state was written; AnswerCode::kImpossible (0x06)
 *         if not, or if the running slot is not A or B
 */
AnswerCode ModeManager::runSwitch()
{
    boot_state_t state{};

    if ((running_ != Slot::kA) && (running_ != Slot::kB)) {
        return AnswerCode::kImpossible;
    }
    state.confirmed = stateCode(running_);
    state.trial = stateCode(otherSlot(running_));
    state.failed_starts = 0U;
    if (!ModePort::writeBootState(state)) {
        return AnswerCode::kImpossible;   /* Open (Interfaces gap): see above */
    }
    restart_due_ = true;
    return AnswerCode::kSuccess;
}

/**
 * @brief CONFIRM, after RECEIVED (REQ-012, REQ-020): boot state confirmed = running slot,
 * trial = none, failed starts = 0; on success EVENT 0x0007 "firmware confirmed" and mode
 * OPERATE (the trial timer stops), and the caller answers DONE 0x00.
 *
 * Open (Interfaces gap): no refusal code for a failed boot-state write. 0x06 is sent and the
 * mode stays TRIAL: the trial timer runs on, so the operator can ARM and CONFIRM again, and
 * without a CONFIRM the trial ends at its time limit (REQ-013).
 *
 * @param[in]     now_ms  milliseconds since start
 * @return AnswerCode::kSuccess if the boot state was written; AnswerCode::kImpossible (0x06)
 *         if not
 */
AnswerCode ModeManager::runConfirm(uint32_t now_ms)
{
    boot_state_t state{};

    state.confirmed = stateCode(running_);
    state.trial = BOOT_SLOT_NONE;
    state.failed_starts = 0U;
    if (!ModePort::writeBootState(state)) {
        /* Open (Interfaces gap): no refusal code for a failed boot-state write. */
        return AnswerCode::kImpossible;
    }
    /* The boot state now names this slot confirmed: the trial is over, whatever happens to the
     * event below. */
    mode_ = HealthMode::kOperate;
    sendEvent(EventCode::kConfirmed, nullptr, 0U, now_ms);
    return AnswerCode::kSuccess;
}

/**
 * @brief Runs SWITCH or CONFIRM after acceptDangerous() accepted it and RECEIVED was sent.
 *
 * @param[in]     command  kCmdSwitch or kCmdConfirm
 * @param[in]     now_ms   milliseconds since start
 * @return AnswerCode::kSuccess (answer DONE 0x00), or the refusal code (answer REFUSED)
 */
AnswerCode ModeManager::runDangerous(uint8_t command, uint32_t now_ms)
{
    if (command == kCmdSwitch) {
        return runSwitch();
    }
    if (command == kCmdConfirm) {
        return runConfirm(now_ms);
    }
    return AnswerCode::kImpossible;   /* not reached: acceptDangerous() takes only these two */
}

/**
 * @brief The checks on arrival of a test-image command (image number 0xFFFF): TRIAL only, then
 * TestImage::accept.
 *
 * Outside a trial no trial test runs, so a test image is an impossible value (0x06, decided
 * design). Every refusal is 0x06 and answered REFUSED only.
 *
 * @param[in]     cmd  the command, read by UpdateReceiver::parse
 * @return AnswerCode::kSuccess if accepted; AnswerCode::kImpossible (0x06) otherwise
 */
AnswerCode ModeManager::acceptTestImage(const UpdateCommand &cmd) const
{
    if (mode_ != HealthMode::kTrial) {
        return AnswerCode::kImpossible;
    }
    return test_image_.accept(cmd);
}

/**
 * @brief Runs a test-image command after RECEIVED (TestImage::run). The result of END is the
 * test's result; poll() acts on it on this same pass.
 *
 * @param[in]     cmd  the command acceptTestImage() accepted
 * @return AnswerCode::kSuccess; AnswerCode::kDamaged (0x08) at END on a wrong check value
 */
AnswerCode ModeManager::runTestImage(const UpdateCommand &cmd)
{
    return test_image_.run(cmd);
}
