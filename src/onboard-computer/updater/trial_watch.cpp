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
: trial_ms_(trial_ms), margin_ms_(margin_ms), running_(false), fired_(false), started_ms_(0),
  count_seen_(false), last_failed_starts_(0)
{
}

/**
 * @brief Takes one HEALTH report: TRIAL times the trial, a new failed-starts count re-arms, any
 *        other mode ends it.
 *
 * A HEALTH report showing another mode means the trial is over (confirmed, given up or never
 * began), so nothing is left to time and the count is forgotten. A further TRIAL report with the
 * same failed-starts count does not move the start, otherwise a hung firmware that keeps
 * reporting would never expire. A TRIAL report whose count differs from the last one seen means
 * the bootloader started the firmware again (finding F-2, test/fault-matrix.md, owner's approval
 * 16 Oct 2026): the full time plus margin starts again, also after an overrun was reported. The
 * first TRIAL report of a trial only records its count. The bootloader's limit of 3 failed starts
 * bounds the re-arms. Proposal (TBR), awaiting the owner's acceptance: any difference re-arms,
 * not only an increase (the contract says "differs"); a count that falls is not expected.
 *
 * @param[in]     mode            the HEALTH report's mode byte
 * @param[in]     failed_starts   the HEALTH report's failed-starts count (offset 20)
 * @param[in]     now_ms          current time, ms
 * @return nothing
 */
void TrialWatch::onHealthMode(uint8_t mode, uint8_t failed_starts, uint64_t now_ms)
{
  if (mode != report::kModeTrial) {
    end();
    return;
  }
  if (count_seen_ && failed_starts != last_failed_starts_) {
    running_ = true;
    fired_ = false;
    started_ms_ = now_ms;
  } else if (!running_ && !fired_) {
    running_ = true;
    started_ms_ = now_ms;
  }
  count_seen_ = true;
  last_failed_starts_ = failed_starts;
}

/**
 * @brief Takes one EVENT: 0x0006 (re)starts the timing, 0x0007 and 0x0008 end it.
 *
 * Other event codes do not touch the timing. A second EVENT 0x0006 is a restarted trial, which
 * has its full time again. The failed-starts count last seen is kept at 0x0006 and forgotten at
 * 0x0007 and 0x0008 (finding F-2, owner's approval 16 Oct 2026).
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
    end();
  }
}

/**
 * @brief Tells once whether the trial has run past its time plus margin.
 *
 * Stops timing when it returns true, so the overrun is reported once; a new trial (HEALTH TRIAL
 * after another mode, a new failed-starts count, or EVENT 0x0006) times again, but HEALTH reports
 * that keep showing TRIAL with the same count do not.
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

/**
 * @brief Ends the timing and forgets the failed-starts count.
 *
 * Called when the trial is over: EVENT 0x0007, EVENT 0x0008 or a HEALTH report with another mode
 * (finding F-2, test/fault-matrix.md, owner's approval 16 Oct 2026).
 *
 * @return nothing
 */
void TrialWatch::end()
{
  running_ = false;
  fired_ = false;
  count_seen_ = false;
  last_failed_starts_ = 0;
}
