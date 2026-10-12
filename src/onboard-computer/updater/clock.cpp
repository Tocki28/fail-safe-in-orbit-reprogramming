#include "clock.hpp"

#include <ctime>

namespace
{

constexpr uint64_t kMsPerSecond = 1000;
constexpr uint64_t kNsPerMs = 1000000;

}  // namespace

namespace clocks
{

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

uint32_t onboardTimeS()
{
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
    return 0;
  }
  return static_cast<uint32_t>(now.tv_sec);
}

}  // namespace clocks
