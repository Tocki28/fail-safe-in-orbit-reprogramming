/*
 * frame_sender.hpp — every frame the controller sends to the onboard computer: answers (Interfaces
 * Sec. 4.2) and reports (Sec. 5.4), with the controller's own sequence numbers (Sec. 4.0).
 *
 * Moved here from main.cpp in step 2d: the mode manager sends EVENT reports and judges the
 * data-link trial test (REQ-139) from what this class saw, so it needs the sender outside
 * main.cpp. It also records two facts for that test: an answer went out whole, and a send
 * failed (UartLink::send returned false).
 *
 * No register access here: the link is reached through UartLink, so the host unit tests link a
 * fake UartLink (test/controller/unit/fake_uart_link.cpp) and read the bytes "sent".
 *
 * C++: class FrameSender (C: main.c's send_frame, send_answer and the file-static
 * next_sequence). The sequence number can change only through send().
 */
#ifndef FRAME_SENDER_HPP
#define FRAME_SENDER_HPP

#include <cstdint>

#include "frame.hpp"
#include "uart_link.hpp"

/* The largest payload the controller sends. Today: an EVENT report with 32 detail bytes,
 * 47 bytes (Interfaces Sec. 5.4); a HEALTH report is 24, an answer 6. Each sender of a payload
 * checks its own size against this bound with a static_assert (main.cpp, mode_manager.cpp). */
constexpr uint32_t kSendPayloadMax = 64U;
constexpr uint32_t kSendFrameMax = kFrameHeaderBytes + kSendPayloadMax + kFrameCheckBytes;

class FrameSender {
public:
    /** @brief A sender on `link`; its first frame carries sequence number 1. */
    explicit constexpr FrameSender(UartLink &link) : link_(link) {}

    /** @brief Build one frame around `payload` and send it whole. */
    void send(FrameType type, const uint8_t *payload, uint16_t length);

    /** @brief Send one 6-byte answer (Interfaces Sec. 4.2). */
    void sendAnswer(uint32_t answered, AnswerKind kind, AnswerCode code);

    /** @brief Wait, bounded, until the link has sent its last byte. */
    [[nodiscard]] bool waitSent();

    /** @brief True once an answer frame went out whole since start. */
    bool answerSent() const
    {
        return answer_sent_;
    }

    /** @brief True once any send since start failed (transmitter stalled). */
    bool sendFailed() const
    {
        return send_failed_;
    }

private:
    UartLink &link_;
    /* Sequence number of the controller's next frame: one higher than its last (Interfaces
     * Sec. 4.0), from 1 at every start, answers and reports alike. Open for step 2b (REQ-065):
     * keeping it over restarts, and what follows 2^32 - 1. */
    uint32_t next_sequence_ = 1U;
    bool answer_sent_ = false;   /* an answer frame left whole since start */
    bool send_failed_ = false;   /* a send since start returned false */
};

#endif /* FRAME_SENDER_HPP */
