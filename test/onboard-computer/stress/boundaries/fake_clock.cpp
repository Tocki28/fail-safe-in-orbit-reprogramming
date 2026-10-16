/*
 * fake_clock.cpp — replaces src/onboard-computer/updater/clock.cpp in the boundary stress binary.
 *
 * nowMs is the real CLOCK_MONOTONIC (the restart runner and the log stamps use it). The two
 * wall-clock readers return a value the test sets (stress_clock.hpp), so that a boundary of the
 * expiry check can be hit to the second. Same signatures as clock.hpp.
 */
#include "clock.hpp"

#include <ctime>

#include "stress_clock.hpp"

namespace
{

// Milliseconds per second and nanoseconds per millisecond: unit conversions, the same two
// constants as src/onboard-computer/updater/clock.cpp:8-9.
constexpr uint64_t kMsPerSecond = 1000;
constexpr uint64_t kNsPerMs = 1000000;

// The fake system clock, seconds since 1970. Zero until the test sets it.
int64_t g_system_s = 0;

}  // namespace

namespace stress_clock
{

/**
 * @brief Sets the fake system clock.
 *
 * The value stays until the next call. Used to put the onboard ground time on an exact second.
 *
 * @param[in]     seconds  seconds since 1970, signed 64-bit
 * @return nothing
 */
void setSystemTimeS(int64_t seconds)
{
  g_system_s = seconds;
}

}  // namespace stress_clock

namespace clocks
{

/**
 * @brief Reads the real monotonic clock in milliseconds.
 *
 * Same behaviour as clock.cpp: 0 if the clock cannot be read.
 *
 * @return milliseconds of CLOCK_MONOTONIC; 0 on failure
 */
uint64_t nowMs()
{
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(now.tv_sec) * kMsPerSecond +
         static_cast<uint64_t>(now.tv_nsec) / kNsPerMs;
}

/**
 * @brief Gives the fake system clock cut to 32 bits.
 *
 * @return low 32 bits of the fake system clock, seconds
 */
uint32_t onboardTimeS()
{
  return static_cast<uint32_t>(static_cast<uint64_t>(g_system_s));
}

/**
 * @brief Gives the fake system clock.
 *
 * @return the value last set with stress_clock::setSystemTimeS, seconds since 1970
 */
int64_t systemTimeS()
{
  return g_system_s;
}

}  // namespace clocks
