#include "trial_watch.hpp"

#include "protocol.hpp"

/**
 * @brief Makes a trial watch with no trial running.
 *
 * @param[in]     trial_ms   trial time, ms (config::kTrialMs)
 * @param[in]     margin_ms  margin after it, ms (config::kTrialMarginMs)
 * @return nothing (constructor)
 */
TrialWatch::TrialWatch(uint64_t trial_ms, uint64_t margin_ms)
: trial_ms_(trial_ms), margin_ms_(margin_ms), running_(false), fired_(false), started_ms_(0)
{
}

/**
 * @brief Takes the mode of one HEALTH report: TRIAL starts the timing if none runs, any other mode ends it.
 *
 * A HEALTH report showing another mode means the trial is over (confirmed, given up or never
 * began), so nothing is left to time. A further TRIAL report in a trial already timed does not
 * move the start, otherwise a hung firmware that keeps reporting would never expire.
 *
 * @param[in]     mode    the HEALTH report's mode byte
 * @param[in]     now_ms  current time, ms
 * @return nothing
 */
void TrialWatch::onHealthMode(uint8_t mode, uint64_t now_ms)
{
  if (mode != report::kModeTrial) {
    running_ = false;
    fired_ = false;
    return;
  }
  if (!running_ && !fired_) {
    running_ = true;
    started_ms_ = now_ms;
  }
}

/**
 * @brief Takes one EVENT: 0x0006 (re)starts the timing, 0x0007 and 0x0008 end it.
 *
 * Other event codes do not touch the timing. A second EVENT 0x0006 is a restarted trial, which
 * has its full time again.
 *
 * @param[in]     code    the event code (Interfaces Sec. 5.4)
 * @param[in]     now_ms  current time, ms
 * @return nothing
 */
void TrialWatch::onEvent(uint16_t code, uint64_t now_ms)
{
  if (code == report::kEventTrialStarted) {
    running_ = true;
    fired_ = false;
    started_ms_ = now_ms;
  } else if (code == report::kEventFirmwareConfirmed || code == report::kEventFirmwareGivenUp) {
    running_ = false;
    fired_ = false;
  }
}

/**
 * @brief Tells once whether the trial has run past its time plus margin.
 *
 * Stops timing when it returns true, so the overrun is reported once; a new trial (HEALTH TRIAL
 * after another mode, or EVENT 0x0006) times again, but HEALTH reports that keep showing TRIAL do not.
 *
 * @param[in]     now_ms  current time, ms
 * @return true: the trial overran just now; false: no trial, or still inside its time
 */
bool TrialWatch::expired(uint64_t now_ms)
{
  if (!running_ || now_ms - started_ms_ <= trial_ms_ + margin_ms_) {
    return false;
  }
  running_ = false;
  fired_ = true;   // HEALTH reports still showing TRIAL must not start the timing again
  return true;
}

/**
 * @brief Tells whether a trial is being timed.
 *
 * @return true: timing; false: no trial
 */
bool TrialWatch::running() const
{
  return running_;
}
