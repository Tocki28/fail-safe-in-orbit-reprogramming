#include "controller_watch.hpp"

#include "log.hpp"

namespace
{

constexpr uint64_t kMsPerSecond = 1000;

}  // namespace

// The watch starts as if the controller had just spoken, so a controller that never speaks
// is reported silent after the first silence_ms.
ControllerWatch::ControllerWatch(uint64_t silence_ms, uint64_t now_ms)
: silence_ms_(silence_ms), last_heard_ms_(now_ms), silent_(false), ever_heard_(false)
{
}

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

bool ControllerWatch::silent() const
{
  return silent_;
}
