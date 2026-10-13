/*
 * frame_sender.cpp — see frame_sender.hpp.
 */
#include "frame_sender.hpp"

#include <array>

/**
 * @brief Build one frame around `payload` (Interfaces Sec. 4.0, Figure 2) and send it whole.
 *
 * Every frame goes to Destination 0x01, the onboard computer, which passes the ground's answers
 * on (Interfaces Sec. 4.2, Figure 4). Open: the Interfaces document does not say whether an
 * answer to a ground command carries Destination 0x01 or 0x03; 0x01 here awaits the owner's
 * decision. Expiry is 0 in answers and reports (Interfaces Sec. 4.0).
 *
 * A stalled transmitter cuts the frame short; the onboard computer drops it on its check value
 * and, missing answers, reports the link lost (REQ-081). The failure is recorded for the
 * data-link trial test (REQ-139): sendFailed() turns true and stays true.
 *
 * @param[in]     type     frame Type: answer or report
 * @param[in]     payload  the payload bytes
 * @param[in]     length   payload bytes, at most kSendPayloadMax
 */
void FrameSender::send(FrameType type, const uint8_t *payload, uint16_t length)
{
    std::array<uint8_t, kSendFrameMax> bytes{};
    const Frame frame{
        type,
        Destination::kOnboard,
        0U,                /* flags */
        length,
        next_sequence_,
        0U,                /* expiry: 0 in answers and reports (Interfaces Sec. 4.0) */
        payload,
    };
    uint32_t size = buildFrame(frame, bytes.data(), kSendFrameMax);

    if (size == 0U) {
        /* Payload longer than kSendPayloadMax: a defect of the caller, which the static_asserts
         * at every caller rule out. Counted as a failed send, the safe direction: during a
         * trial it fails the data-link test and the trial is given up. */
        send_failed_ = true;
        return;
    }
    next_sequence_++;
    if (!link_.send(bytes.data(), size)) {
        send_failed_ = true;
        return;
    }
    if (type == FrameType::kAnswer) {
        answer_sent_ = true;
    }
}

/**
 * @brief Send one answer: the sequence number answered, the kind and the code (Interfaces
 * Sec. 4.2).
 *
 * @param[in]     answered  sequence number of the command answered
 * @param[in]     kind      RECEIVED, DONE, REFUSED or ALREADY DONE
 * @param[in]     code      0x00 for DONE on success, or the refusal reason
 */
void FrameSender::sendAnswer(uint32_t answered, AnswerKind kind, AnswerCode code)
{
    std::array<uint8_t, kAnswerPayloadBytes> payload{};

    answerPayload(answered, kind, code, payload);
    send(FrameType::kAnswer, payload.data(), kAnswerPayloadBytes);
}

/**
 * @brief Wait, bounded, until the link has sent its last byte (UartLink::waitSent).
 *
 * @return true if the link finished in time, false if it stalled
 */
bool FrameSender::waitSent()
{
    return link_.waitSent();
}
