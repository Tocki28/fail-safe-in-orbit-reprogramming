/*
 * trial_watch.hpp — checks that the controller's trial ends in time (decision H of the
 * controller audit; Interfaces Sec. 5.4, REQ-037).
 *
 * The trial time is enforced by the firmware on trial. If that firmware hangs, nothing else
 * ends the trial. The onboard computer therefore times it too: a trial starts at the first
 * HEALTH report showing TRIAL or at an EVENT 0x0006 "trial started"; EVENT 0x0007 (firmware
 * confirmed), EVENT 0x0008 (firmware given up) or a HEALTH report showing another mode ends
 * it. If none arrives within config::kTrialMs + config::kTrialMarginMs, expired() says so once.
 *
 * Re-arming (finding F-2, test/fault-matrix.md; owner's approval 16 Oct 2026): a firmware that is
 * alive but never confirms and never gives up is reset once and started again by the bootloader,
 * with no EVENT 0x0006 (bench, test/ground/results/2026-10-15/trial-forever-decision-h.log). So
 * while the mode stays TRIAL, a HEALTH report whose failed-starts count differs from the last one
 * seen starts the full time plus margin again. The bootloader gives the trial up at its third
 * failed start, which bounds the number of resets.
  *
 * STATUS: decision H, accepted by the owner, 13 Oct 2026 (trial time 120 s stays the bench value
 * for TBD-trial; margin 30 s). Separable: this file, trial_watch.cpp, restart_runner.*, the
 * constants in config.hpp, Updater::trialWatch with its two calls, and the restart members of
 * Updater.
 *
 * What the updater does then: it runs the restart script (restore.sh reset, restart_runner.hpp)
 * and, once the controller was reset, sends the ground EVENT 0x0106 with detail 0x01 (updater.cpp,
 * onTrialOverrun and onRestartFinished; Interfaces Sec. 5.4).
 */
#ifndef FSIR_UPDATER_TRIAL_WATCH_HPP
#define FSIR_UPDATER_TRIAL_WATCH_HPP

#include <cstdint>

class TrialWatch
{
public:
  TrialWatch(uint64_t trial_ms, uint64_t margin_ms);

  // A HEALTH report arrived showing this mode and this failed-starts count (protocol.hpp,
  // report::kMode..., report::kOffsetFailedStarts).
  void onHealthMode(uint8_t mode, uint8_t failed_starts, uint64_t now_ms);

  // An EVENT arrived with this code (protocol.hpp, report::kEvent...).
  void onEvent(uint16_t code, uint64_t now_ms);

  // True once per overrun: a trial has run past its time plus margin without an end event.
  bool expired(uint64_t now_ms);

  // A trial is being timed.
  bool running() const;

private:
  void end();

  uint64_t trial_ms_;
  uint64_t margin_ms_;
  bool running_;
  bool fired_;   // the overrun was reported; wait for the trial to end before timing again
  uint64_t started_ms_;
  bool count_seen_;        // a HEALTH TRIAL report was seen since the trial began or ended
  uint8_t last_failed_starts_;   // failed-starts count of that report; valid if count_seen_
};

#endif  // FSIR_UPDATER_TRIAL_WATCH_HPP
