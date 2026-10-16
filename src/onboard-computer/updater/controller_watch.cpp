#include "controller_watch.hpp"

#include "config.hpp"
#include "log.hpp"

namespace
{

constexpr uint64_t kMsPerSecond = 1000;

}  // namespace

/**
 * @brief Creates a watch on the controller's frames.
 *
 * The watch starts as if the controller had just spoken, so a controller that never speaks
 * is reported silent after the first silence_ms.
 *
 * @param[in]    silence_ms  silence limit, milliseconds
 * @param[in]    now_ms  current monotonic time, milliseconds
 * @return nothing (constructor)
 */
ControllerWatch::ControllerWatch(uint64_t silence_ms, uint64_t now_ms)
: silence_ms_(silence_ms), last_heard_ms_(now_ms), silent_(false), ever_heard_(false),
  resets_done_(0), last_reset_ms_(0), gave_up_(false)
{
}

/**
 * @brief Records that a frame from the controller arrived.
 *
 * Logs the first time the controller is heard and the return after a silence. Clears the silent
 * state and the reset schedule: the reset count goes back to 0 and no further reset is due
 * (owner decision 20, 15 Oct 2026).
 *
 * @param[in]    now_ms  current monotonic time, milliseconds
 * @return true when this frame ended a silence (the updater sends EVENT 0x0102); false otherwise
 */
bool ControllerWatch::heard(uint64_t now_ms)
{
  const bool ended = silent_;
  if (silent_) {
    logging::line("controller heard again after %llu s of silence",
      static_cast<unsigned long long>((now_ms - last_heard_ms_) / kMsPerSecond));
  } else if (!ever_heard_) {
    logging::line("controller heard");
  }
  silent_ = false;
  resets_done_ = 0;
  gave_up_ = false;
  ever_heard_ = true;
  last_heard_ms_ = now_ms;
  return ended;
}

/**
 * @brief Declares the controller silent once the silence limit has passed.
 *
 * Logs once per silence (REQ-061). The reset count of the new silence starts at 0. Bringing the
 * controller back is the updater's work, driven by resetDue().
 *
 * @param[in]    now_ms  current monotonic time, milliseconds
 * @return true when this call started a silence (the updater sends EVENT 0x0101 and resets the
 *         controller); false otherwise
 */
bool ControllerWatch::check(uint64_t now_ms)
{
  if (silent_ || now_ms - last_heard_ms_ < silence_ms_) {
    return false;
  }
  silent_ = true;
  resets_done_ = 0;
  gave_up_ = false;
  logging::line("controller silent: no frame from the controller for %llu s (REQ-061)",
    static_cast<unsigned long long>((now_ms - last_heard_ms_) / kMsPerSecond));
  return true;
}

/**
 * @brief Tells whether the controller is currently considered silent (REQ-061).
 *
 * @return true: no frame within the silence limit; false: heard recently
 */
bool ControllerWatch::silent() const
{
  return silent_;
}

/**
 * @brief Says what the reset schedule asks for at this time (owner decision 20, 15 Oct 2026).
 *
 * The first reset is due at the start of the silence. Proposal (TBR), awaiting the owner's
 * acceptance: at the start, not 60 s into it, and the gap is counted from the moment the previous
 * reset was requested, not from its end. Each next one is due config::kSilentResetGapMs after the
 * previous, up to config::kSilentResetsMax. After the last, a further gap of silence gives kGiveUp.
 * Pure: it changes nothing, so a due reset that has to wait (runner busy) is asked again.
 *
 * @param[in]    now_ms  current monotonic time, milliseconds
 * @return kNone when not silent, after the give-up, or between resets; else kReset or kGiveUp
 */
ControllerWatch::ResetStep ControllerWatch::resetDue(uint64_t now_ms) const
{
  if (!silent_ || gave_up_) {
    return ResetStep::kNone;
  }
  if (resets_done_ == 0) {
    return ResetStep::kReset;
  }
  if (now_ms - last_reset_ms_ < config::kSilentResetGapMs) {
    return ResetStep::kNone;
  }
  return (resets_done_ < config::kSilentResetsMax) ? ResetStep::kReset : ResetStep::kGiveUp;
}

/**
 * @brief Counts a reset the updater began and starts the gap before the next one.
 *
 * Called when the reset script was started, and also when it could not be started (not
 * configured, fork failed): Proposal (TBR), awaiting the owner's acceptance, a failed attempt
 * uses one of the kSilentResetsMax, so a missing script is retried at the gap, not every tick.
 *
 * @param[in]    now_ms  current monotonic time, milliseconds
 * @return nothing
 */
void ControllerWatch::resetRequested(uint64_t now_ms)
{
  if (resets_done_ < config::kSilentResetsMax) {
    ++resets_done_;
  }
  last_reset_ms_ = now_ms;
}

/**
 * @brief Records that the give-up was logged; resetDue() answers kNone until the next silence.
 *
 * @return nothing
 */
void ControllerWatch::gaveUp()
{
  gave_up_ = true;
}

/**
 * @brief Tells how many resets the silence now running has used.
 *
 * @return the count, 0 to config::kSilentResetsMax; 0 when the controller is not silent
 */
uint32_t ControllerWatch::resetsDone() const
{
  return resets_done_;
}
