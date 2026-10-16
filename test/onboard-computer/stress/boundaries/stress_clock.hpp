/*
 * stress_clock.hpp — the test's hand on the updater's system clock (stress audit Part A.4).
 *
 * The updater reads the system clock through clocks::systemTimeS (clock.hpp). This audit links
 * fake_clock.cpp in place of src/onboard-computer/updater/clock.cpp, so that the onboard ground
 * time (system clock + offset, Updater::groundTimeS) can be set to an exact second. Nothing in
 * src/ is changed; the substitution is a link-time choice in this directory's Makefile.
 */
#ifndef FSIR_STRESS_BOUNDARIES_CLOCK_HPP
#define FSIR_STRESS_BOUNDARIES_CLOCK_HPP

#include <cstdint>

namespace stress_clock
{

// Sets the value clocks::systemTimeS returns, in seconds since 1970, signed 64-bit.
void setSystemTimeS(int64_t seconds);

}  // namespace stress_clock

#endif  // FSIR_STRESS_BOUNDARIES_CLOCK_HPP
