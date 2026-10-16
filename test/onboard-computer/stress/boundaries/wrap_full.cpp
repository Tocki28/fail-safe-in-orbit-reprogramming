/*
 * wrap_full.cpp — the 2^32 wrap of data link A's sequence counter, run in full on DataLink itself
 * (stress audit Part A.4). Not part of `make run`; `make wrap-full` builds and runs it.
 *
 * Sends 2^32 + 2 commands through a DataLink over a transport that discards the bytes, releasing
 * each at once, and checks every sequence number it carries: 1, 2, ... 2^32 - 1, then 1 again
 * (data_link.cpp:81-82; Interfaces Sec. 4.0: 1 to 2^32 - 1). 0 must never appear.
 */
#include <chrono>
#include <cstdint>
#include <cstdio>

#include "data_link.hpp"

namespace
{

// Number of commands sent: one full turn of the counter, 2^32 - 1 numbers, plus 3 more to see
// the numbers after the wrap (1, 2 and 3).
constexpr uint64_t kCommands = static_cast<uint64_t>(UINT32_MAX) + 3U;

// A progress line is printed every this many commands: 2^28. Test choice.
constexpr uint64_t kProgressEvery = 1ULL << 28;

// A transport that takes every byte and returns nothing.
class NullTransport final : public Transport
{
public:
  /**
   * @brief Discards the bytes.
   *
   * @param[in]     data  the bytes (not read)
   * @param[in]     size  number of bytes (not read)
   * @return true, always
   */
  bool sendBytes(const uint8_t * data, size_t size) override
  {
    (void)data;
    (void)size;
    return true;
  }

  /**
   * @brief Gives no bytes.
   *
   * @param[out]    buffer    not written
   * @param[in]     capacity  not used
   * @return 0, always
   */
  long receiveBytes(uint8_t * buffer, size_t capacity) override
  {
    (void)buffer;
    (void)capacity;
    return 0;
  }

  /**
   * @brief Gives no descriptor.
   *
   * @return -1, always
   */
  int pollFd() const override
  {
    return -1;
  }
};

}  // namespace

/**
 * @brief Sends 2^32 + 2 commands through a DataLink and checks each sequence number.
 *
 * @return 0 when every number was as expected; 1 on the first difference
 */
int main()
{
  NullTransport transport;
  DataLink link(transport);
  const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  const uint8_t payload = 0x25;
  uint32_t expected = 1;
  for (uint64_t i = 0; i < kCommands; i++) {
    uint32_t sequence = 0;
    const uint32_t announced = link.nextSequence();
    if (!link.sendCommand(&payload, 1, UINT32_MAX, 1, 0, sequence) || sequence != expected ||
        announced != expected || sequence == 0) {
      std::printf("FINDING  wrap-full | command %llu | expected: sequence %u | actual: sequence "
        "%u (nextSequence said %u)\n", static_cast<unsigned long long>(i), expected, sequence,
        announced);
      return 1;
    }
    link.release();
    expected = (expected == UINT32_MAX) ? 1U : expected + 1U;
    if (i % kProgressEvery == 0) {
      const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
      std::printf("INFO     wrap-full | command %llu sent | measured: %.0f s elapsed\n",
        static_cast<unsigned long long>(i), seconds);
      (void)std::fflush(stdout);
    }
  }
  const double seconds = std::chrono::duration<double>(
    std::chrono::steady_clock::now() - start).count();
  std::printf("OK       wrap-full | %llu commands through one DataLink, 2^32 - 1 numbers and the "
    "wrap | expected: 1..2^32-1 then 1 again, never 0 [data_link.cpp:81-82] | actual: as "
    "expected; next number %u; %.0f s\n", static_cast<unsigned long long>(kCommands), expected,
    seconds);
  return 0;
}
