/*
 * flash.h — erase and program the STM32G431's own flash, and notice reads of damaged flash.
 *
 * Damaged flash: every 64-bit flash word carries 8 ECC bits. Two wrong bits in one word, as a
 * power cut during programming can leave, raise an NMI when the word is read
 * [RM0440 Rev 9, Sec. 5.3.2, p. 190]. ST's default NMI handler loops forever, which would stop
 * the bootloader with no watchdog running. This module's NMI handler records the error and
 * returns, so the reader can treat the data as damaged and carry on.
 */
#ifndef FLASH_H
#define FLASH_H

#include <stdbool.h>
#include <stdint.h>

/* C linkage when a C++ file includes this (boot_state.h says why). */
#ifdef __cplusplus
extern "C" {
#endif

/* Erase the 2 KB page that starts at page_start. True if the chip reported no error. */
bool flash_erase_page(uint32_t page_start);

/* Program pairs of 32-bit words (one 64-bit flash word per pair) at an 8-byte aligned
 * address that is erased. True if the chip reported no error. */
bool flash_program(uint32_t start, const uint32_t *words, uint32_t pairs);

/* Forget earlier ECC errors; call before reading flash that may be damaged. */
void flash_ecc_clear(void);

/* True if a read since flash_ecc_clear() hit a word with two wrong bits. */
bool flash_ecc_error_seen(void);

#ifdef __cplusplus
}
#endif

#endif /* FLASH_H */
