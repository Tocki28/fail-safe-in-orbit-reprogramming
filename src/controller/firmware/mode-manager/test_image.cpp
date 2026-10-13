/*
 * test_image.cpp — see test_image.hpp.
 *
 * The offset rules are the update receiver's (update-receiver/update_receiver.cpp, chunkFits):
 * a CHUNK starts at the next byte not yet received and stays inside the length from BEGIN;
 * anything else is refused 0x06. Not taken over: the receiver's rule that every CHUNK but the
 * last is a whole number of 8-byte flash words. It exists because a written flash word cannot
 * be written again [RM0440 Rev 9, Sec. 5.3.7, p. 196]; the test image is in RAM.
 */
#include "test_image.hpp"

#include <cstring>

#include "crc32.h"
#include "image_check.h"

/**
 * @brief Returns true if the command carries image number 0xFFFF, the test image (Interfaces
 * Sec. 5.1).
 *
 * @param[in]     cmd  an upload command read by UpdateReceiver::parse
 * @return true for the test image, false for any other image number
 */
bool TestImage::isTestImage(const UpdateCommand &cmd)
{
    return cmd.image_number == kTestImageNumber;
}

/**
 * @brief The checks on arrival of one test-image command; any refusal is 0x06 (REQ-028).
 *
 * BEGIN: length from 5 to 1,024 bytes, accepted in any phase (a BEGIN restarts the transfer,
 * as in the update receiver). CHUNK: a transfer is open, the offset is the next byte expected,
 * the data stays inside the length. END: a transfer is open and every byte has arrived.
 * CANCEL: a transfer was begun (open or closed by END).
 *
 * All four UploadId values have their own case (-Wswitch-enum, common/build.mk). The default is
 * for a value outside the enum (a corrupted variable; MISRA C:2012 Rule 16.4): refused 0x06.
 *
 * @param[in]     cmd  the command, read by UpdateReceiver::parse
 * @return kUpdateOk if accepted; AnswerCode::kImpossible (0x06) otherwise
 */
AnswerCode TestImage::accept(const UpdateCommand &cmd) const
{
    if (!isTestImage(cmd)) {
        return AnswerCode::kImpossible;
    }
    switch (cmd.id) {
    case UploadId::kBegin:
        return ((cmd.value >= kTestImageMinBytes) && (cmd.value <= kTestImageMaxBytes))
                   ? kUpdateOk
                   : AnswerCode::kImpossible;
    case UploadId::kChunk:
        /* received_ never exceeds length_, so the subtraction cannot wrap. */
        return ((phase_ == UpdatePhase::kReceiving) && (cmd.value == received_) &&
                (cmd.data_bytes <= (length_ - received_)))
                   ? kUpdateOk
                   : AnswerCode::kImpossible;
    case UploadId::kEnd:
        return ((phase_ == UpdatePhase::kReceiving) && (received_ == length_))
                   ? kUpdateOk
                   : AnswerCode::kImpossible;
    case UploadId::kCancel:
        return (phase_ != UpdatePhase::kIdle) ? kUpdateOk : AnswerCode::kImpossible;
    default:
        return AnswerCode::kImpossible;   /* outside the enum only: refuse */
    }
}

/**
 * @brief Compares the check value at the end of the test image with the CRC-32 of the bytes
 * before it (Interfaces Sec. 5.1; CRC-32 as Sec. 4.0, common/crc32.c), and records the result.
 *
 * The transfer is closed whatever the result; a new BEGIN starts another.
 *
 * @return kUpdateOk if they match (test passed); AnswerCode::kDamaged (0x08) if not (test
 *         failed)
 */
AnswerCode TestImage::runEnd()
{
    /* accept() let END through only with length_ >= 5 bytes, all received. */
    uint32_t covered = length_ - kTestImageCheckBytes;
    uint32_t stored = image_read_u32(&bytes_[covered]);

    phase_ = UpdatePhase::kClosed;
    if (crc32_compute(bytes_.data(), covered) == stored) {
        result_ = TestImageResult::kPassed;
        return kUpdateOk;
    }
    result_ = TestImageResult::kFailed;
    return AnswerCode::kDamaged;
}

/**
 * @brief Runs a command that accept() accepted.
 *
 * BEGIN opens a transfer of the announced length; CHUNK copies its data at the next byte;
 * END checks the check value; CANCEL forgets the transfer. A BEGIN or CANCEL leaves an earlier
 * END's result as it is: only END sets it. The CHUNK bound is checked again here, against the
 * buffer itself, so no command can write past it even if a caller skipped accept().
 *
 * All four UploadId values have their own case (-Wswitch-enum). The default is for a value
 * outside the enum (a corrupted variable; MISRA C:2012 Rule 16.4): nothing runs, 0x06.
 *
 * @param[in]     cmd  the command
 * @return kUpdateOk; at END, AnswerCode::kDamaged (0x08) on a wrong check value; 0x06 for a
 *         command accept() would have refused
 */
AnswerCode TestImage::run(const UpdateCommand &cmd)
{
    switch (cmd.id) {
    case UploadId::kBegin:
        length_ = cmd.value;
        received_ = 0U;
        phase_ = UpdatePhase::kReceiving;
        return kUpdateOk;
    case UploadId::kChunk:
        if ((cmd.data_bytes > (kTestImageMaxBytes - received_)) || (cmd.data == nullptr)) {
            return AnswerCode::kImpossible;
        }
        std::memcpy(&bytes_[received_], cmd.data, cmd.data_bytes);
        received_ += cmd.data_bytes;
        return kUpdateOk;
    case UploadId::kEnd:
        return runEnd();
    case UploadId::kCancel:
        phase_ = UpdatePhase::kIdle;
        received_ = 0U;
        return kUpdateOk;
    default:
        return AnswerCode::kImpossible;   /* outside the enum only: accept() takes these four */
    }
}
