/*
 * controller_watch.hpp — the controller watch (Architecture Sec. 6.0), bench part: notices a
 * silent controller (REQ-061) and keeps the schedule of the resets that try to bring it back.
 * Any good frame from the controller counts as a sign of life (answers and reports alike).
 * After 5 s without one (config.hpp, bench value; TBD in the requirements) it logs "controller
 * silent", once, and "controller heard again" when a frame arrives. check() and heard() tell
 * the caller when a silence starts and ends, so the updater can send EVENT 0x0101 and 0x0102
 * (owner decision 27, 15 Oct 2026; REQ-081, REQ-082).
 *
 * The schedule (owner decision 20, 15 Oct 2026; REQ-061): a reset is due at the start of the
 * silence and again every config::kSilentResetGapMs after the previous one, at most
 * config::kSilentResetsMax per silence. When the last reset was followed by a full gap of
 * silence, resetDue() answers kGiveUp once: the restore method awaits the owner (decision 20,
 * TBD), so nothing more is done until the controller is heard again. The watch only keeps the
 * schedule; the updater runs the resets (RestartRunner) and tells the watch when it started one.
 * Restoring the controller (REQ-124) is the restore program's work, another piece.
 */
#ifndef FSIR_UPDATER_CONTROLLER_WATCH_HPP
#define FSIR_UPDATER_CONTROLLER_WATCH_HPP

#include <cstdint>

class ControllerWatch
{
public:
  // What the schedule asks of the updater now.
  enum class ResetStep : uint8_t
  {
    kNone,     // nothing is due
    kReset,    // a reset is due: run it, then call resetRequested()
    kGiveUp,   // all resets used and a full gap passed: log it, then call gaveUp()
  };

  ControllerWatch(uint64_t silence_ms, uint64_t now_ms);

  // A good frame arrived from the controller. True when this ended a silence.
  bool heard(uint64_t now_ms);

  // Logs the start of a silence, once. True when this call started a silence.
  bool check(uint64_t now_ms);

  bool silent() const;

  // The next step of the reset schedule at this time; kNone while the controller is not silent.
  ResetStep resetDue(uint64_t now_ms) const;

  // The updater began a reset (or tried to) at this time: counts it, restarts the gap.
  void resetRequested(uint64_t now_ms);

  // The updater logged the give-up: nothing more is due until the controller is heard again.
  void gaveUp();

  // Resets counted in the silence now running (0 when not silent).
  uint32_t resetsDone() const;

private:
  uint64_t silence_ms_;
  uint64_t last_heard_ms_;
  bool silent_;
  bool ever_heard_;
  uint32_t resets_done_;
  uint64_t last_reset_ms_;
  bool gave_up_;
};

#endif  // FSIR_UPDATER_CONTROLLER_WATCH_HPP
