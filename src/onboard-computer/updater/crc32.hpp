/*
 * crc32.hpp — the CRC-32 of the Interfaces document, Sec. 4.0: polynomial 0x4C11DB7, start
 * 0xFFFFFFFF, each byte least significant bit first, result inverted. The same value as zlib's
 * crc32 and as the controller's crc32.c (src/controller/common); this is the onboard
 * computer's own copy, not that file.
 */
#ifndef FSIR_UPDATER_CRC32_HPP
#define FSIR_UPDATER_CRC32_HPP

#include <cstddef>
#include <cstdint>

namespace crc
{

uint32_t crc32(const uint8_t * data, size_t size);

}  // namespace crc

#endif  // FSIR_UPDATER_CRC32_HPP
