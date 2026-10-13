/*
 * test_image.hpp — the test image of the controller's trial tests, and the rule for when the
 * onboard computer sends it (Interfaces Sec. 5.1, Sec. 5.4, Sec. 5.5).
 *
 * The test image (Sec. 5.1): UPLOAD BEGIN, UPLOAD CHUNKs and UPLOAD END with image number
 * 0xFFFF, accepted by the controller during a trial and for that image only. Its last four
 * bytes are the CRC-32 of the bytes before them; the controller keeps it in working memory,
 * checks that value and writes nothing into either slot [Interfaces, Sec. 5.1; REQ-139].
 *
 * Bench choices (the Interfaces document gives no size or content):
 *   - size 1,024 bytes: the controller's buffer holds at most 1,024 bytes (decided by the
 *     owner), which is four chunks of 256 bytes (Sec. 5.5);
 *   - content: 1,020 bytes of the pattern  byte[i] = i mod 251, then the CRC-32 of those
 *     1,020 bytes, little-endian as every multi-byte field (Sec. 4.0). 251 is prime and below
 *     256, so the pattern does not repeat at the 256-byte chunk boundaries: a chunk stored at
 *     the wrong offset, or twice, gives a different image and a wrong check value.
 *
 * The rule (TestImageTrigger): once per trial. A trial runs from the first HEALTH report that
 * shows mode TRIAL until a HEALTH report shows another mode. A restarted trial (a second EVENT
 * 0x0006 "trial started" with no other mode between) is a new trial and gets its image again. No retry (owner's
 * decision): the controller gives the trial up itself when its trial time ends, and the ground
 * sees its events (Sec. 5.4).
 */
#ifndef FSIR_UPDATER_TEST_IMAGE_HPP
#define FSIR_UPDATER_TEST_IMAGE_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace test_image
{

constexpr size_t kBytes = 1024;                          // bench choice, see above
constexpr size_t kCheckBytes = 4;                        // the CRC-32 at the end
constexpr size_t kPatternBytes = kBytes - kCheckBytes;   // 1,020
constexpr uint32_t kPatternModulus = 251;                // bench choice, see above

// Writes the test image into out (kBytes bytes).
void build(std::array<uint8_t, kBytes> & out);

}  // namespace test_image

class TestImageTrigger
{
public:
  TestImageTrigger();

  // A HEALTH report arrived showing this mode (protocol.hpp, report::kMode...).
  void onHealthMode(uint8_t mode);

  // An EVENT 0x0006 "trial started" arrived. The first one of a trial adds nothing (the HEALTH
  // report showing TRIAL starts the trial); a further one in the same trial is a restarted
  // trial and makes the image due again.
  void onTrialStartedEvent();

  // The test image is due: a trial is running and the image has not been started in it.
  bool due() const;

  // The updater has started sending the test image; it is not due again in this trial.
  void markStarted();

private:
  bool in_trial_;
  bool due_;
  bool event_seen_;   // an EVENT 0x0006 arrived in this trial
};

#endif  // FSIR_UPDATER_TEST_IMAGE_HPP
