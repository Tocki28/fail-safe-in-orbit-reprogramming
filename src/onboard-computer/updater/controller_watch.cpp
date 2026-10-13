#include "controller_watch.hpp"

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
: silence_ms_(silence_ms), last_heard_ms_(now_ms), silent_(false), ever_heard_(false)
{
}

/**
 * @brief Records that a frame from the controller arrived.
 *
 * Logs the first time the controller is heard and the return after a silence. Clears the silent state.
 *
 * @param[in]    now_ms  current monotonic time, milliseconds
 * @return nothing
 */
void ControllerWatch::heard(uint64_t now_ms)
{
  if (silent_) {
    logging::line("controller heard again after %llu s of silence",
      static_cast<unsigned long long>((now_ms - last_heard_ms_) / kMsPerSecond));
  } else if (!ever_heard_) {
    logging::line("controller heard");
  }
  silent_ = false;
  ever_heard_ = true;
  last_heard_ms_ = now_ms;
}

/**
 * @brief Declares the controller silent once the silence limit has passed.
 *
 * Logs once per silence (REQ-061). REQ-061 also asks to try to bring it back; not in this piece
 * (header comment).
 *
 * @param[in]    now_ms  current monotonic time, milliseconds
 * @return nothing
 */
void ControllerWatch::check(uint64_t now_ms)
{
  if (silent_ || now_ms - last_heard_ms_ < silence_ms_) {
    return;
  }
  silent_ = true;
  // REQ-061 also asks to try to bring it back; not in this piece (header comment).
  logging::line("controller silent: no frame from the controller for %llu s (REQ-061)",
    static_cast<unsigned long long>((now_ms - last_heard_ms_) / kMsPerSecond));
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
