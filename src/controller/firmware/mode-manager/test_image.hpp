/*
 * test_image.hpp — the test image of the trial tests (Interfaces Sec. 5.1, REQ-139): during a
 * trial the onboard computer sends it with UPLOAD BEGIN, CHUNK, END and CANCEL, image number
 * 0xFFFF. The controller keeps it in working memory, never in flash, and checks its check
 * value: its last four bytes are the CRC-32 (Interfaces Sec. 4.0) of the bytes before them,
 * little-endian (Sec. 4.0). Match: the test passes; mismatch: it fails.
 *
 * Why here and not in the update receiver: the update receiver writes the slot that is not
 * confirmed, behind its address barrier (update-receiver/update_receiver.hpp). The test image
 * writes nothing into either slot (Interfaces Sec. 5.1). Kept in its own class, no command of
 * image number 0xFFFF can reach a flash erase or write: the separation is in the structure, not
 * in a branch inside the receiver. Its result is a trial test, which the mode manager judges.
 * The payload is read by the receiver's own UpdateReceiver::parse, so the layout has one
 * reader.
 *
 * The mode check (TRIAL only) is the mode manager's (ModeManager::acceptTestImage); this class
 * checks the command's values, in the update receiver's two calls:
 *   accept()  the checks on arrival; a refusal here is answered REFUSED only (0x06);
 *   run()     the work, after RECEIVED; END answers DONE 0x00 or REFUSED 0x08.
 *
 * No hardware here: the same file compiles for the unit tests (test/controller/unit).
 */
#ifndef TEST_IMAGE_HPP
#define TEST_IMAGE_HPP

#include <array>
#include <cstdint>

#include "frame.hpp"
#include "update_receiver.hpp"

/* Image number of the test image (Interfaces Sec. 5.1). */
constexpr uint16_t kTestImageNumber = 0xFFFFU;
/* Largest test image the controller keeps: 1,024 bytes of working memory. Bench maximum; the
 * Interfaces document sets no size. */
constexpr uint32_t kTestImageMaxBytes = 1024U;
/* The check value at the end of the test image (Interfaces Sec. 5.1). */
constexpr uint32_t kTestImageCheckBytes = 4U;
/* Smallest test image: one byte and its check value. */
constexpr uint32_t kTestImageMinBytes = kTestImageCheckBytes + 1U;

enum class TestImageResult : uint8_t {
    kNotRun,   /* no END has run since start */
    kPassed,   /* the last END found the check value right */
    kFailed,   /* the last END found it wrong */
};

class TestImage {
public:
    /** @brief True if the command carries the test image's number, 0xFFFF. */
    [[nodiscard]] static bool isTestImage(const UpdateCommand &cmd);

    /** @brief Checks on arrival of one test-image command: kUpdateOk or 0x06. */
    AnswerCode accept(const UpdateCommand &cmd) const;

    /** @brief Run a command that accept() accepted: kUpdateOk, or 0x08 at END on a wrong check value. */
    AnswerCode run(const UpdateCommand &cmd);

    /** @brief The result of the last END. */
    TestImageResult result() const
    {
        return result_;
    }

private:
    AnswerCode runEnd();

    /* All members start as zero bytes (kIdle and kNotRun are the first enumerators), so the
     * object sits in .bss (main.cpp). */
    UpdatePhase phase_ = UpdatePhase::kIdle;
    TestImageResult result_ = TestImageResult::kNotRun;
    uint32_t length_ = 0U;     /* length from the last BEGIN */
    uint32_t received_ = 0U;   /* bytes received so far; never above length_ */
    std::array<uint8_t, kTestImageMaxBytes> bytes_{};
};

#endif /* TEST_IMAGE_HPP */
