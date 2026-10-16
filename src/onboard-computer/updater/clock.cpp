#include "clock.hpp"

#include <ctime>

namespace
{

constexpr uint64_t kMsPerSecond = 1000;
constexpr uint64_t kNsPerMs = 1000000;

}  // namespace

namespace clocks
{

/**
 * @brief Reads the monotonic clock in milliseconds.
 *
 * CLOCK_MONOTONIC cannot fail with a valid pointer (clock_gettime(2)); on failure the time
 * reads 0 and timeouts run late, never early.
 * @return milliseconds since an arbitrary start; 0 if the clock could not be read
 */
uint64_t nowMs()
{
  timespec now{};
  // CLOCK_MONOTONIC cannot fail with a valid pointer (clock_gettime(2)); on failure the time
  // reads 0 and timeouts run late, never early.
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(now.tv_sec) * kMsPerSecond +
         static_cast<uint64_t>(now.tv_nsec) / kNsPerMs;
}

/**
 * @brief Reads the onboard real-time clock in whole seconds.
 *
 * Uses CLOCK_REALTIME; the fractional second is dropped.
 * @return seconds of the real-time clock, truncated to 32 bits; 0 if the clock could not be read
 */
uint32_t onboardTimeS()
{
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
    return 0;
  }
  return static_cast<uint32_t>(now.tv_sec);
}

/**
 * @brief Reads the system real-time clock in whole seconds, signed 64-bit.
 *
 * Uses CLOCK_REALTIME; the fractional second is dropped. The base of the onboard ground time
 * (owner decision 25, 15 Oct 2026).
 * @return seconds since 1970; 0 if the clock could not be read (then the offset alone shows)
 */
int64_t systemTimeS()
{
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
    return 0;
  }
  return static_cast<int64_t>(now.tv_sec);
}

}  // namespace clocks
