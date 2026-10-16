/*
 * clock.hpp — the two times the updater uses.
 *
 * nowMs: milliseconds of a clock that only moves forward (CLOCK_MONOTONIC), for timeouts and
 * the controller watch. Setting the date does not move it.
 *
 * systemTimeS: the system clock (CLOCK_REALTIME) in whole seconds since 1970, signed 64-bit. The
 * onboard ground time is this plus the stored offset (clock_record.hpp, owner decision 25,
 * 15 Oct 2026; Updater::groundTimeS); it stamps every report (REQ-070) and is the clock for
 * expiry checks (REQ-066). The system clock itself is never set (REQ-073).
 *
 * onboardTimeS: the system clock alone, in seconds, cut to 32 bits, without the offset. Since
 * decision 25 the updater stamps reports with the ground time instead; this stays as the raw
 * system clock.
 */
#ifndef FSIR_UPDATER_CLOCK_HPP
#define FSIR_UPDATER_CLOCK_HPP

#include <cstdint>

namespace clocks
{

uint64_t nowMs();

uint32_t onboardTimeS();

int64_t systemTimeS();

}  // namespace clocks

#endif  // FSIR_UPDATER_CLOCK_HPP
