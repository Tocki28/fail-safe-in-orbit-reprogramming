/*
 * mode_manager.hpp — the controller's mode, the trial of new firmware, its trial tests, and the
 * three dangerous commands of this step, SWITCH, RETURN TO PREVIOUS and CONFIRM (Architecture:
 * mode manager; REQ-011 to REQ-021, REQ-113, REQ-139, REQ-140).
 *
 * Mode at start (decided design, step 2d): the running slot is the boot state's trial slot ->
 * TRIAL; otherwise OPERATE. FALLBACK is a later step. The mode then changes only by CONFIRM
 * (TRIAL -> OPERATE); every other way out of a trial, RETURN TO PREVIOUS included, is a restart.
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
 *     registers instead, and the bootloader applies it (decision G); EVENT 0x0008 still goes out
 *     when the request is stored (decision 5).
 *
 * At every start, once: EVENT 0x0005 with the fault location if the bootloader left one
 * (decision 3), EVENT 0x0001 with the fault kind if the clock fell back to 16 MHz (decisions C
 * and 2), and one EVENT 0x0002 per copy the bootloader refused or skipped on this start
 * (decision 4; Interfaces Sec. 5.4). After kNormalRunMs of running, the bootloader's restart
 * count of this slot is cleared (decision B).
 * In OPERATE a failed self-test sends EVENT 0x0004 and restarts the chip, counted by the
 * bootloader (decision L).
 *
 * The self-test runs at every start and then every kSelfTestPeriodMs while the firmware runs, in
 * OPERATE and in TRIAL (REQ-039; owner's decision 23, 15 Oct 2026). A failed periodic self-test
 * has the reaction of a failed one at start in each mode: in OPERATE EVENT 0x0004 and the
 * restart (decision L), in TRIAL EVENT 0x000B with detail 0x01 and the give-up (REQ-140). The
 * reasoning, the time one test takes and what it costs the watchdog and the UART are at
 * kSelfTestPeriodMs.
 *
 * SWITCH (0x20), RETURN TO PREVIOUS (0x21) and CONFIRM (0x26): checks on arrival in this order,
 * each refusal answered REFUSED only: mode (SWITCH in OPERATE, RETURN TO PREVIOUS in TRIAL,
 * CONFIRM in TRIAL and before kTrialMs have passed; else 0x05), arming (0x01, 0x02;
 * command-checker/arm_state.hpp), and for CONFIRM the trial tests (0x0F). Then RECEIVED, the work,
 * and DONE or REFUSED. SWITCH's work: the boot state read again (decision E, 0x05), the other
 * slot's image checked (decision F(a), the UPLOAD END codes), the boot-state write (0x11 if it
 * fails, decision N). CONFIRM's work: the boot-state write (0x11). RETURN TO PREVIOUS's work: the
 * give-up's boot-state write (0x11 if it fails) and EVENT 0x0008; the restart follows on the next
 * pass, after DONE (REQ-034), as for SWITCH.
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

/* The self-test's period: 60 s. Owner decision 23, 15 Oct 2026; bench value for TBD-selftest
 * (docs/requirements/main.tex, Open Items; REQ-039). There is no technical reason for 60 s: the
 * owner chose it for the bench, and the flight value stays TBD.
 *
 * What runs: ModeManager::selfTest(), the CRC-32 of the running image's body against the check
 * value in its header, plus the flash read-error flag. It runs in start() and again from poll()
 * whenever at least kSelfTestPeriodMs have passed since the last run, the one in start() counting
 * as the first. At most once per poll() pass, so a long pause of the main loop gives one run, not
 * a catch-up. It runs in OPERATE and in TRIAL; it reads only the running slot's flash and changes
 * nothing but the stored result, which HEALTH reports as "last self-test" (Interfaces Sec. 5.4).
 * A pass that finds the image intact sends nothing.
 *
 * Why it runs again: the flash can change while the firmware runs (a damaged word, REQ-041); a
 * test at start alone would find it only at the next start. With the period, the longest an
 * unnoticed change to the image's body lives is kSelfTestPeriodMs plus one main-loop pass.
 *
 * When it fails, the reaction is the one a failed self-test at start already has in that mode:
 *   OPERATE  EVENT 0x0004, a bounded wait for the UART, restart counted by the bootloader
 *            (decision L; pollOperate);
 *   TRIAL    EVENT 0x000B with detail 0x01, then the give-up: boot state trial = none, EVENT
 *            0x0008, restart into the previous firmware (REQ-140; pollTrial, giveUp).
 * No new reaction is added. The bootloader checks the image again after the restart, so a
 * damaged image is refused there and a loop of restarts is ended by its count (decision B).
 *
 * How long one test takes (computed, not measured; to be measured on the bench). The cost is the
 * CRC-32 over the body. src/controller/common/crc32.c, line 3, gives "~10 ms for a full 40 KB
 * slot at 170 MHz" (kCoreClockHz, core_clock.hpp). The longest body is kBodyMaxBytes = SLOT_BYTES
 * 40960 - header 64 - two signatures 128 = 40768 bytes (common/layout.h, lines 38, 72, 74-75),
 * so 40768 / 40960 x 10 ms = 9.95 ms, at most 10 ms. crc32.c states no optimisation level and
 * the firmware builds with -Os (firmware/Makefile, line 44), so the bench may show more. After a
 * clock fault the core runs at 16 MHz (kHsi16Hz, decision C): 10 ms x 170 / 16 = 106 ms.
 *   Against the watchdog: it is bounded at 1.88 s at the shortest (watchdog.hpp, line 3; the
 *     requirement calls it 2 s) and reloaded every 100 ms between passes. One test is 0.5 % of
 *     1.88 s at 170 MHz (10 / 1880) and 5.6 % at 16 MHz (106 / 1880). The test and an UPLOAD END
 *     check (about 40 ms at -O2, watchdog.hpp, lines 46-47) can fall in one pass; their sum is
 *     still far below 1.88 s. The test adds its own time to the pass and nothing more: it has
 *     no wait and no loop but the CRC-32's, which is bounded by the body length checked against
 *     kBodyMaxBytes before any byte is read.
 *   Against the UART: the interrupt keeps running during the test, and the ring holds the
 *     bytes. At 115200 baud, 8N1, 10 bits per byte (uart_link.hpp, kBaudRate and the ring
 *     comment) 11520 bytes arrive per second: 116 bytes in 10 ms (computed, rounded up), 5.7 %
 *     of the 2048-byte ring (UartLink::kRingBytes); 1218 bytes in 106 ms at 16 MHz, 59.5 %.
 *     The ring comment already sizes it for pauses of tens of ms (UPLOAD END), so a test
 *     adds no new kind of pause.
 *
 * The millisecond count wraps after 2^32 ms = 49.7 days (computed: 4294967296 / 86400000). The
 * elapsed time is therefore an unsigned subtraction, now_ms - last run, which stays right across
 * the wrap as long as the period is far below 2^32 ms. */
constexpr uint32_t kSelfTestPeriodMs = 60000U;

/* The dangerous commands of this step, each 1 byte with no fields (Interfaces Sec. 5.1).
 * START FAILED (0x22) is not in this step. RETURN TO PREVIOUS is row 0x21 of the command table
 * (docs/interfaces/main.tex, line 359; REQ-113). */
constexpr uint8_t kCmdSwitch = 0x20U;
constexpr uint8_t kCmdReturnToPrevious = 0x21U;
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

    /** @brief True if the payload's identifier is SWITCH, RETURN TO PREVIOUS or CONFIRM. */
    [[nodiscard]] static bool isDangerous(const uint8_t *payload, uint16_t length);

    /** @brief Checks on arrival of SWITCH, RETURN TO PREVIOUS or CONFIRM: kSuccess or refusal. */
    AnswerCode acceptDangerous(const uint8_t *payload, uint16_t length, uint32_t sequence,
                               uint32_t now_ms, ArmState &arm);

    /** @brief Run SWITCH, RETURN TO PREVIOUS or CONFIRM after RECEIVED: kSuccess or refusal. */
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

    /**
     * @brief The result of the last self-test: the one at start, or the latest periodic one.
     *
     * HEALTH's "last self-test" field (Interfaces Sec. 5.4) and trial test a of REQ-139 read it.
     * poll() replaces it every kSelfTestPeriodMs (owner's decision 23, 15 Oct 2026).
     *
     * @return true if the last self-test passed
     */
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
    void sendBootloaderFault(uint32_t now_ms);
    void sendFallbackEvents(uint32_t now_ms);
    void clearRestartCountWhenDue(uint32_t now_ms);
    void runSelfTestWhenDue(uint32_t now_ms);
    void pollOperate(uint32_t now_ms);
    void pollTrial(uint32_t now_ms);
    void giveUp(uint32_t now_ms);
    void restartWhenSent();
    AnswerCode runSwitch();
    AnswerCode runConfirm(uint32_t now_ms);
    AnswerCode runReturnToPrevious(uint32_t now_ms);

    FrameSender &sender_;
    TestImage &test_image_;
    HealthMode mode_ = HealthMode::kOperate;
    Slot running_ = Slot::kNone;
    bool self_test_passed_ = false;
    bool trial_started_sent_ = false;   /* EVENT 0x0006 sent */
    bool tests_passed_sent_ = false;    /* EVENT 0x000A sent */
    bool restart_due_ = false;          /* SWITCH done: restart on the next pass */
    bool clock_fault_ = false;          /* the clock fell back to 16 MHz (decision C) */
    bool start_events_sent_ = false;    /* EVENT 0x0005 / 0x0001 / 0x0002 of this start handled */
    bool restart_count_cleared_ = false;   /* decision B's count cleared after kNormalRunMs */
    uint32_t trial_start_ms_ = 0U;      /* the firmware's start, milliseconds since start */
    uint32_t last_self_test_ms_ = 0U;   /* the last self-test's poll() time (start() the first) */
};

#endif /* MODE_MANAGER_HPP */
