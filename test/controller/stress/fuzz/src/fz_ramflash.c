/*
 * fz_ramflash.c — the controller's flash as RAM for the C fuzz targets (see fz_ramflash.h).
 */
#include "fz_ramflash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bl_flash.h"
#include "flash.h"

#define ERASED_BYTE  0xFFU
/* A chip program unit is one 64-bit word [RM0440 Rev 9, Sec. 5.3.7, p. 196]. */
#define WORD_BYTES   8U
#define PAGE_BYTES   ((uint32_t)FLASH_PAGE_BYTES)
#define TOTAL_BYTES  ((uint32_t)FLASH_BYTES)

static uint8_t g_flash[FLASH_BYTES];
static uint32_t g_ecc_start[RAMFLASH_ECC_RANGES];
static uint32_t g_ecc_bytes[RAMFLASH_ECC_RANGES];
static bool g_ecc_seen;
static uint32_t g_erases;
static uint32_t g_programs;
static bool g_fail_erase;
static bool g_fail_program;

/**
 * @brief Erases the whole array and forgets the ECC ranges and counters.
 */
void ramflash_reset(void)
{
    memset(g_flash, ERASED_BYTE, sizeof g_flash);
    memset(g_ecc_start, 0, sizeof g_ecc_start);
    memset(g_ecc_bytes, 0, sizeof g_ecc_bytes);
    g_ecc_seen = false;
    g_erases = 0U;
    g_programs = 0U;
    g_fail_erase = false;
    g_fail_program = false;
}

/**
 * @brief Returns the array position of a chip address.
 *
 * @param[in]     address  chip address
 * @return pointer, or NULL if the address is outside the 128 KB flash
 */
uint8_t *ramflash_ptr(uint32_t address)
{
    if ((address < (uint32_t)FLASH_START) || ((address - (uint32_t)FLASH_START) >= TOTAL_BYTES)) {
        return NULL;
    }
    return &g_flash[address - (uint32_t)FLASH_START];
}

/**
 * @brief Sets an ECC range.
 *
 * @param[in]     slot   0 or 1
 * @param[in]     start  first chip address
 * @param[in]     bytes  size of the range (0 clears it)
 */
void ramflash_set_ecc(uint32_t slot, uint32_t start, uint32_t bytes)
{
    if (slot < RAMFLASH_ECC_RANGES) {
        g_ecc_start[slot] = start;
        g_ecc_bytes[slot] = bytes;
    }
}

/**
 * @brief Clears both ECC ranges.
 */
void ramflash_clear_ecc(void)
{
    memset(g_ecc_bytes, 0, sizeof g_ecc_bytes);
}

/**
 * @brief Returns the number of erases since reset.
 *
 * @return count
 */
uint32_t ramflash_erases(void)
{
    return g_erases;
}

/**
 * @brief Returns the number of programs since reset.
 *
 * @return count
 */
uint32_t ramflash_programs(void)
{
    return g_programs;
}

/**
 * @brief Makes the next erase and/or program fail.
 *
 * @param[in]     erase    the next erase fails
 * @param[in]     program  the next program fails
 */
void ramflash_fail_next(bool erase, bool program)
{
    g_fail_erase = erase;
    g_fail_program = program;
}

/**
 * @brief Erases a page; the same rules for both entry points.
 *
 * @param[in]     page_start  chip address of the page
 * @return true if erased
 */
static bool do_erase(uint32_t page_start)
{
    uint8_t *p = ramflash_ptr(page_start);

    if (g_fail_erase) {
        g_fail_erase = false;
        return false;
    }
    if ((p == NULL) || (((page_start - (uint32_t)FLASH_START) % PAGE_BYTES) != 0U)) {
        return false;
    }
    memset(p, ERASED_BYTE, PAGE_BYTES);
    g_erases++;
    return true;
}

/**
 * @brief Programs 64-bit words; the same rules for both entry points.
 *
 * @param[in]     start  chip address, 8-byte aligned
 * @param[in]     words  32-bit words
 * @param[in]     pairs  number of 64-bit words
 * @return true if programmed
 */
static bool do_program(uint32_t start, const uint32_t *words, uint32_t pairs)
{
    uint8_t *p = ramflash_ptr(start);
    uint32_t bytes = pairs * WORD_BYTES;

    if (g_fail_program) {
        g_fail_program = false;
        return false;
    }
    if ((p == NULL) || ((start % WORD_BYTES) != 0U)
        || (bytes > (TOTAL_BYTES - (start - (uint32_t)FLASH_START)))) {
        return false;
    }
    for (uint32_t i = 0U; i < bytes; i++) {
        if (p[i] != ERASED_BYTE) {
            return false;   /* PROGERR: written over bytes that were not erased */
        }
    }
    memcpy(p, words, bytes);
    g_programs++;
    return true;
}

/**
 * @brief Returns a pointer to the flash and raises the ECC error inside an ECC range.
 *
 * @param[in]     address  chip address
 * @return pointer to the byte; the first byte of the array if the address is outside the flash
 */
static const uint8_t *do_at(uint32_t address)
{
    const uint8_t *p = ramflash_ptr(address);

    if (p == NULL) {
        (void)fprintf(stderr, "ramflash: read at 0x%08x outside the flash\n", (unsigned)address);
        abort();
    }
    for (uint32_t i = 0U; i < RAMFLASH_ECC_RANGES; i++) {
        if ((g_ecc_bytes[i] != 0U) && (address >= g_ecc_start[i]) &&
            ((address - g_ecc_start[i]) < g_ecc_bytes[i])) {
            g_ecc_seen = true;
        }
    }
    return p;
}

/**
 * @brief flash.h flash_erase_page over the RAM flash.
 *
 * @param[in]     page_start  page address
 * @return true if erased
 */
bool flash_erase_page(uint32_t page_start)
{
    return do_erase(page_start);
}

/**
 * @brief flash.h flash_program over the RAM flash.
 *
 * @param[in]     start  first 64-bit word
 * @param[in]     words  32-bit words
 * @param[in]     pairs  number of 64-bit words
 * @return true if programmed
 */
bool flash_program(uint32_t start, const uint32_t *words, uint32_t pairs)
{
    return do_program(start, words, pairs);
}

/**
 * @brief flash.h flash_ecc_clear over the RAM flash.
 */
void flash_ecc_clear(void)
{
    g_ecc_seen = false;
}

/**
 * @brief flash.h flash_ecc_error_seen over the RAM flash.
 *
 * @return true if a read since the clear hit an ECC range
 */
bool flash_ecc_error_seen(void)
{
    return g_ecc_seen;
}

/**
 * @brief flash.h flash_at over the RAM flash.
 *
 * @param[in]     address  chip address
 * @return pointer to the byte
 */
const uint8_t *flash_at(uint32_t address)
{
    return do_at(address);
}

/**
 * @brief bl_flash.h bl_flash_erase_page over the RAM flash.
 *
 * @param[in]     page_start  page address
 * @return true if erased
 */
bool bl_flash_erase_page(uint32_t page_start)
{
    return do_erase(page_start);
}

/**
 * @brief bl_flash.h bl_flash_program over the RAM flash.
 *
 * @param[in]     start  first 64-bit word
 * @param[in]     words  32-bit words
 * @param[in]     pairs  number of 64-bit words
 * @return true if programmed
 */
bool bl_flash_program(uint32_t start, const uint32_t *words, uint32_t pairs)
{
    return do_program(start, words, pairs);
}

/**
 * @brief bl_flash.h bl_flash_ecc_clear over the RAM flash.
 */
void bl_flash_ecc_clear(void)
{
    g_ecc_seen = false;
}

/**
 * @brief bl_flash.h bl_flash_ecc_error_seen over the RAM flash.
 *
 * @return true if a read since the clear hit an ECC range
 */
bool bl_flash_ecc_error_seen(void)
{
    return g_ecc_seen;
}

/**
 * @brief bl_flash.h bl_flash_at over the RAM flash.
 *
 * @param[in]     address  chip address
 * @return pointer to the byte
 */
const uint8_t *bl_flash_at(uint32_t address)
{
    return do_at(address);
}
