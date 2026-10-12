/*
 * clock.hpp — the two times the updater uses.
 *
 * nowMs: milliseconds of a clock that only moves forward (CLOCK_MONOTONIC), for timeouts and
 * the controller watch. Setting the date does not move it.
 *
 * onboardTimeS: the onboard computer clock time put into every report (REQ-070), in seconds
 * of CLOCK_REALTIME (since 1970), cut to 32 bits. Bench limit: the Interfaces document gives
 * this field 4 bytes but no unit or epoch (Sec. 5.4), and on the bench nothing sets the board's
 * clock from ground time (REQ-072, REQ-073: clock keeper, not built).
 */
#ifndef FSIR_UPDATER_CLOCK_HPP
#define FSIR_UPDATER_CLOCK_HPP

#include <cstdint>

namespace clocks
{

uint64_t nowMs();

uint32_t onboardTimeS();

}  // namespace clocks

#endif  // FSIR_UPDATER_CLOCK_HPP
