/*
 * controller_watch.hpp — the controller watch (Architecture Sec. 6.0), bench part: notices a
 * silent controller (REQ-061). Any good frame from the controller counts as a sign of life
 * (answers and reports alike). After 5 s without one (config.hpp, bench value; TBD in the
 * requirements) it logs "controller silent", once, and "controller heard again" when a frame
 * arrives. It does nothing else yet: bringing the controller back (restart, restore) is the
 * restore program's work (REQ-061, REQ-124), another piece.
 */
#ifndef FSIR_UPDATER_CONTROLLER_WATCH_HPP
#define FSIR_UPDATER_CONTROLLER_WATCH_HPP

#include <cstdint>

class ControllerWatch
{
public:
  ControllerWatch(uint64_t silence_ms, uint64_t now_ms);

  // A good frame arrived from the controller.
  void heard(uint64_t now_ms);

  // Logs the start of a silence, once.
  void check(uint64_t now_ms);

  bool silent() const;

private:
  uint64_t silence_ms_;
  uint64_t last_heard_ms_;
  bool silent_;
  bool ever_heard_;
};

#endif  // FSIR_UPDATER_CONTROLLER_WATCH_HPP
