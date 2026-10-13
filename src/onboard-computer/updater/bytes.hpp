/*
 * bytes.hpp — little-endian reads and writes. Every multi-byte field of the frame, the answer,
 * the reports and the image header is little-endian (Interfaces Sec. 4.0). Written byte by
 * byte, so the result does not depend on the processor's own byte order.
 */
#ifndef FSIR_UPDATER_BYTES_HPP
#define FSIR_UPDATER_BYTES_HPP

#include <cstdint>

namespace bytes
{

/**
 * @brief Reads a little-endian 16-bit number.
 *
 * Little-endian is the byte order of the frame layer and the report fields (Interfaces Sec. 4, Sec. 5.4).
 *
 * @param[in]     b  two bytes, least significant first
 * @return the number
 */
inline uint16_t readU16(const uint8_t * b)
{
  return static_cast<uint16_t>(static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8));
}

/**
 * @brief Reads a little-endian 32-bit number.
 *
 * Each byte is widened before the shift so no promoted type overflows (code rule B5).
 *
 * @param[in]     b  four bytes, least significant first
 * @return the number
 */
inline uint32_t readU32(const uint8_t * b)
{
  return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

/**
 * @brief Writes a 16-bit number little-endian.
 *
 * @param[out]    b      room for two bytes
 * @param[in]     value  the number
 * @return nothing
 */
inline void writeU16(uint8_t * b, uint16_t value)
{
  b[0] = static_cast<uint8_t>(value);
  b[1] = static_cast<uint8_t>(value >> 8);
}

/**
 * @brief Writes a 32-bit number little-endian.
 *
 * @param[out]    b      room for four bytes
 * @param[in]     value  the number
 * @return nothing
 */
inline void writeU32(uint8_t * b, uint32_t value)
{
  b[0] = static_cast<uint8_t>(value);
  b[1] = static_cast<uint8_t>(value >> 8);
  b[2] = static_cast<uint8_t>(value >> 16);
  b[3] = static_cast<uint8_t>(value >> 24);
}

}  // namespace bytes

#endif  // FSIR_UPDATER_BYTES_HPP
