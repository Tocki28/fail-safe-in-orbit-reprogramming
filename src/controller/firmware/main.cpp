/*
 * main.cpp — the controller's main firmware, step 2d: it starts, keeps the watchdog reloaded,
 * runs data link A, answers GET STATUS, receives firmware updates into the slot that is not
 * confirmed, runs the trial of new firmware with its trial tests, takes ARM, SWITCH and
 * CONFIRM, and sends a HEALTH report every second and EVENT reports as events happen.
 *
 * This file is the wiring only: one object per component, the routing of each command to its
 * component, and the answers. The decisions are in the components.
 *
 * Commands handled (Interfaces Sec. 5.1):
 *   GET STATUS (0x25, no fields): RECEIVED, then DONE 0x00.
 *   UPLOAD BEGIN, CHUNK, END, CANCEL (0x01-0x04): image number 0xFFFF, the test image, goes to
 *     the mode manager (mode-manager/test_image.hpp), every other number to the update receiver
 *     (update-receiver/). A refusal on arrival is answered REFUSED only; otherwise RECEIVED,
 *     the work, then DONE 0x00 or REFUSED with the reason (Interfaces Sec. 4.2, REQ-033,
 *     REQ-034).
 *   ARM (0x0D): in every mode, RECEIVED then DONE 0x00 (command-checker/arm_state.hpp).
 *   SWITCH (0x20), CONFIRM (0x26): dangerous; checks on arrival and the work in the mode manager
 *     (mode-manager/mode_manager.hpp).
 *   Anything else: REFUSED 0x06 only (Interfaces Sec. 4.2). Not in this step, so also refused
 *     0x06 as unknown: RETURN TO PREVIOUS (0x21), START FAILED (0x22), LEAVE FALLBACK (0x23),
 *     SET CONTROLLER TIME (0x24).
 * A frame not addressed to the controller, or not a command, is ignored.
 *
 * Linked for slot A or B (Makefile: SLOT=A or B).
 *
 * C++: this file owns one object per component, in static storage (no heap). The interrupt
 * handlers forward into those objects. main() starts them and runs the one loop.
 *
 * Start-up (ST's startup_stm32g431xx.s, Reset_Handler): copy .data, clear .bss, call
 * __libc_init_array ("Call static constructors"), then main. __libc_init_array runs every
 * function listed in .init_array, C++ constructors included. The objects below need no
 * constructor code: their members have constant initial values, so the compiler stores them in
 * .bss and .data as finished bytes (constant initialization). .init_array holds only the C
 * library's own entry (frame_dummy, from crtbegin.o), exactly as in the C build (firmware.map).
 * So no object is used before it is built, and no interrupt can reach a half-built one. A
 * constructor that did need code would still run, through __libc_init_array, before main.
 */
#include <array>
#include <cstdint>

#include "arm_state.hpp"
#include "boot_slot.hpp"
#include "boot_state.h"
#include "core_clock.hpp"
#include "frame.hpp"
#include "frame_sender.hpp"
#include "health_monitor.hpp"
#include "layout.h"
#include "mode_manager.hpp"
#include "stm32g4xx.h"
#include "test_image.hpp"
#include "uart_link.hpp"
#include "update_receiver.hpp"
#include "watchdog.hpp"

namespace {

/* GET STATUS: identifier 0x25, no fields (Interfaces Sec. 5.1). */
constexpr uint8_t kCmdGetStatus = 0x25U;
constexpr uint16_t kGetStatusBytes = 1U;
/* Code byte of a RECEIVED answer. Interfaces Sec. 4.2 gives codes only for DONE and REFUSED;
 * 0x00 here is a placeholder awaiting the owner's decision. */
constexpr AnswerCode kReceivedCode = static_cast<AnswerCode>(0x00U);

static_assert(kHealthReportBytes <= kSendPayloadMax, "a HEALTH report must fit FrameSender");

/* The components. At namespace scope, not local to main(), because the interrupt handlers
 * below must reach them too; in the unnamed namespace, so no other file can. Where they sit:
 *   .bss (all-zero bytes): core_clock, uart_link, watchdog, updater, health_monitor,
 *     test_image (with its 1,024-byte buffer);
 *   .data (non-zero initial bytes): sender (an address and the first sequence number),
 *     mode_manager (two addresses), arm_state (the complement words of the disarmed state). */
CoreClock core_clock;
UartLink uart_link;
Watchdog watchdog;
UpdateReceiver updater;
HealthMonitor health_monitor;
FrameSender sender{uart_link};
TestImage test_image;
ModeManager mode_manager{sender, test_image};
ArmState arm_state;

/**
 * @brief An UPLOAD command for the update receiver: REFUSED on arrival, or RECEIVED, the work,
 * then DONE or REFUSED.
 *
 * RECEIVED goes out before the work starts: the onboard computer learns the command arrived
 * even when the work (an erase) takes half a second.
 *
 * @param[in]     frame  the command frame
 */
void handleUpload(const Frame &frame)
{
    UpdateCommand command{};
    AnswerCode result = updater.accept(frame.payload, frame.length, command);

    if (result != kUpdateOk) {
        sender.sendAnswer(frame.sequence, AnswerKind::kRefused, result);
        return;
    }
    sender.sendAnswer(frame.sequence, AnswerKind::kReceived, kReceivedCode);
    result = updater.run(command);
    sender.sendAnswer(frame.sequence,
                      (result == kUpdateOk) ? AnswerKind::kDone : AnswerKind::kRefused, result);
}

/**
 * @brief An UPLOAD command carrying the test image (image number 0xFFFF) for the mode manager,
 * answered as handleUpload answers.
 *
 * @param[in]     frame    the command frame
 * @param[in]     command  its fields, read by UpdateReceiver::parse
 */
void handleTestImage(const Frame &frame, const UpdateCommand &command)
{
    AnswerCode result = mode_manager.acceptTestImage(command);

    if (result != AnswerCode::kSuccess) {
        sender.sendAnswer(frame.sequence, AnswerKind::kRefused, result);
        return;
    }
    sender.sendAnswer(frame.sequence, AnswerKind::kReceived, kReceivedCode);
    result = mode_manager.runTestImage(command);
    sender.sendAnswer(frame.sequence,
                      (result == AnswerCode::kSuccess) ? AnswerKind::kDone : AnswerKind::kRefused,
                      result);
}

/**
 * @brief Routes an UPLOAD command: image number 0xFFFF to the test image, any other number (or
 * a payload whose size fits no upload command) to the update receiver.
 *
 * The test image never reaches the update receiver, so it cannot reach a flash erase or write
 * (Interfaces Sec. 5.1, REQ-139). A payload of the wrong size goes to the receiver, which
 * refuses it 0x06 as before.
 *
 * @param[in]     frame  the command frame
 */
void routeUpload(const Frame &frame)
{
    UpdateCommand command{};

    if (UpdateReceiver::parse(frame.payload, frame.length, command) &&
        TestImage::isTestImage(command)) {
        handleTestImage(frame, command);
        return;
    }
    handleUpload(frame);
}

/**
 * @brief ARM: REFUSED 0x06 if its size is wrong; otherwise RECEIVED, the arm state stored, DONE
 * 0x00. Accepted in every mode (Interfaces Sec. 5.1).
 *
 * @param[in]     frame  the command frame
 */
void handleArm(const Frame &frame)
{
    ArmRequest request{};

    if (!ArmState::parse(frame.payload, frame.length, request)) {
        sender.sendAnswer(frame.sequence, AnswerKind::kRefused, AnswerCode::kImpossible);
        return;
    }
    sender.sendAnswer(frame.sequence, AnswerKind::kReceived, kReceivedCode);
    arm_state.arm(request, core_clock.tickMs());
    sender.sendAnswer(frame.sequence, AnswerKind::kDone, AnswerCode::kSuccess);
}

/**
 * @brief SWITCH or CONFIRM: REFUSED on arrival (mode, arming, trial tests), or RECEIVED, the
 * boot-state write, then DONE or REFUSED.
 *
 * After a SWITCH answered DONE, the mode manager restarts the chip on this same pass
 * (ModeManager::poll), so the DONE leaves first.
 *
 * @param[in]     frame  the command frame
 */
void handleDangerous(const Frame &frame)
{
    AnswerCode result = mode_manager.acceptDangerous(frame.payload, frame.length, frame.sequence,
                                                     core_clock.tickMs(), arm_state);

    if (result != AnswerCode::kSuccess) {
        sender.sendAnswer(frame.sequence, AnswerKind::kRefused, result);
        return;
    }
    sender.sendAnswer(frame.sequence, AnswerKind::kReceived, kReceivedCode);
    result = mode_manager.runDangerous(frame.payload[0], core_clock.tickMs());
    sender.sendAnswer(frame.sequence,
                      (result == AnswerCode::kSuccess) ? AnswerKind::kDone : AnswerKind::kRefused,
                      result);
}

/**
 * @brief Returns true if the frame is GET STATUS of the right size.
 *
 * @param[in]     command  the command frame
 * @return true for a 1-byte payload 0x25, false otherwise
 */
bool isGetStatus(const Frame &command)
{
    return (command.length == kGetStatusBytes) && (command.payload[0] == kCmdGetStatus);
}

/**
 * @brief Routes one received frame to the component that handles its command.
 *
 * Step 2b puts the command checker (firmware/command-checker) in front of this, with the
 * checks on arrival that come before RECEIVED for every command: sequence numbers (REQ-065;
 * ALREADY DONE, REQ-031), expiry (REQ-066, REQ-133), modes (REQ-027), Flags and scheduled
 * commands (REQ-134). Today the mode and arming checks run for SWITCH and CONFIRM only
 * (ModeManager::acceptDangerous), and the command itself is checked (REQ-028).
 *
 * @param[in]     frame  a frame that passed the frame checks (FrameParser)
 */
void handle(const Frame &frame)
{
    if ((frame.destination != Destination::kController) || (frame.type != FrameType::kCommand)) {
        return;   /* Destination names who acts on a frame (Interfaces Sec. 4.0) */
    }
    if (UpdateReceiver::isUpload(frame.payload, frame.length)) {
        routeUpload(frame);
        return;
    }
    if (ArmState::isArm(frame.payload, frame.length)) {
        handleArm(frame);
        return;
    }
    if (ModeManager::isDangerous(frame.payload, frame.length)) {
        handleDangerous(frame);
        return;
    }
    if (!isGetStatus(frame)) {
        sender.sendAnswer(frame.sequence, AnswerKind::kRefused, AnswerCode::kImpossible);
        return;
    }
    sender.sendAnswer(frame.sequence, AnswerKind::kReceived, kReceivedCode);
    sender.sendAnswer(frame.sequence, AnswerKind::kDone, AnswerCode::kSuccess);
}

/**
 * @brief Sends one HEALTH report every kHealthPeriodMs (health-monitor/health_monitor.hpp).
 *
 * Mode, trial time left and self-test come from the mode manager; failed starts from the boot
 * state; firmware kind and version from the running image's header.
 */
void sendHealthWhenDue()
{
    uint32_t now = core_clock.tickMs();

    if (!health_monitor.reportDue(now)) {
        return;
    }
    HealthInputs in{};
    std::array<uint8_t, kHealthReportBytes> payload{};

    in.now_ms = now;
    in.state_valid = boot_state_read(&in.state);
    in.running_slot = updater.runningSlot();
    /* The image header sits at the slot's first byte (Interfaces Sec. 5.5, layout.h). */
    in.running_header =
        (in.running_slot == Slot::kNone)
            ? nullptr
            : reinterpret_cast<const uint8_t *>(
                  static_cast<uintptr_t>(UpdateReceiver::slotStart(in.running_slot)));
    in.mode = mode_manager.mode();
    in.trial_left_s = mode_manager.trialSecondsLeft(now);
    in.self_test_passed = mode_manager.selfTestPassed();
    HealthMonitor::report(in, payload);
    sender.send(FrameType::kReport, payload.data(), kHealthReportBytes);
}

}  // namespace

/*
 * Interrupt handlers. Names fixed by ST's vector table (startup_stm32g431xx.s): it lists these
 * exact names, each as a weak alias of Default_Handler (an endless loop). C++ changes a
 * function's name in the object file to encode its parameters ("name mangling":
 * SysTick_Handler() becomes _Z15SysTick_Handlerv), so the linker would not match it to the
 * table's entry, would keep the weak Default_Handler, and would report no error: the chip
 * would hang at the first tick. extern "C" keeps the plain C name. Each handler only forwards
 * into its object, so the work stays with the state it changes.
 * C: SysTick_Handler was in core_clock.c, USART2_IRQHandler in uart_link.c.
 */

/**
 * @brief The SysTick interrupt, once per millisecond: advances the millisecond count.
 */
extern "C" void SysTick_Handler(void)
{
    core_clock.onTick();
}

/**
 * @brief The USART2 interrupt: takes the received byte into the link's ring.
 *
 * The arrival time is read here and passed in. SysTick and USART2 have the same priority
 * (none is set, core_clock.cpp), so the tick cannot advance while this handler runs: the time
 * is the one the C handler read.
 */
extern "C" void USART2_IRQHandler(void)
{
    uart_link.onInterrupt(core_clock.tickMs());
}

/**
 * @brief Starts the components, then runs the one loop.
 *
 * main needs no extern "C": the language keeps its name as it is, and ST's startup calls it
 * as `bl main`.
 *
 * Order at start: vector table, running slot, clock, tick, link, then the mode manager, which
 * reads the boot state and runs the self-test (REQ-039). The loop: watchdog, arm expiry, the
 * link and its commands, the mode manager (trial events, give-up, restart), the HEALTH report.
 * The mode manager comes after the commands, so an answer goes out before the events and the
 * restart it causes.
 *
 * @note Does not return.
 */
int main()
{
    /* Own vector table, also when started by a debugger instead of the bootloader. This build
     * runs only at the address it is linked for, so VTOR also names the slot it runs from. */
    SCB->VTOR = static_cast<uint32_t>(CODE_START);
    Slot running = UpdateReceiver::slotOf(SCB->VTOR);
    updater.init(running);
    if (!CoreClock::start()) {
        /* No usable clock, so no usable link. Stop reloading the watchdog: it restarts the
         * chip within ~2 s (REQ-022). */
        for (;;) {
        }
    }
    CoreClock::startTick();
    uart_link.init();
    mode_manager.start(running, core_clock.tickMs());

    /* The one endless loop; the watchdog bounds every pass. */
    for (;;) {
        Frame frame{};

        watchdog.reloadWhenDue(core_clock.tickMs());
        arm_state.expireWhenDue(core_clock.tickMs());
        if (uart_link.receive(frame)) {
            handle(frame);
        }
        mode_manager.poll(core_clock.tickMs());
        sendHealthWhenDue();
    }
}
