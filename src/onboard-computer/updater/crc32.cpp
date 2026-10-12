#include "crc32.hpp"

namespace
{

// Start value and final inversion (Interfaces Sec. 4.0).
constexpr uint32_t kInitial = 0xFFFFFFFFU;
constexpr uint32_t kFinalXor = 0xFFFFFFFFU;
// 0x4C11DB7 with its bits in reverse order: taking each byte least significant bit first is
// the same as shifting right with the bit-reversed polynomial.
constexpr uint32_t kPolyReversed = 0xEDB88320U;
constexpr unsigned kBitsPerByte = 8;

}  // namespace

namespace crc
{

/**
 * @brief Computes the CRC-32 of a byte block.
 *
 * Bit by bit, no table: about 8 passes per byte, ~330 000 for the largest image (40 KB). The
 * two signature checks that follow cost far more, so a table would save nothing that matters.
 * Start value, bit order and final inversion as in Interfaces Sec. 4.0.
 *
 * @param[in]    data  bytes to check
 * @param[in]    size  number of bytes
 * @return CRC-32 value
 */
uint32_t crc32(const uint8_t * data, size_t size)
{
  uint32_t value = kInitial;
  for (size_t i = 0; i < size; i++) {
    value ^= data[i];
    for (unsigned bit = 0; bit < kBitsPerByte; bit++) {
      // If the bit shifted out is 1, the polynomial divides in at this step.
      uint32_t divide = ((value & 1U) != 0U) ? kPolyReversed : 0U;
      value = (value >> 1) ^ divide;
    }
  }
  return value ^ kFinalXor;
}

}  // namespace crc
