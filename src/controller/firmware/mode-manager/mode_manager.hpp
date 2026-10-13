/*
 * mode_manager.hpp — the controller's mode, the trial of new firmware, its trial tests, and the
 * two dangerous commands of this step, SWITCH and CONFIRM (Architecture: mode manager;
 * REQ-011 to REQ-021, REQ-139, REQ-140).
 *
 * Mode at start (decided design, step 2d): the running slot is the boot state's trial slot ->
 * TRIAL; otherwise OPERATE. FALLBACK is a later step. The mode then changes only by CONFIRM
 * (TRIAL -> OPERATE); every other way out of a trial is a restart.
 *
 * The trial (TRIAL only):
 *   - EVENT 0x0006 "trial started", once, on the first pass after start;
 *   - the trial tests (REQ-139), run on their own:
 *       self-test     the running image's body CRC-32 equals its header's check value;
 *       data link     an answer went out whole, and no send failed;
 *       test image    a test image (image number 0xFFFF) arrived with a right check value;
 *     all three passed: EVENT 0x000A "trial tests passed", once;
 *   - a test fails (REQ-140): EVENT 0x000B with the test's number, then the trial is given up;
 *   - kTrialMs pass without a CONFIRM (REQ-013): the trial is given up;
 *   - given up: the boot state is written with trial = none and failed starts = 0, EVENT 0x0008
 *     "firmware given up", the UART is drained (bounded), and the chip restarts; the bootloader
 *     then starts the confirmed slot. If the write fails, a give-up request is left in the backup
 *     registers instead, and the bootloader applies it (decision G).
 *
 * At every start, once: EVENT 0x0001 if the clock fell back to 16 MHz (decision C), and EVENT
 * 0x0002 if the bootloader left a fall-back record for this start (Interfaces Sec. 5.4). After
 * kNormalRunMs of running, the bootloader's restart count of this slot is cleared (decision B).
 * In OPERATE a failed self-test sends EVENT 0x0004 and restarts the chip, counted by the
 * bootloader (decision L).
 *
 * SWITCH (0x20) and CONFIRM (0x26): checks on arrival in this order, each refusal answered
 * REFUSED only: mode (SWITCH in OPERATE, CONFIRM in TRIAL and before kTrialMs have passed; else
 * 0x05), arming (0x01, 0x02; command-checker/arm_state.hpp), and for CONFIRM the trial tests
 * (0x0F). Then RECEIVED, the work, and DONE or REFUSED. SWITCH's work: the boot state read again
 * (decision E, 0x05), the other slot's image checked (decision F(a), the UPLOAD END codes), the
 * boot-state write (0x11 if it fails, decision N). CONFIRM's work: the boot-state write (0x11).
 *
 * Every decision is here; main.cpp only routes frames and sends the answers. The chip is reached
 * through mode_port.hpp and the link through FrameSender, so the same file runs in the host unit
 * tests (test/controller/unit/test_mode.cpp).
 */
#ifndef MODE_MANAGER_HPP
#define MODE_MANAGER_HPP

#include <cstdint>

#include "arm_state.hpp"
#include "boot_slot.hpp"
#include "frame.hpp"
#include "frame_sender.hpp"
#include "health_monitor.hpp"
#include "mode_port.hpp"
#include "test_image.hpp"
#include "update_receiver.hpp"

/* Trial length: 120 s from the firmware's start in TRIAL. Bench value; the requirement's value
 * is TBD-trial in the SRS (REQ-011). */
constexpr uint32_t kTrialMs = 120000U;

/* How long the firmware must run before it clears the bootloader's count of restarts of its slot
 * (decision B, owner, 13 Oct 2026): 60 s from the firmware's start. Bench value, TBD for flight. */
constexpr uint32_t kNormalRunMs = 60000U;

/* The dangerous commands of this step, each 1 byte with no fields (Interfaces Sec. 5.1).
 * RETURN TO PREVIOUS (0x21) and START FAILED (0x22) are not in this step. */
constexpr uint8_t kCmdSwitch = 0x20U;
constexpr uint8_t kCmdConfirm = 0x26U;
constexpr uint16_t kDangerousBytes = 1U;

/* The detail byte of EVENT 0x000B "trial tests failed": which test (Interfaces Sec. 5.4: 0x01
 * self-test, 0x02 data link, 0x03 test image). Accepted by the owner, 13 Oct 2026 (decision P). */
enum class TrialTest : uint8_t {
    kNone = 0x00U,        /* no test failed (never sent) */
    kSelfTest = 0x01U,
    kDataLink = 0x02U,
    kTestImage = 0x03U,
};

class ModeManager {
public:
    /**
     * @brief A mode manager that sends through `sender` and judges `test_image`.
     *
     * @param[in,out] sender      the frame sender for answers and events
     * @param[in,out] test_image  the trial's test image
     */
    constexpr ModeManager(FrameSender &sender, TestImage &test_image)
        : sender_(sender), test_image_(test_image)
    {
    }

    /** @brief Decide the mode from the boot state and run the self-test; once, at start. */
    void start(Slot running, uint32_t now_ms, bool clock_fault);

    /** @brief The work of one main-loop pass: events, failed tests, time-out, restart. */
    void poll(uint32_t now_ms);

    /** @brief True if the payload's identifier is SWITCH or CONFIRM, whatever its size. */
    [[nodiscard]] static bool isDangerous(const uint8_t *payload, uint16_t length);

    /** @brief Checks on arrival of SWITCH or CONFIRM: kSuccess or the refusal code. */
    AnswerCode acceptDangerous(const uint8_t *payload, uint16_t length, uint32_t sequence,
                               uint32_t now_ms, ArmState &arm);

    /** @brief Run SWITCH or CONFIRM after RECEIVED: kSuccess or the refusal code. */
    AnswerCode runDangerous(uint8_t command, uint32_t now_ms);

    /** @brief Checks on arrival of a test-image command (TRIAL only): kSuccess or 0x06. */
    AnswerCode acceptTestImage(const UpdateCommand &cmd) const;

    /** @brief Run a test-image command after RECEIVED: kSuccess, or 0x08 at END. */
    AnswerCode runTestImage(const UpdateCommand &cmd);

    /** @brief The mode, coded as in the HEALTH report. */
    HealthMode mode() const
    {
        return mode_;
    }

    /** @brief Seconds of trial left, rounded up; 0 outside a trial. */
    uint16_t trialSecondsLeft(uint32_t now_ms) const;

    /** @brief The result of the self-test at start. */
    bool selfTestPassed() const
    {
        return self_test_passed_;
    }

    /** @brief True if all three trial tests have passed. */
    bool trialTestsPassed() const;

    /** @brief The first failed trial test in the order self-test, data link, test image. */
    TrialTest firstFailedTest() const;

    /** @brief The self-test (REQ-039): the running image's body against its header's check value. */
    [[nodiscard]] static bool selfTest(Slot running);

private:
    void sendEvent(EventCode code, const uint8_t *detail, uint32_t detail_bytes, uint32_t now_ms);
    void sendStartEvents(uint32_t now_ms);
    [[nodiscard]] bool fallbackForThisStart(const FallbackRecord &record) const;
    void clearRestartCountWhenDue(uint32_t now_ms);
    void pollOperate(uint32_t now_ms);
    void pollTrial(uint32_t now_ms);
    void giveUp(uint32_t now_ms);
    void restartWhenSent();
    AnswerCode runSwitch();
    AnswerCode runConfirm(uint32_t now_ms);

    FrameSender &sender_;
    TestImage &test_image_;
    HealthMode mode_ = HealthMode::kOperate;
    Slot running_ = Slot::kNone;
    bool self_test_passed_ = false;
    bool trial_started_sent_ = false;   /* EVENT 0x0006 sent */
    bool tests_passed_sent_ = false;    /* EVENT 0x000A sent */
    bool restart_due_ = false;          /* SWITCH done: restart on the next pass */
    bool clock_fault_ = false;          /* the clock fell back to 16 MHz (decision C) */
    bool start_events_sent_ = false;    /* EVENT 0x0001 / 0x0002 of this start handled */
    bool restart_count_cleared_ = false;   /* decision B's count cleared after kNormalRunMs */
    uint32_t trial_start_ms_ = 0U;      /* the firmware's start, milliseconds since start */
};

#endif /* MODE_MANAGER_HPP */
