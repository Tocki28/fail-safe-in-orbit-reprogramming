/*
 * crc32.h — the CRC-32 of the Interfaces document (Sec. 4.0): polynomial 0x4C11DB7, start
 * 0xFFFFFFFF, each byte least significant bit first, result inverted. Gives the same value
 * as zlib.crc32 / Ethernet; tools/check_crc32.py checks that on the host.
 */
#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>

/* C linkage when a C++ file includes this (boot_state.h says why). */
#ifdef __cplusplus
extern "C" {
#endif

/* CRC-32 of the size bytes at data. */
uint32_t crc32_compute(const uint8_t *data, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* CRC32_H */
