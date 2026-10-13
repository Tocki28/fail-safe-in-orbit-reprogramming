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
 * STATUS: decision H, Proposal (TBR), awaiting the owner's acceptance. Separable: this file,
 * trial_watch.cpp, the two constants in config.hpp, kEventRestart in protocol.hpp, and
 * Updater::trialWatch with its two calls.
 *
 * What the updater does then: the restart path of the controller (REQ-061, REQ-124) is not
 * built yet, so it only logs and reports to the ground (updater.cpp, onTrialOverrun).
 */
#ifndef FSIR_UPDATER_TRIAL_WATCH_HPP
#define FSIR_UPDATER_TRIAL_WATCH_HPP

#include <cstdint>

class TrialWatch
{
public:
  TrialWatch(uint64_t trial_ms, uint64_t margin_ms);

  // A HEALTH report arrived showing this mode (protocol.hpp, report::kMode...).
  void onHealthMode(uint8_t mode, uint64_t now_ms);

  // An EVENT arrived with this code (protocol.hpp, report::kEvent...).
  void onEvent(uint16_t code, uint64_t now_ms);

  // True once per overrun: a trial has run past its time plus margin without an end event.
  bool expired(uint64_t now_ms);

  // A trial is being timed.
  bool running() const;

private:
  uint64_t trial_ms_;
  uint64_t margin_ms_;
  bool running_;
  bool fired_;   // the overrun was reported; wait for the trial to end before timing again
  uint64_t started_ms_;
};

#endif  // FSIR_UPDATER_TRIAL_WATCH_HPP
