/*
 * crc32.c — bit-by-bit CRC-32, no table: 20 lines anyone can check against the definition.
 * Cost: about 8 loop passes per byte, ~10 ms for a full 40 KB slot at 170 MHz; the two
 * signature checks that follow take longer, so a table would not change the boot time much.
 */
#include "crc32.h"

/* Start value and final inversion (Interfaces Sec. 4.0). */
#define CRC32_INITIAL      0xFFFFFFFFU
#define CRC32_FINAL_XOR    0xFFFFFFFFU
/* 0x4C11DB7 with its bits in reverse order. Taking each byte least significant bit first is
 * the same as shifting right with the bit-reversed polynomial (Interfaces Sec. 4.0). */
#define CRC32_POLY_REVERSED 0xEDB88320U
#define BITS_PER_BYTE      8U

/**
 * @brief Returns the CRC-32 of the size bytes at data.
 *
 * @param[in]     data  first byte
 * @param[in]     size  number of bytes
 * @return CRC-32 of the bytes
 */
uint32_t crc32_compute(const uint8_t *data, uint32_t size)
{
    uint32_t crc = CRC32_INITIAL;

    for (uint32_t i = 0U; i < size; i++) {
        crc ^= data[i];
        for (uint32_t bit = 0U; bit < BITS_PER_BYTE; bit++) {
            /* If the bit shifted out is 1, the polynomial divides in at this step. */
            uint32_t divide = (crc & 1U) != 0U ? CRC32_POLY_REVERSED : 0U;
            crc = (crc >> 1) ^ divide;
        }
    }
    return crc ^ CRC32_FINAL_XOR;
}
