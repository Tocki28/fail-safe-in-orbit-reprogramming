/*
 * bl_flash.h — the bootloader's own flash access (REQ-119): erase and program the STM32G431's
 * flash, and notice reads of damaged flash. Same meanings as common/flash.h, which the firmware
 * runs; written apart from it (bl_flash.c says what differs), so a defect in one is not in both
 * (bl_crc32.h says why: owner's decision 35, 15 Oct 2026).
 *
 * Damaged flash: every 64-bit flash word carries 8 ECC bits. Two wrong bits in one word, as a
 * power cut during programming can leave, raise an NMI when the word is read
 * [RM0440 Rev 9, Sec. 5.3.2, p. 190]. bl_flash.c's NMI handler records it and returns, so the
 * reader treats the data as damaged and carries on. Not every read of such a word is caught: a
 * re-read can be served from the current flash buffer without raising ECCD [RM0440 Rev 9,
 * Sec. 5.3.2, p. 190], so the readers keep their own CRC-32 and compare checks too.
 */
#ifndef BL_FLASH_H
#define BL_FLASH_H

#include <stdbool.h>
#include <stdint.h>

/* Erase the 2 KB page that contains page_start. True if the chip reported no error; false also
 * when that page is outside the main flash or in the bootloader or last-resort region. */
bool bl_flash_erase_page(uint32_t page_start);

/* Program pairs of 32-bit words (one 64-bit flash word per pair) at an 8-byte aligned, erased
 * address. True if the chip reported no error; false also when the range is outside the main
 * flash or touches the bootloader or last-resort region, or start is not 8-byte aligned. Zero
 * pairs: true for any start inside the main flash, false outside it; nothing is written. */
bool bl_flash_program(uint32_t start, const uint32_t *words, uint32_t pairs);

/* Forget earlier ECC errors; call before reading flash that may be damaged. */
void bl_flash_ecc_clear(void);

/* True if a read since bl_flash_ecc_clear() hit a word with two wrong bits and the NMI handler
 * saw it. Not every such read is caught (see the file comment). */
bool bl_flash_ecc_error_seen(void);

/* Where the bootloader reads the flash byte at chip address `address`: the flash is read in
 * place at its own address [RM0440 Rev 9, Table 28, p. 189]. One function for every flash read
 * of the bootloader, so the host tests can serve those reads from a RAM copy. */
const uint8_t *bl_flash_at(uint32_t address);

#endif /* BL_FLASH_H */
