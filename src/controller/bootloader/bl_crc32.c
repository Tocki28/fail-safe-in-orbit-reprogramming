/*
 * bl_crc32.c — the bootloader's own CRC-32 (bl_crc32.h says why it has its own: REQ-119).
 *
 * Algorithm form: four bits at a time, with a 16-entry table. common/crc32.c, which the
 * firmware runs, goes one bit at a time with no table. The two forms share no line, so a slip in
 * one (a wrong shift, a missed bit) does not sit in the other, and the differential tests compare
 * them byte for byte. Why 16 entries and not the usual 256: a 256-entry table is 1 KB of the
 * bootloader's 24 KB region (layout.h, BOOTLOADER_BYTES); 16 entries are 64 bytes.
 *
 * How it works. The reflected CRC shifts right one bit per step and adds (XOR) the bit-reversed
 * polynomial when the bit shifted out is 1. Four such steps depend only on the low four bits of
 * the register, so they can be looked up: entry n of the table is what four steps leave of the
 * value n. One byte is two look-ups: its low four bits first, then its high four bits, which is
 * the order "least significant bit first" asks for (Interfaces Sec. 4.0).
 */
#include "bl_crc32.h"

/* Start value and final inversion (Interfaces Sec. 4.0). */
#define START_VALUE        0xFFFFFFFFU
#define FINAL_INVERSION    0xFFFFFFFFU
/* Four bits per look-up; a byte has two such halves. */
#define BITS_PER_LOOKUP    4U
#define LOW_FOUR_BITS      0x0FU

/*
 * Entry n: the register after four right-shift steps of the polynomial 0x4C11DB7 bit-reversed,
 * 0xEDB88320 (Interfaces Sec. 4.0), starting from the value n. Computed on 15 Oct 2026 from that
 * definition (a four-step loop, Python on the host), not copied from a listing. Two checks a
 * reader can make: entry 8 is the bit-reversed polynomial itself (only the fourth step adds it),
 * and entry (a XOR b) = entry a XOR entry b for every a and b (the CRC is linear).
 */
static const uint32_t NIBBLE_TABLE[16] = {
    0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU,
    0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
    0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
    0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU,
};

/**
 * @brief Returns the CRC-32 of the size bytes at data (Interfaces Sec. 4.0).
 *
 * Each byte is fed in two halves of four bits, low half first: the half is added (XOR) to the low
 * bits of the register, and the four steps that follow are one table look-up (see the file
 * comment). The loop runs exactly size times; size is the caller's length, which the caller has
 * already checked against its buffer (bl_image_check.c checks the body length against the
 * region before it asks for the CRC; bl_boot_state.c passes a fixed 12).
 *
 * @param[in]     data  first byte
 * @param[in]     size  number of bytes
 * @return CRC-32 of the bytes
 */
uint32_t bl_crc32_compute(const uint8_t *data, uint32_t size)
{
    uint32_t reg = START_VALUE;

    for (uint32_t i = 0U; i < size; i++) {
        uint32_t byte = (uint32_t)data[i];
        reg = (reg >> BITS_PER_LOOKUP) ^ NIBBLE_TABLE[(reg ^ byte) & LOW_FOUR_BITS];
        reg = (reg >> BITS_PER_LOOKUP) ^
              NIBBLE_TABLE[(reg ^ (byte >> BITS_PER_LOOKUP)) & LOW_FOUR_BITS];
    }
    return reg ^ FINAL_INVERSION;
}
