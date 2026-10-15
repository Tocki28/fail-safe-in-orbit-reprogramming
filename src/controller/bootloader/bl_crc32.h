/*
 * bl_crc32.h — the bootloader's own CRC-32 (REQ-119): the CRC-32 of the Interfaces document
 * (Sec. 4.0): polynomial 0x4C11DB7, start 0xFFFFFFFF, each byte least significant bit first,
 * result inverted. The same value as common/crc32.c and as zlib.crc32.
 *
 * Why the bootloader has its own: REQ-119 asks it to share no code with the main firmware or the
 * last-resort firmware (owner's decision 35, 15 Oct 2026). A defect in shared code reaches every
 * program that links it at once, so the bootloader that must recover from a bad firmware would
 * carry the firmware's defect. This file is written apart from common/crc32.c, in another
 * algorithm form (bl_crc32.c), so one defect does not sit in both. Both must give the same value
 * for the same bytes: the firmware and the bootloader check each other's records and images.
 */
#ifndef BL_CRC32_H
#define BL_CRC32_H

#include <stdint.h>

/* CRC-32 of the size bytes at data. Same meaning as crc32_compute (common/crc32.h). */
uint32_t bl_crc32_compute(const uint8_t *data, uint32_t size);

#endif /* BL_CRC32_H */
