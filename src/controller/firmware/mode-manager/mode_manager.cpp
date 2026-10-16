/*
 * mode_manager.cpp — see mode_manager.hpp.
 *
 * The owner's decisions of 13 Oct 2026 carried out here (letters of the controller audit):
 *   B  the bootloader's restart count of this slot is cleared after kNormalRunMs of running;
 *   C  a clock that fell back to 16 MHz is reported with EVENT 0x0001 at start;
 *   E  SWITCH reads the boot state again and runs only from the confirmed slot with no trial;
 *   F(a) SWITCH checks the other slot's image with UPLOAD END's checks before it writes;
 *   G  a give-up whose boot-state write fails leaves a give-up request for the bootloader;
 *   L  a failed self-test in OPERATE restarts the chip, counted by the bootloader;
 *   N  SWITCH and CONFIRM answer 0x11 when the boot-state write fails;
 *   O, P  the self-test design and the EVENT 0x000B detail codes are accepted as built;
 *   check-result reporting: the bootloader's fall-back record goes out as EVENT 0x0002 once.
 * And the owner's decisions of 13 Oct 2026 numbered 2 to 6:
 *   2  EVENT 0x0001 carries the fault kind, 0x01 for the clock fault (common/fault_codes.h);
 *   3  a fault location the bootloader left goes out once as EVENT 0x0005 with its code;
 *   4  one EVENT 0x0002 per copy the bootloader refused or skipped on this start;
 *   5  a give-up handed to the bootloader as a request still sends EVENT 0x0008 (giveUp);
 *   6  (in the update receiver) a slot written and passed by UPLOAD END gets a fresh count.
 * Decision 5 closes the open item that was recorded here: a give-up whose boot-state write failed
 * sent no EVENT 0x0008, even when the give-up request was stored.
 * And the owner's decision 23, 15 Oct 2026:
 *   23 the self-test runs every kSelfTestPeriodMs (60 s, bench value for TBD-selftest) while the
 *      firmware runs, in OPERATE and in TRIAL (runSelfTestWhenDue; REQ-039). Its failure uses the
 *      reactions that exist (pollOperate, pollTrial); the numbers are at kSelfTestPeriodMs.
 * And the owner's decision 30, 15 Oct 2026 (finding F-1: a SWITCH restarted a firmware that had
 * been given up; ConOps Scenario-310; REQ-019, REQ-117):
 *   30 FALLBACK: a give-up writes the FALLBACK flag and the given-up image's mark (giveUpState);
 *      start() sets FALLBACK from the flag; FALLBACK runs OPERATE's work (poll); SWITCH refuses
 *      0x13 on the mark; START FAILED and LEAVE FALLBACK are built; CONFIRM clears the flag.
 *
 * Switches over an enum have no default label. No default: the compiler checks every case
 * (-Wswitch, -Werror); only a corrupted value reaches the line after the switch, which calls the
 * fault reaction (firmware/fault.hpp). A deliberate deviation from MISRA C:2012 Rule 16.4, whose
 * purpose, handling a value outside the cases, the fault reaction meets.
 */
#include "mode_manager.hpp"

#include <array>

#include "backup_layout.h"
#include "crc32.h"
#include "fault.hpp"
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
/* EVENT 0x0002's detail: refused copy (1 byte), check result (1 byte) (Interfaces Sec. 5.4). */
constexpr uint32_t kFallbackDetailBytes = 2U;
/* The detail of EVENT 0x0001 (fault kind): 1 byte (decision 2, common/fault_codes.h). */
constexpr uint32_t kCodeDetailBytes = 1U;

/* The highest check result a refused or skipped copy can have: CHECK_LAST (0x10, "boot state
 * damaged"; Interfaces Sec. 5.4, "Check result" table, common/backup_layout.h). 0x00, "passed",
 * is no refusal. Before decision 4 the highest was 0x0A, "signature B"; before the owner's
 * decisions of 13 Oct 2026, second round, 0x0D, "restarted too often". */
constexpr uint8_t kCheckResultLast = CHECK_LAST;
/* The fault locations the bootloader can leave: its own (0x01 to 0x0F) and those of the shared
 * code in common/ it runs (0x10 to 0x1F) (common/fault_codes.h). */
constexpr uint32_t kBootloaderFaultFirst = FAULT_RANGE_BOOTLOADER_FIRST;
constexpr uint32_t kBootloaderFaultLast = FAULT_RANGE_COMMON_LAST;

static_assert(kEventReportMaxBytes <= kSendPayloadMax, "an EVENT report must fit FrameSender");

/* stateCode moved to boot_slot.hpp with decision 6 (the update receiver needs it too). */

/**
 * @brief Returns the code a slot has in the bootloader's fall-back record: the HEALTH report's
 * running-slot code (Interfaces Sec. 5.4; common/backup_layout.h).
 *
 * Slot::kNone gives 0, which no record holds, so a record never matches a firmware whose running
 * slot is not known. A value outside the enum is a corrupted variable: the fault reaction.
 *
 * @param[in]     slot  Slot::kNone, kA or kB
 * @return BACKUP_COPY_A, BACKUP_COPY_B, or 0 for Slot::kNone
 * @note Does not return for a value outside Slot.
 */
uint8_t copyCode(Slot slot)
{
    switch (slot) {
    case Slot::kNone:
        return 0U;
    case Slot::kA:
        return BACKUP_COPY_A;
    case Slot::kB:
        return BACKUP_COPY_B;
    }
    /* only a corrupted value reaches here */
    faultImpossibleValue(FAULT_AT_COPY_CODE, "copyCode");
}

/**
 * @brief Returns the body check value in the header of the image in a slot, or 0 when the header's
 * proprietary string is not there or its read saw a flash read error.
 *
 * The mark of a given-up image in the boot state (given_up_check; owner's decision 30, 15 Oct
 * 2026), and the value SWITCH compares with it. Only the header is read, at its fixed offsets
 * (common/image_format.h; Interfaces Sec. 5.5, Figure 6); no length is read, so no byte outside
 * the header is touched. 0 for a header without the proprietary string, as the decision says:
 * such a slot holds no image that could be started. A value read with a two-bit flash error is
 * not the header's value, so it is 0 too: Proposal (TBR), awaiting the owner's acceptance. The
 * bootloader reads the same field with its own code (bootloader/main.c, given_up_check_of;
 * REQ-119).
 *
 * @param[in]     slot  Slot::kA or Slot::kB; Slot::kNone gives 0 (no slot, so no image)
 * @return the header's body check value, or 0
 */
uint32_t headerBodyCheck(Slot slot)
{
    uint32_t start = UpdateReceiver::slotStart(slot);

    if (start == 0U) {
        return 0U;
    }
    const uint8_t *header = ModePort::flash(start);
    ModePort::eccClear();
    bool magic = (header[IMAGE_OFFSET_MAGIC] == IMAGE_MAGIC_0) &&
                 (header[IMAGE_OFFSET_MAGIC + 1U] == IMAGE_MAGIC_1) &&
                 (header[IMAGE_OFFSET_MAGIC + 2U] == IMAGE_MAGIC_2) &&
                 (header[IMAGE_OFFSET_MAGIC + 3U] == IMAGE_MAGIC_3);
    uint32_t check = image_read_u32(&header[IMAGE_OFFSET_BODY_CHECK]);
    if (!magic || ModePort::eccErrorSeen()) {
        return 0U;
    }
    return check;
}

/**
 * @brief Returns the boot state that a give-up of the trial running in `running` writes
 * (REQ-013, REQ-113, REQ-140; owner's decision 30, 15 Oct 2026).
 *
 * confirmed = the other slot, which is the previous one; trial = none; failed starts = 0; the
 * FALLBACK flag; and the mark, the running image's header body check value (headerBodyCheck), so
 * a later SWITCH to that image is refused 0x13 (REQ-019). One function for giveUp and
 * runReturnToPrevious, so the two give-ups write the same record. The bootloader writes the same
 * fields with its own code (bootloader/main.c, give_up_trial).
 *
 * @param[in]     running  the slot of the trial given up: Slot::kA or Slot::kB
 * @return the record to write; the write sets its sequence number and check value
 * @note Does not return for Slot::kNone (otherSlot's fault reaction).
 */
boot_state_t giveUpState(Slot running)
{
    boot_state_t state{};

    state.confirmed = stateCode(otherSlot(running));
    state.trial = BOOT_SLOT_NONE;
    state.failed_starts = 0U;
    state.flags = BOOT_STATE_FLAG_FALLBACK;
    state.given_up_check = headerBodyCheck(running);
    return state;
}

/**
 * @brief Returns true if START FAILED's "which copy" byte names the slot that is not running.
 *
 * The byte is input from the link (code rule A5), read only after the size check. Its codes are
 * the HEALTH report's running-slot codes (Interfaces Sec. 5.4; common/backup_layout.h): 0x01 slot
 * A, 0x02 slot B, 0x03 the last-resort firmware. Owner's decision 30, 15 Oct 2026: the running
 * slot, the last-resort firmware and every other value are refused 0x06. So the only copy that
 * passes is the other slot, and the work acts on otherSlot(running_), not on the byte
 * (ModeManager::runSwitch).
 *
 * @param[in]     copy     the byte as received
 * @param[in]     running  the slot this firmware runs from
 * @return true for slot A or slot B when it is not the running slot, false otherwise
 */
bool startFailedCopyOk(uint8_t copy, Slot running)
{
    bool a_or_b = (copy == BACKUP_COPY_A) || (copy == BACKUP_COPY_B);
    return a_or_b && (copy != copyCode(running));
}

}  // namespace

/**
 * @brief Decides the mode from the boot state and runs the self-test; called once, at start,
 * after the link is up.
 *
 * TRIAL if the running slot is the boot state's trial slot (decided design, step 2d). Otherwise
 * FALLBACK if the boot state has the FALLBACK flag and names the running slot as confirmed
 * (owner's decision 30, 15 Oct 2026): a give-up wrote the flag, and the bootloader started the
 * previous slot. Otherwise OPERATE. A boot state that cannot be read gives OPERATE: no trial and
 * no FALLBACK is known. The flag with a running slot that is not the confirmed one (the
 * last-resort firmware never runs this code; a slot started with no boot state) gives OPERATE:
 * the firmware that fell back is not this one. The trial time starts now: the firmware's start,
 * not the bootloader's. The events of the start (clock fault, fall-back) go out on the first
 * poll().
 *
 * The self-test runs at every start, in every mode (REQ-039); HEALTH reports its result.
 * This run is the first of the periodic self-tests (runSelfTestWhenDue): the next is due
 * kSelfTestPeriodMs after now_ms.
 *
 * @param[in]     running      the slot this firmware runs from (UpdateReceiver::slotOf)
 * @param[in]     now_ms       milliseconds since start
 * @param[in]     clock_fault  true if the core clock fell back to 16 MHz (CoreClock::start,
 *                             decision C)
 */
void ModeManager::start(Slot running, uint32_t now_ms, bool clock_fault)
{
    boot_state_t state{};
    bool valid = ModePort::readBootState(state);
    bool in_slot = (running == Slot::kA) || (running == Slot::kB);

    running_ = running;
    trial_start_ms_ = now_ms;
    trial_started_sent_ = false;
    tests_passed_sent_ = false;
    restart_due_ = false;
    clock_fault_ = clock_fault;
    start_events_sent_ = false;
    restart_count_cleared_ = false;
    bool fallback = (state.flags & BOOT_STATE_FLAG_FALLBACK) != 0U;
    if (valid && in_slot && (toSlot(state.trial) == running)) {
        mode_ = HealthMode::kTrial;
    } else if (valid && in_slot && fallback && (toSlot(state.confirmed) == running)) {
        mode_ = HealthMode::kFallback;
    } else {
        mode_ = HealthMode::kOperate;
    }
    self_test_passed_ = selfTest(running);
    last_self_test_ms_ = now_ms;
}

/**
 * @brief The self-test (REQ-039, trial test a of REQ-139): recomputes the CRC-32 of the
 * running image's body and compares it with the body check value in the image's own header.
 *
 * Accepted by the owner, 13 Oct 2026 (decision O). Header layout: common/image_format.h
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
 * Self-test: it is the last self-test (selfTestPassed), replaced every kSelfTestPeriodMs. A
 * failure is handled by pollTrial on the pass that found it, and the give-up restarts the chip,
 * so on the board a later passing run never undoes a failed one.
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
 * @param[in]     detail_bytes  0, 1 or 2 in this step; at most 32
 * @param[in]     now_ms        milliseconds since start (the report's controller clock time)
 */
void ModeManager::sendEvent(EventCode code, const uint8_t *detail, uint32_t detail_bytes,
                            uint32_t now_ms)
{
    std::array<uint8_t, kEventReportMaxBytes> payload{};
    uint16_t length = HealthMonitor::eventReport(now_ms, code, detail, detail_bytes, payload);

    /* 0 only for a detail above 32 bytes; every call here passes 0, 1 or 2 bytes. */
    if (length == 0U) {
        return;
    }
    sender_.send(FrameType::kReport, payload.data(), length);
}

/**
 * @brief Returns true if the bootloader's fall-back record describes this start: it names this
 * firmware's slot as the copy started, every copy's check result is 0x00 (not refused) or a code
 * of the table, this slot itself is not among the refused, and at least one copy was refused.
 *
 * The record is stored data read from the backup registers, so a value outside the codes fails
 * this check and is dropped whole, never a fault (common/backup_layout.h). The bootloader clears
 * the record at every start before it writes a new one, so a record for another start is a
 * record that went bad, not an old one.
 *
 * @param[in]     record  the record as read
 * @return true if every field is one of its codes and the started copy is this slot
 */
bool ModeManager::fallbackForThisStart(const FallbackRecord &record) const
{
    uint8_t started = copyCode(running_);
    bool any = false;

    if ((started == 0U) || (record.started != started)) {
        return false;
    }
    for (const FallbackEntry &entry : record.copies) {
        if (entry.check == CHECK_PASSED) {
            continue;
        }
        if ((entry.check > kCheckResultLast) || (entry.copy == started)) {
            return false;
        }
        any = true;
    }
    return any;
}

/**
 * @brief Sends EVENT 0x0005 once if the bootloader left a fault location (decision 3, owner,
 * 13 Oct 2026), then clears it.
 *
 * The bootloader has no link to the ground, so its fault reaction leaves the code in the backup
 * registers and resets (bootloader/fault.c); the first firmware that runs reports it. The detail
 * is two bytes: where, which is the code with FAULT_FOUND_BY_BOOTLOADER (0x80) added so the
 * ground can tell a fault the bootloader found from one the firmware found, and how many faults
 * the bootloader found after the first (at most 255). The code is stored data: one outside the
 * bootloader's and common/'s ranges (0x01 to 0x1F, common/fault_codes.h) is dropped, never a
 * fault. A count above 255 cannot be stored by the bootloader, so a larger value is stored data
 * that went bad and is cut to 255. Cleared whether it was sent or dropped; if the clear fails,
 * this start does not read it again (start_events_sent_), and the next start reports it once
 * more.
 *
 * @param[in]     now_ms  milliseconds since start
 */
void ModeManager::sendBootloaderFault(uint32_t now_ms)
{
    uint32_t location = 0U;
    uint32_t later = 0U;

    if (!ModePort::readBootloaderFault(location, later)) {
        return;
    }
    if ((location >= kBootloaderFaultFirst) && (location <= kBootloaderFaultLast)) {
        const uint32_t later_sent =
            (later < BACKUP_FAULT_LATER_MAX) ? later : BACKUP_FAULT_LATER_MAX;
        const std::array<uint8_t, kMemoryChangedDetailBytes> detail =
            HealthMonitor::memoryChangedDetailBootloader(static_cast<uint8_t>(location),
                                                         static_cast<uint8_t>(later_sent));
        sendEvent(EventCode::kMemoryChanged, detail.data(), kMemoryChangedDetailBytes, now_ms);
    }
    (void)ModePort::clearBootloaderFault();   /* see above */
}

/**
 * @brief Sends one EVENT 0x0002 per copy the bootloader refused or skipped on this start, then
 * clears the record (decision 4, owner, 13 Oct 2026; Interfaces Sec. 5.4).
 *
 * EVENT 0x0002 "fall-back to the previous copy", detail refused copy (1) and check result (1): a
 * main slot runs here, so a fall-back to it is a fall-back to the previous copy; 0x0003 is the
 * last-resort firmware's to send. This build runs only in slot A or B (firmware/Makefile), so it
 * never sends 0x0003. The events go in the order of the copies, A, B, last-resort, which is how
 * the record holds them (common/backup_layout.h). Before decision 4 one record, one event.
 *
 * @param[in]     now_ms  milliseconds since start
 */
void ModeManager::sendFallbackEvents(uint32_t now_ms)
{
    FallbackRecord record{};

    if (!ModePort::readFallbackRecord(record)) {
        return;
    }
    if (fallbackForThisStart(record)) {
        for (const FallbackEntry &entry : record.copies) {
            if (entry.check != CHECK_PASSED) {
                const std::array<uint8_t, kFallbackDetailBytes> detail = {entry.copy, entry.check};
                sendEvent(EventCode::kFallbackPrevious, detail.data(), kFallbackDetailBytes,
                          now_ms);
            }
        }
    }
    /* Cleared whether it was sent or dropped. If the clear fails, the bootloader clears the
     * record at the next start anyway (bootloader/main.c, clear_fallback_record), and this start
     * does not read it again (start_events_sent_). */
    (void)ModePort::clearFallbackRecord();
}

/**
 * @brief The events of a start, once, on the first poll(): EVENT 0x0005 with a fault location the
 * bootloader left (decision 3), EVENT 0x0001 after a clock fault (decision C), then one EVENT
 * 0x0002 per copy the bootloader refused or skipped (decision 4) (Interfaces Sec. 5.4).
 *
 * EVENT 0x0001 "fault detected": accepted, with its detail, by the owner's decision 2, 13 Oct 2026
 * ("the C decision's report"). Decision C left the choice of report to the builder. Why this one:
 * the HEALTH report has no field for the clock; 0x0001 is the event for a fault the controller
 * found and answered on its own (REQ-038), and running at 16 MHz is that answer. Not 0x0004: the
 * self-test is the image check of decision O, which the clock does not fail. The detail is the
 * fault kind FAULT_KIND_CORE_CLOCK, 0x01 (common/fault_codes.h). Before decision 2 it was a
 * Proposal (TBR) sent with no detail: the Interfaces table defined none for 0x0001, so the ground
 * could not tell a clock fault from another fault.
 * The order: the bootloader's fault happened before this start, so it goes first; the clock
 * event before the fall-back events as before (no technical reason).
 *
 * @param[in]     now_ms  milliseconds since start
 */
void ModeManager::sendStartEvents(uint32_t now_ms)
{
    if (start_events_sent_) {
        return;
    }
    start_events_sent_ = true;
    sendBootloaderFault(now_ms);
    if (clock_fault_) {
        const uint8_t kind = FAULT_KIND_CORE_CLOCK;
        sendEvent(EventCode::kFaultDetected, &kind, kCodeDetailBytes, now_ms);
    }
    sendFallbackEvents(now_ms);
}

/**
 * @brief Clears the bootloader's count of restarts of this slot once the firmware has run for
 * kNormalRunMs (decision B).
 *
 * "Run normally": the main loop has called poll() for kNormalRunMs since start, so the clock, the
 * tick and the loop work, and nothing restarted the chip in that time. One try: if the write
 * fails the backup domain cannot be written, and a retry on every pass would not change that.
 *
 * @param[in]     now_ms  milliseconds since start
 */
void ModeManager::clearRestartCountWhenDue(uint32_t now_ms)
{
    /* Unsigned subtraction stays right when the millisecond count wraps. */
    if (restart_count_cleared_ || ((now_ms - trial_start_ms_) < kNormalRunMs)) {
        return;
    }
    restart_count_cleared_ = true;
    /* If the clear fails (see above), the count stays: at worst this slot is skipped after fewer
     * new restarts, the safe direction of decision B. */
    (void)ModePort::clearRestartCount(stateCode(running_));
}

/**
 * @brief Runs the self-test again once kSelfTestPeriodMs have passed since the last run, and
 * stores the result for HEALTH (REQ-039; owner's decision 23, 15 Oct 2026).
 *
 * What it is: the periodic part of REQ-039, "at every start and at TBD-selftest". The run in
 * start() is the first; this is every later one. Why it is like this: a change to the image in
 * flash after the start would otherwise go unseen until the next start. The 60 s, the time one
 * run takes (at most 10 ms at 170 MHz, 106 ms at 16 MHz, computed, not measured) and the
 * comparison with the 1.88 s watchdog and the UART ring are at kSelfTestPeriodMs.
 *
 * At most one run per call, and so per main-loop pass; the clock for the next run is the now_ms
 * of the call that ran, so runs are never closer than kSelfTestPeriodMs. A pass that is late
 * (the loop was busy) gives one run, never a catch-up. A passing run changes only the stored
 * result and sends nothing. A failing run stores false; the reaction is not here but in the
 * mode's own work that follows in poll(): pollOperate (EVENT 0x0004, restart, decision L) or
 * pollTrial (EVENT 0x000B with detail 0x01, then the give-up, REQ-140). This function has no
 * wait and no loop of its own; selfTest() is bounded by the slot (see there).
 * Unsigned subtraction stays right when the millisecond count wraps (every 49.7 days).
 *
 * @param[in]     now_ms  milliseconds since start
 */
void ModeManager::runSelfTestWhenDue(uint32_t now_ms)
{
    if ((now_ms - last_self_test_ms_) < kSelfTestPeriodMs) {
        return;
    }
    last_self_test_ms_ = now_ms;
    self_test_passed_ = selfTest(running_);
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
 * @brief Gives the trial up (REQ-013, REQ-140): boot state trial = none, failed starts = 0, the
 * FALLBACK flag and the given-up image's mark (giveUpState), EVENT 0x0008 "firmware given up",
 * then the restart.
 *
 * The confirmed slot is the other one: in TRIAL the boot state names the running slot as the
 * trial, and a valid boot state's trial slot is always the one not confirmed
 * (common/boot_state.c, fields_valid). After the restart the bootloader starts the confirmed
 * slot, and never the given-up one again unless the operator orders it (bootloader/main.c,
 * REQ-019). TRIAL needs a running slot of A or B (start()), so otherSlot() always has an answer.
 * The flag makes the firmware that starts next run in FALLBACK; the mark makes it refuse a SWITCH
 * back to this image (0x13) until START FAILED orders it (owner's decision 30, 15 Oct 2026;
 * finding F-1). Before, the record had neither, and a SWITCH restarted the given-up image.
 *
 * If the write fails (decision G): the give-up request is left in the backup registers, and the
 * bootloader gives the trial up before it counts its next start (bootloader/main.c,
 * apply_give_up_request). Before, nothing was left, and the trial could start again until the
 * bootloader's start count ended it (REQ-015). If even the request cannot be stored, that start
 * count still ends it.
 * EVENT 0x0008 goes out when the give-up is stored either way: in the boot state, or as the
 * request the bootloader applies (decision 5, owner, 13 Oct 2026). Before, it went out only after
 * the boot-state write. When neither could be stored, no 0x0008: the trial is not given up yet;
 * the bootloader's start count ends it later (REQ-015).
 *
 * @param[in]     now_ms  milliseconds since start
 * @note Does not return on the board.
 */
void ModeManager::giveUp(uint32_t now_ms)
{
    boot_state_t state = giveUpState(running_);

    /* The request only when the write failed: nowhere else to keep the give-up then. The
     * bootloader's give-up of the request writes the flag and the mark too (give_up_trial). */
    bool stored = ModePort::writeBootState(state) ||
                  ModePort::leaveGiveUpRequest(stateCode(running_));
    if (stored) {
        sendEvent(EventCode::kGivenUp, nullptr, 0U, now_ms);
    }
    restartWhenSent();
}

/**
 * @brief OPERATE's work on one pass, and FALLBACK's: a failed self-test restarts the chip
 * (decision L).
 *
 * FALLBACK runs exactly this work (owner's decision 30, 15 Oct 2026; poll): the firmware it runs
 * is the confirmed one, so a failed self-test there has OPERATE's reaction. After the restart the
 * boot state still has the FALLBACK flag, so the firmware starts in FALLBACK again (REQ-117).
 *
 * A failed self-test means the running image's body no longer matches its check value
 * (selfTest). The restart makes the bootloader check the image again before it runs it: if the
 * flash is damaged it is refused and the bootloader falls back; if the image passes there and
 * fails here again, every restart is counted (decision B: the restart carries the fault mark) and
 * the third starts the last-resort firmware, so the loop is bounded. EVENT 0x0004 "self-test
 * failed" (Interfaces Sec. 5.4, REQ-039) goes out first, since the restart comes before the
 * first HEALTH report could carry the result. Before, the failure was only reported in HEALTH
 * and the firmware ran on.
 * The failed self-test is the last one run: the one at start, or a periodic one found by
 * runSelfTestWhenDue() earlier in the same poll() pass (owner's decision 23, 15 Oct 2026).
 *
 * @param[in]     now_ms  milliseconds since start
 * @note Does not return on the board when the self-test failed.
 */
void ModeManager::pollOperate(uint32_t now_ms)
{
    if (self_test_passed_) {
        return;
    }
    sendEvent(EventCode::kSelfTestFailed, nullptr, 0U, now_ms);
    (void)sender_.waitSent();   /* bounded (UartLink::waitSent); restart either way */
    ModePort::restartCounted(BACKUP_FAULT_SELF_TEST);
}

/**
 * @brief TRIAL's work on one pass: the trial's events, a failed test, and the time-out.
 *
 * In order: EVENT 0x0006 once; a failed test: EVENT 0x000B with the test's number (TrialTest),
 * then the give-up (REQ-140); all tests passed: EVENT 0x000A once; trial time over without
 * CONFIRM: the give-up (REQ-013), with EVENT 0x0008 but no 0x000B.
 *
 * @param[in]     now_ms  milliseconds since start
 */
void ModeManager::pollTrial(uint32_t now_ms)
{
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
 * @brief The work of one main-loop pass: a restart after SWITCH, the events of the start, the
 * restart-count clear, the periodic self-test, then the mode's own work.
 *
 * The periodic self-test (runSelfTestWhenDue, every kSelfTestPeriodMs, owner's decision 23,
 * 15 Oct 2026) comes before the mode's work, so a failure it finds is handled by that work on
 * the same pass: pollOperate restarts, pollTrial reports EVENT 0x000B and gives the trial up.
 * It does not run on a pass that restarts after SWITCH: the chip is going away.
 *
 * Called after the frame of this pass was handled, so an answer always goes out before the
 * events and the restart it causes: UPLOAD END of a bad test image is answered REFUSED 0x08,
 * then EVENT 0x000B and 0x0008 follow on this pass.
 *
 * Every HealthMode has its own case (see the file comment). FALLBACK runs exactly OPERATE's work
 * (pollOperate: the periodic self-test's reaction, decision L), as owner's decision 30, 15 Oct
 * 2026 sets it; what differs in FALLBACK is only which commands are accepted (acceptDangerous,
 * acceptLeaveFallback). Before decision 30 no code set FALLBACK, and its case was the fault
 * reaction, like a corrupted value.
 *
 * @param[in]     now_ms  milliseconds since start
 * @note Does not return for a value outside HealthMode.
 */
void ModeManager::poll(uint32_t now_ms)
{
    if (restart_due_) {
        restartWhenSent();
        return;
    }
    sendStartEvents(now_ms);
    clearRestartCountWhenDue(now_ms);
    runSelfTestWhenDue(now_ms);
    switch (mode_) {
    case HealthMode::kOperate:
        pollOperate(now_ms);
        return;
    case HealthMode::kTrial:
        pollTrial(now_ms);
        return;
    case HealthMode::kFallback:
        pollOperate(now_ms);   /* OPERATE's work: see above */
        return;
    }
    /* only a corrupted value reaches here */
    faultImpossibleValue(FAULT_AT_MODE_MANAGER_POLL, "ModeManager::poll");
}

/**
 * @brief Returns true if the payload's identifier is SWITCH (0x20), RETURN TO PREVIOUS (0x21),
 * START FAILED (0x22) or CONFIRM (0x26).
 *
 * The size is checked by acceptDangerous(), so one of them of the wrong size is refused 0x06
 * there. START FAILED is dangerous (Interfaces Sec. 5.1, command table, row 0x22); it joined with
 * owner's decision 30, 15 Oct 2026, and was refused 0x06 as unknown before.
 *
 * @param[in]     payload  command payload
 * @param[in]     length   payload bytes
 * @return true for SWITCH, RETURN TO PREVIOUS, START FAILED or CONFIRM, false otherwise or for an
 *         empty payload
 */
bool ModeManager::isDangerous(const uint8_t *payload, uint16_t length)
{
    return (length >= 1U) && ((payload[0] == kCmdSwitch) ||
                              (payload[0] == kCmdReturnToPrevious) ||
                              (payload[0] == kCmdStartFailed) ||
                              (payload[0] == kCmdConfirm));
}

/**
 * @brief The checks on arrival of SWITCH, RETURN TO PREVIOUS, START FAILED or CONFIRM, in the
 * order of the decided design; each refusal is answered REFUSED only.
 *
 * 0. the command itself (0x06, REQ-028): exactly 1 byte, START FAILED exactly 2; START FAILED's
 *    copy must be the slot that is not running (startFailedCopyOk; owner's decision 30, 15 Oct
 *    2026: the running slot, 0x03 the last-resort firmware and any other value are 0x06). Its
 *    place, with the size and before the mode, is a Proposal (TBR), awaiting the owner's
 *    acceptance: the copy is a value of the command, which REQ-028 checks with the command;
 * 1. mode (REQ-027): SWITCH and START FAILED in OPERATE or FALLBACK, CONFIRM only in TRIAL
 *    (Interfaces Sec. 5.1, command table; owner's decision 30 added FALLBACK to SWITCH and built
 *    START FAILED); else 0x05. RETURN TO PREVIOUS only in TRIAL, else 0x05. A mode byte that is
 *    no HealthMode (a corrupted value) matches no mode and is refused here; poll() gives it the
 *    fault reaction on the same pass.
 *    Accepted by the owner's decision 14a, 15 Oct 2026: RETURN TO PREVIOUS is accepted in TRIAL
 *    only and refused 0x05 in OPERATE; the Interfaces row 0x21 (line 359 of
 *    docs/interfaces/main.tex) says TRIAL. (The row had allowed OPERATE, but what "previous"
 *    means in OPERATE was not defined, REQ-113.) The trial time and the trial tests do
 *    not apply to RETURN TO PREVIOUS: the operator may give a trial up at any time.
 *    CONFIRM also needs the trial time not to be
 *    over: from kTrialMs after the start on, the trial is over (REQ-013) even if poll() has not
 *    yet given it up on this pass, so CONFIRM is refused 0x05 too (audit finding #22). Why 0x05
 *    and not another code of Interfaces Sec. 4.2: the table's other time codes are 0x02
 *    (arming command expired, REQ-030) and 0x03 (command past its expiry, REQ-066), which are
 *    about the ARM and about the command's own expiry, both still valid here; 0x0F is "trial
 *    tests not passed", which is not the reason. What is wrong is that the controller is no
 *    longer in a trial, which is "not allowed in the current mode". It is checked with the mode,
 *    before the arming, so the ARM stays in place (see below);
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
    if (!isDangerous(payload, length)) {
        return AnswerCode::kImpossible;
    }
    uint8_t command = payload[0];
    uint16_t size = (command == kCmdStartFailed) ? kStartFailedBytes : kDangerousBytes;
    if (length != size) {
        return AnswerCode::kImpossible;
    }
    /* The size is checked above, so payload[1] exists here. */
    if ((command == kCmdStartFailed) && !startFailedCopyOk(payload[1], running_)) {
        return AnswerCode::kImpossible;
    }
    bool trial_command = (command == kCmdReturnToPrevious) || (command == kCmdConfirm);
    bool allowed = trial_command ? (mode_ == HealthMode::kTrial)
                                 : ((mode_ == HealthMode::kOperate) ||
                                    (mode_ == HealthMode::kFallback));
    if (!allowed) {
        return AnswerCode::kMode;
    }
    /* Unsigned subtraction stays right when the millisecond count wraps. */
    if ((command == kCmdConfirm) && ((now_ms - trial_start_ms_) >= kTrialMs)) {
        return AnswerCode::kMode;   /* the trial time is over: see the list above */
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
 * @brief SWITCH (REQ-021) or START FAILED (REQ-019), after RECEIVED: boot state confirmed =
 * running slot, trial = the other slot, failed starts = 0, FALLBACK flag cleared; the restart
 * follows on the next pass (poll), after DONE.
 *
 * The two differ only in the given-up mark (owner's decision 30, 15 Oct 2026; finding F-1):
 *   SWITCH: if the other slot's header body check value (headerBodyCheck) equals the mark and the
 *      mark is not 0, REFUSED 0x13 "the image was given up; START FAILED starts it again", and
 *      nothing is written; otherwise the mark is kept. Checked after E (it needs the boot state)
 *      and before F(a), so a given-up image is refused without the 0.081 s of checks; an image
 *      whose header has no proprietary string gives 0 and never matches, so F(a) answers for it.
 *      This order is a Proposal (TBR), awaiting the owner's acceptance;
 *   START FAILED: no mark check (it is the operator's order to start that image again), and the
 *      mark is written as 0. Its copy byte was checked on arrival (startFailedCopyOk): it names
 *      the other slot, so the work uses otherSlot(running_). E and F(a) apply to it as to SWITCH:
 *      Proposal (TBR), awaiting the owner's acceptance (decision 30 names only F(a), "the slot
 *      check"; E keeps START FAILED from replacing a trial already set, as for SWITCH).
 * The mark stays through CONFIRM and is cleared only by START FAILED or replaced by the next
 * give-up: Proposal (TBR), awaiting the owner's acceptance.
 *
 * Before the write, in this order:
 *   E: the boot state is read again; SWITCH proceeds only if it names the running slot as
 *      confirmed and no trial is set. Otherwise 0x05, "not allowed in the current mode": the
 *      command is right and armed, but the controller is not in the state SWITCH needs (it runs a
 *      slot that is not the confirmed one, or a trial is already waiting, or the boot state
 *      cannot be read, so neither is known). Not 0x06 (SWITCH has no values to be wrong), not
 *      0x07 (that is for uploads during a trial, REQ-050), not 0x11 (nothing was written). Why
 *      again: the mode was decided at start from the boot state as it was then;
 *   F(a): the image in the other slot gets UPLOAD END's checks (UpdateReceiver::checkSlot:
 *      contents, both signatures, flash read errors), and a refusal answers that check's code
 *      (0x08, 0x09, 0x0B, 0x0E, 0x10). Why: the boot state must not name a trial that the
 *      bootloader would refuse; the check sees the slot as it is now, after anything since END.
 *      Time: END's checks took 0.081 s at 170 MHz on the bench (test/controller/results/
 *      2026-10-12/step-2c-cpp-upload-probe.log, line 82), inside the watchdog's 1.88 s; at 16 MHz
 *      after a clock fault, 10.6 times longer (computed), not measured: TBD.
 * N: a failed boot-state write answers 0x11, "boot state could not be stored" (Interfaces Sec. 4.2,
 * accepted 13 Oct 2026); before, 0x06. The chip does not restart: nothing changed.
 *
 * @param[in]     start_failed  true for START FAILED, false for SWITCH
 * @return AnswerCode::kSuccess if the boot state was written; kImpossible (0x06) if the running
 *         slot is not A or B; kMode (0x05) per E; kImageGivenUp (0x13) for SWITCH to the given-up
 *         image; the image check's code per F(a); kStateNotStored (0x11) if the write failed
 */
AnswerCode ModeManager::runSwitch(bool start_failed)
{
    boot_state_t state{};

    if ((running_ != Slot::kA) && (running_ != Slot::kB)) {
        return AnswerCode::kImpossible;
    }
    if (!ModePort::readBootState(state) || (toSlot(state.confirmed) != running_) ||
        (toSlot(state.trial) != Slot::kNone)) {
        return AnswerCode::kMode;
    }
    Slot other = otherSlot(running_);
    if (!start_failed && (state.given_up_check != 0U) &&
        (headerBodyCheck(other) == state.given_up_check)) {
        return AnswerCode::kImageGivenUp;
    }
    AnswerCode image = UpdateReceiver::checkSlot(other);
    if (image != kUpdateOk) {
        return image;
    }
    uint32_t mark = start_failed ? 0U : state.given_up_check;
    state = boot_state_t{};
    state.confirmed = stateCode(running_);
    state.trial = stateCode(other);
    state.failed_starts = 0U;
    state.flags = 0U;
    state.given_up_check = mark;
    if (!ModePort::writeBootState(state)) {
        return AnswerCode::kStateNotStored;
    }
    restart_due_ = true;
    return AnswerCode::kSuccess;
}

/**
 * @brief CONFIRM, after RECEIVED (REQ-012, REQ-020): boot state confirmed = running slot,
 * trial = none, failed starts = 0; on success EVENT 0x0007 "firmware confirmed" and mode
 * OPERATE (the trial timer stops), and the caller answers DONE 0x00.
 *
 * N: a failed boot-state write answers 0x11, "boot state could not be stored" (Interfaces
 * Sec. 4.2, accepted 13 Oct 2026); before, 0x06. The mode stays TRIAL: the trial timer runs on,
 * so the operator can ARM and CONFIRM again, and without a CONFIRM the trial ends at its time
 * limit (REQ-013).
 * Owner's decision 30, 15 Oct 2026: the FALLBACK flag is written as 0 and the given-up mark is
 * kept, copied from the boot state as it is now. A boot state that cannot be read has no mark to
 * copy, and the record is written with mark 0: Proposal (TBR), awaiting the owner's acceptance.
 * (In TRIAL, start() read a valid boot state, and only this firmware has written it since.)
 *
 * @param[in]     now_ms  milliseconds since start
 * @return AnswerCode::kSuccess if the boot state was written; AnswerCode::kStateNotStored (0x11)
 *         if not
 */
AnswerCode ModeManager::runConfirm(uint32_t now_ms)
{
    boot_state_t current{};
    boot_state_t state{};

    uint32_t mark = ModePort::readBootState(current) ? current.given_up_check : 0U;
    state.confirmed = stateCode(running_);
    state.trial = BOOT_SLOT_NONE;
    state.failed_starts = 0U;
    state.flags = 0U;
    state.given_up_check = mark;
    if (!ModePort::writeBootState(state)) {
        return AnswerCode::kStateNotStored;
    }
    /* The boot state now names this slot confirmed: the trial is over, whatever happens to the
     * event below. */
    mode_ = HealthMode::kOperate;
    sendEvent(EventCode::kConfirmed, nullptr, 0U, now_ms);
    return AnswerCode::kSuccess;
}

/**
 * @brief RETURN TO PREVIOUS, after RECEIVED (REQ-113): gives the trial up as a failed trial test
 * does (giveUp): boot state confirmed = the other slot, trial = none, failed starts = 0, the
 * FALLBACK flag and the given-up image's mark (giveUpState; owner's decision 30, 15 Oct 2026),
 * then EVENT 0x0008 "firmware given up"; the restart follows on the next pass (poll), after DONE
 * (REQ-034), as for SWITCH. The previous firmware then starts in FALLBACK.
 *
 * Only TRIAL reaches here (acceptDangerous), so the confirmed slot is the other one, as in
 * giveUp(). Unlike giveUp(), a failed boot-state write answers 0x11, "boot state could not be
 * stored" (Interfaces Sec. 4.2), and leaves no give-up request: the operator is told and can order
 * it again, and the chip does not restart, as for runSwitch(). Accepted by the owner's decision
 * 15a, 15 Oct 2026.
 * EVENT 0x0008 has no detail byte in Interfaces Sec. 5.4, so the operator's order cannot be told
 * apart from a time-out or a failed test in the event itself; it is told apart by the DONE that
 * precedes the event after an order. Accepted by the owner's decision 16a, 15 Oct 2026.
 *
 * @param[in]     now_ms  milliseconds since start
 * @return AnswerCode::kSuccess if the boot state was written; kImpossible (0x06) if the running
 *         slot is not A or B; kStateNotStored (0x11) if the write failed
 */
AnswerCode ModeManager::runReturnToPrevious(uint32_t now_ms)
{
    if ((running_ != Slot::kA) && (running_ != Slot::kB)) {
        return AnswerCode::kImpossible;
    }
    boot_state_t state = giveUpState(running_);
    if (!ModePort::writeBootState(state)) {
        return AnswerCode::kStateNotStored;
    }
    sendEvent(EventCode::kGivenUp, nullptr, 0U, now_ms);
    restart_due_ = true;
    return AnswerCode::kSuccess;
}

/**
 * @brief Runs SWITCH, RETURN TO PREVIOUS, START FAILED or CONFIRM after acceptDangerous()
 * accepted it and RECEIVED was sent.
 *
 * acceptDangerous() lets only these four identifiers through, so any other value here is a
 * corrupted variable: the fault reaction (firmware/fault.hpp). Before, it answered 0x06.
 * START FAILED needs only its identifier here: its one valid copy is the other slot (see
 * startFailedCopyOk), so runSwitch acts on otherSlot(running_).
 *
 * @param[in]     command  kCmdSwitch, kCmdReturnToPrevious, kCmdStartFailed or kCmdConfirm
 * @param[in]     now_ms   milliseconds since start
 * @return AnswerCode::kSuccess (answer DONE 0x00), or the refusal code (answer REFUSED)
 * @note Does not return for any other command.
 */
AnswerCode ModeManager::runDangerous(uint8_t command, uint32_t now_ms)
{
    if (command == kCmdSwitch) {
        return runSwitch(false);
    }
    if (command == kCmdStartFailed) {
        return runSwitch(true);
    }
    if (command == kCmdReturnToPrevious) {
        return runReturnToPrevious(now_ms);
    }
    if (command == kCmdConfirm) {
        return runConfirm(now_ms);
    }
    /* only a corrupted value reaches here */
    faultImpossibleValue(FAULT_AT_MODE_MANAGER_RUN_DANGEROUS, "ModeManager::runDangerous");
}

/**
 * @brief Returns true if the payload's identifier is LEAVE FALLBACK (0x23).
 *
 * The size is checked by acceptLeaveFallback(), so a LEAVE FALLBACK of the wrong size is refused
 * 0x06 there, as for the dangerous commands (isDangerous).
 *
 * @param[in]     payload  command payload
 * @param[in]     length   payload bytes
 * @return true for LEAVE FALLBACK, false otherwise or for an empty payload
 */
bool ModeManager::isLeaveFallback(const uint8_t *payload, uint16_t length)
{
    return (length >= 1U) && (payload[0] == kCmdLeaveFallback);
}

/**
 * @brief The checks on arrival of LEAVE FALLBACK (Interfaces Sec. 5.1, command table, row 0x23;
 * REQ-117; owner's decision 30, 15 Oct 2026); each refusal is answered REFUSED only.
 *
 * 0. the command itself: exactly 1 byte (0x06, REQ-028);
 * 1. mode (REQ-027): FALLBACK only, else 0x05.
 * Not dangerous (the command table says NO), so no ARM is checked or used up.
 *
 * @param[in]     payload  command payload
 * @param[in]     length   payload bytes
 * @return AnswerCode::kSuccess if the command may run; otherwise the refusal code
 */
AnswerCode ModeManager::acceptLeaveFallback(const uint8_t *payload, uint16_t length) const
{
    if ((length != kLeaveFallbackBytes) || !isLeaveFallback(payload, length)) {
        return AnswerCode::kImpossible;
    }
    if (mode_ != HealthMode::kFallback) {
        return AnswerCode::kMode;
    }
    return AnswerCode::kSuccess;
}

/**
 * @brief LEAVE FALLBACK, after RECEIVED (REQ-117; owner's decision 30, 15 Oct 2026): the boot
 * state with the FALLBACK flag cleared, then mode OPERATE at once, with no restart.
 *
 * The record is the boot state as it is now with only the flag changed, so the confirmed slot,
 * the trial, the start count and the given-up mark stay as they are (the mark is kept: a later
 * SWITCH to the given-up image is still refused 0x13). The running firmware does not change, so
 * nothing restarts. A failed write answers 0x11, "boot state could not be stored", and the mode
 * stays FALLBACK. A boot state that cannot be read cannot be copied, so nothing is written and
 * the answer is 0x11 too: Proposal (TBR), awaiting the owner's acceptance (0x05, as SWITCH's
 * decision E answers an unreadable boot state, would say the mode is wrong, which it is not).
 *
 * @return AnswerCode::kSuccess if the boot state was written; kStateNotStored (0x11) if it was
 *         not read or not written
 */
AnswerCode ModeManager::runLeaveFallback()
{
    boot_state_t state{};

    if (!ModePort::readBootState(state)) {
        return AnswerCode::kStateNotStored;   /* see above */
    }
    state.flags = 0U;
    if (!ModePort::writeBootState(state)) {
        return AnswerCode::kStateNotStored;
    }
    mode_ = HealthMode::kOperate;
    return AnswerCode::kSuccess;
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
