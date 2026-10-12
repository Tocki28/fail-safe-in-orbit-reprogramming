#include "test_image.hpp"

#include "bytes.hpp"
#include "crc32.hpp"
#include "protocol.hpp"

namespace test_image
{

/**
 * @brief Writes the 1,024-byte test image: 1,020 pattern bytes, then their CRC-32.
 *
 * The controller checks the last four bytes against the CRC-32 of the rest and keeps the image
 * in working memory [Interfaces, Sec. 5.1; REQ-139]. The CRC-32 is the one of the frame check
 * value [Interfaces, Sec. 4.0], from crc32.cpp, written little-endian like every multi-byte
 * field.
 *
 * @param[out]    out  the image, kBytes bytes
 * @return nothing
 */
void build(std::array<uint8_t, kBytes> & out)
{
  for (size_t i = 0; i < kPatternBytes; i++) {
    out[i] = static_cast<uint8_t>(i % kPatternModulus);
  }
  bytes::writeU32(&out[kPatternBytes], crc::crc32(out.data(), kPatternBytes));
}

}  // namespace test_image

/**
 * @brief Starts with no trial seen and nothing due.
 *
 * Before the first HEALTH report the onboard computer does not know the controller's mode, so
 * it sends nothing (Interfaces Sec. 5.4: the mode comes only in HEALTH).
 *
 * @return nothing (constructor)
 */
TestImageTrigger::TestImageTrigger()
: in_trial_(false), due_(false)
{
}

/**
 * @brief Takes the mode of one HEALTH report: the first TRIAL of a trial makes the image due,
 *        any other mode ends the trial.
 *
 * "Once per trial" needs the trial's edges: the first HEALTH report showing TRIAL starts it,
 * and a report showing another mode ends it, so a later trial gets its own image. A due image
 * not yet started when the trial ends is withdrawn: the controller no longer accepts it
 * [Interfaces, Sec. 5.1].
 *
 * @param[in]     mode  the HEALTH report's mode byte (protocol.hpp, report::kMode...)
 * @return nothing
 */
void TestImageTrigger::onHealthMode(uint8_t mode)
{
  if (mode != report::kModeTrial) {
    in_trial_ = false;
    due_ = false;
    return;
  }
  if (!in_trial_) {
    in_trial_ = true;
    due_ = true;
  }
}

/**
 * @brief Tells whether the test image is due.
 *
 * @return true: a trial is running and the image has not been started in it; false: otherwise
 */
bool TestImageTrigger::due() const
{
  return due_;
}

/**
 * @brief Records that sending the test image has started, so it is not due again in this trial.
 *
 * Marked at the start, not at the end: a refusal or no answer is not retried [owner's decision:
 * the controller gives the trial up itself when its trial time ends, and the ground sees the
 * events, Interfaces Sec. 5.4].
 *
 * @return nothing
 */
void TestImageTrigger::markStarted()
{
  due_ = false;
}
