/*
 * main.cpp — the controller's main firmware, step 2c: it starts, keeps the watchdog reloaded,
 * runs data link A, answers GET STATUS, receives firmware updates into the slot that is not
 * confirmed, and sends a HEALTH report every second.
 *
 * Commands handled (Interfaces Sec. 5.1):
 *   GET STATUS (0x25, no fields): RECEIVED, then DONE 0x00.
 *   UPLOAD BEGIN, CHUNK, END, CANCEL (0x01-0x04): update receiver (update-receiver/). A
 *     refusal on arrival is answered REFUSED only; otherwise RECEIVED, the work, then DONE 0x00
 *     or REFUSED with the reason (Interfaces Sec. 4.2, REQ-033, REQ-034).
 *   Anything else: REFUSED 0x06 only (Interfaces Sec. 4.2).
 * A frame not addressed to the controller, or not a command, is ignored.
 *
 * Not yet: it does not confirm itself in the boot state (step 2d). Linked for slot A or B
 * (Makefile: SLOT=A or B).
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

#include "boot_slot.hpp"
#include "boot_state.h"
#include "core_clock.hpp"
#include "frame.hpp"
#include "health_monitor.hpp"
#include "layout.h"
#include "stm32g4xx.h"
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
/* The largest frame the controller sends: a HEALTH report (24-byte payload). */
constexpr uint32_t kOutFrameBytes = kFrameHeaderBytes + kHealthReportBytes + kFrameCheckBytes;

static_assert(kHealthReportBytes >= kAnswerPayloadBytes, "kOutFrameBytes must hold an answer");

/* One frame to the onboard computer, which passes the ground's answers on (Interfaces
 * Sec. 4.2, Figure 4). Open: the Interfaces document does not say whether an answer to a
 * ground command carries Destination 0x01 or 0x03; 0x01 here awaits the owner's decision.
 *
 * C++: class FrameSender (C: main.c's send_frame, send_answer and the file-static
 * next_sequence). The sequence number can change only through send(). */
class FrameSender {
public:
    explicit constexpr FrameSender(UartLink &link) : link_(link) {}

    void send(FrameType type, const uint8_t *payload, uint16_t length)
    {
        std::array<uint8_t, kOutFrameBytes> bytes{};
        const Frame frame{
            type,
            Destination::kOnboard,
            0U,                /* flags */
            length,
            next_sequence_,
            0U,                /* expiry: 0 in answers and reports (Interfaces Sec. 4.0) */
            payload,
        };
        uint32_t size = buildFrame(frame, bytes.data(), kOutFrameBytes);

        next_sequence_++;
        /* A stalled transmitter cuts the frame short; the onboard computer drops it and,
         * missing answers, reports the link lost (REQ-081). Nothing more to do here in this
         * step. */
        (void)link_.send(bytes.data(), size);
    }

    void sendAnswer(uint32_t answered, AnswerKind kind, AnswerCode code)
    {
        std::array<uint8_t, kAnswerPayloadBytes> payload{};

        answerPayload(answered, kind, code, payload);
        send(FrameType::kAnswer, payload.data(), kAnswerPayloadBytes);
    }

private:
    UartLink &link_;
    /* Sequence number of the controller's next frame: one higher than its last (Interfaces
     * Sec. 4.0), from 1 at every start, answers and reports alike. Open for step 2b (REQ-065):
     * keeping it over restarts, and what follows 2^32 - 1. */
    uint32_t next_sequence_ = 1U;
};

/* The components. At namespace scope, not local to main(), because the interrupt handlers
 * below must reach them too; in the unnamed namespace, so no other file can. All but `sender`
 * start as all-zero bytes and sit in .bss, as the C file-statics did; `sender` holds a
 * reference (an address) and the first sequence number, so it sits in .data (8 bytes). */
CoreClock core_clock;
UartLink uart_link;
Watchdog watchdog;
UpdateReceiver updater;
HealthMonitor health_monitor;
FrameSender sender{uart_link};

/* RECEIVED goes out before the work starts: the onboard computer learns the command arrived
 * even when the work (an erase) takes half a second. */
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

bool isGetStatus(const Frame &command)
{
    return (command.length == kGetStatusBytes) && (command.payload[0] == kCmdGetStatus);
}

/* Step 2b puts the command checker (firmware/command-checker) in front of this, with the
 * checks on arrival that come before RECEIVED: sequence numbers (REQ-065; ALREADY DONE,
 * REQ-031), expiry (REQ-066, REQ-133), modes (REQ-027), arming (REQ-029, REQ-030), Flags and
 * scheduled commands (REQ-134). Here only the command itself is checked (REQ-028). */
void handle(const Frame &frame)
{
    if ((frame.destination != Destination::kController) || (frame.type != FrameType::kCommand)) {
        return;   /* Destination names who acts on a frame (Interfaces Sec. 4.0) */
    }
    if (UpdateReceiver::isUpload(frame.payload, frame.length)) {
        handleUpload(frame);
        return;
    }
    if (!isGetStatus(frame)) {
        sender.sendAnswer(frame.sequence, AnswerKind::kRefused, AnswerCode::kImpossible);
        return;
    }
    sender.sendAnswer(frame.sequence, AnswerKind::kReceived, kReceivedCode);
    sender.sendAnswer(frame.sequence, AnswerKind::kDone, AnswerCode::kSuccess);
}

/* One HEALTH report every kHealthPeriodMs (health-monitor/health_monitor.hpp). */
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
extern "C" void SysTick_Handler(void)
{
    core_clock.onTick();
}

/* The arrival time is read here and passed in. SysTick and USART2 have the same priority
 * (none is set, core_clock.cpp), so the tick cannot advance while this handler runs: the time
 * is the one the C handler read. */
extern "C" void USART2_IRQHandler(void)
{
    uart_link.onInterrupt(core_clock.tickMs());
}

/* main needs no extern "C": the language keeps its name as it is, and ST's startup calls it
 * as `bl main`. */
int main()
{
    /* Own vector table, also when started by a debugger instead of the bootloader. This build
     * runs only at the address it is linked for, so VTOR also names the slot it runs from. */
    SCB->VTOR = static_cast<uint32_t>(CODE_START);
    updater.init(UpdateReceiver::slotOf(SCB->VTOR));
    if (!CoreClock::start()) {
        /* No usable clock, so no usable link. Stop reloading the watchdog: it restarts the
         * chip within ~2 s (REQ-022). */
        for (;;) {
        }
    }
    CoreClock::startTick();
    uart_link.init();

    /* The one endless loop; the watchdog bounds every pass. Same order as the C version:
     * watchdog, then the link and its commands, then the HEALTH report. */
    for (;;) {
        Frame frame{};

        watchdog.reloadWhenDue(core_clock.tickMs());
        if (uart_link.receive(frame)) {
            handle(frame);
        }
        sendHealthWhenDue();
    }
}
