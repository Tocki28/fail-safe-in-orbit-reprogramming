/*
 * flash.c — page erase, double-word programming and the ECC-error NMI, register level.
 * Sequences from RM0440 Rev 9, Sec. 5.3.5-5.3.7, p. 194-199 (category 2 devices: G431).
 * Code keeps running from the same flash while it is erased or programmed: reads stall
 * until the operation ends [RM0440 Rev 9, Sec. 5.3.5, p. 195], so no RAM routine is needed.
 */
#include "flash.h"

#include "layout.h"
#include "stm32g4xx.h"

/* Unlock keys for FLASH_CR [RM0440 Rev 9, Sec. 5.3.5, p. 195]. */
#define FLASH_KEY1 0x45670123U
#define FLASH_KEY2 0xCDEF89ABU

/* Flags that report a failed program or erase [RM0440 Rev 9, Sec. 5.3.7, p. 198-199].
 * Written as 1 to clear them before the next operation, or PGSERR is set (p. 195-196). */
#define FLASH_OP_ERRORS (FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | \
                         FLASH_SR_SIZERR | FLASH_SR_PGSERR | FLASH_SR_MISERR | FLASH_SR_FASTERR)

/* Set by the NMI handler; volatile because it changes outside the normal flow of code. */
static volatile bool s_ecc_error = false;

/**
 * @brief Waits until the flash is not busy.
 *
 * The wait has no time limit of its own, on purpose: BSY is the chip's own statement that an
 * erase or a program is running [RM0440 Rev 9, Sec. 5.7.5, p. 217], a page erase takes up to
 * 24.47 ms [DS12589 Rev 6, Table 46, p. 107], and giving up early would let the next flash
 * write start while the chip is still busy. What ends a BSY that never clears is the
 * independent watchdog, which nothing reloads inside this loop: the bootloader starts it as its
 * first action and reloads it only outside flash waits (bootloader/main.c, bootloader/
 * watchdog.c), so a stuck wait ends in a reset within the watchdog time-out (audit findings #3
 * and #4); the firmware's watchdog does the same with its own time-out (firmware/watchdog.hpp).
 *
 * @note The wait has no time limit of its own.
 */
static void wait_not_busy(void)
{
    while ((FLASH->SR & FLASH_SR_BSY) != 0U) {
    }
}

/**
 * @brief Start of every operation: idle, old errors cleared, FLASH_CR unlocked (p. 195-196).
 */
static void begin(void)
{
    wait_not_busy();
    FLASH->SR = FLASH_OP_ERRORS;
    if ((FLASH->CR & FLASH_CR_LOCK) != 0U) {
        FLASH->KEYR = FLASH_KEY1;
        FLASH->KEYR = FLASH_KEY2;
    }
}

/**
 * @brief Wait for the end and report it.
 *
 * EOP is not used: it is only set when its interrupt is enabled [RM0440 Rev 9, Sec. 5.7.5, p.
 * 218], so the error flags are the result.
 *
 * @return true if no error flag is set, false otherwise
 */
static bool end_ok(void)
{
    wait_not_busy();
    return (FLASH->SR & FLASH_OP_ERRORS) == 0U;
}

/**
 * @brief Lock again, so a stray write cannot start an operation [RM0440 Rev 9, Sec. 5.3.5, p.
 * 195].
 */
static void lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

/**
 * @brief After an erase the data cache may still hold the old bytes; flush it, with the cache off
 * while it is flushed [RM0440 Rev 9, Sec. 5.3.7 "Programming and caches", p. 199].
 */
static void flush_data_cache(void)
{
    FLASH->ACR &= ~FLASH_ACR_DCEN;
    FLASH->ACR |= FLASH_ACR_DCRST;
    FLASH->ACR &= ~FLASH_ACR_DCRST;
    FLASH->ACR |= FLASH_ACR_DCEN;
}

/**
 * @brief Returns true if the bytes from start to start + bytes may be erased or programmed.
 *
 * Allowed: inside the main flash, and clear of the bootloader region and of the last-resort
 * region. Those two are never written by any update or by the boot state (REQ-007, REQ-008,
 * layout.h), so this is a second barrier under the callers' own checks: a wrong address
 * reaching the chip would otherwise erase the code that starts the chip. The main flash is
 * pages 0 to 63 of 2 KB [RM0440 Rev 9, Table 28, p. 189]. The comparisons are written so that
 * no sum wraps past 2^32.
 *
 * @param[in]     start  first byte of the range
 * @param[in]     bytes  length of the range in bytes; 0 allows any start inside the main flash
 * @return true if the range is allowed, false otherwise
 */
static bool range_allowed(uint32_t start, uint32_t bytes)
{
    const uint32_t flash_first = (uint32_t)FLASH_START;
    const uint32_t flash_end = (uint32_t)FLASH_START + (uint32_t)FLASH_BYTES;
    const uint32_t protected_start[2] = {(uint32_t)BOOTLOADER_START, (uint32_t)LAST_RESORT_START};
    const uint32_t protected_end[2] = {(uint32_t)BOOTLOADER_START + (uint32_t)BOOTLOADER_BYTES,
                                       (uint32_t)LAST_RESORT_START + (uint32_t)LAST_RESORT_BYTES};

    if ((start < flash_first) || (start >= flash_end) || (bytes > (flash_end - start))) {
        return false;
    }
    for (uint32_t i = 0U; i < 2U; i++) {
        if ((bytes > 0U) && (start < protected_end[i]) && ((start + bytes) > protected_start[i])) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Erases the 2 KB page that contains page_start, then flushes the data cache.
 *
 * Refuses (returns false, touches no register) when that page is outside the main flash or is a
 * page of the bootloader region or of the last-resort region (range_allowed). The page is the
 * one the chip will erase: the address divided by the page size, rounded down. The page number
 * is also masked to the 6-bit PNB field (FLASH_CR bits 8:3, PNB[5:0], pages 0 to 63
 * [RM0440 Rev 9, Sec. 5.7.6, p. 220]) so that a number that does not fit can never reach other
 * bits of FLASH_CR.
 *
 * @param[in]     page_start  address of the first byte of the page
 * @return true if the chip reported no error; false if the erase was refused or the chip
 *         reported an error
 */
bool flash_erase_page(uint32_t page_start)
{
    uint32_t page = (page_start - (uint32_t)FLASH_START) / (uint32_t)FLASH_PAGE_BYTES;
    uint32_t page_first = (uint32_t)FLASH_START + (page * (uint32_t)FLASH_PAGE_BYTES);

    if ((page_start < (uint32_t)FLASH_START) || !range_allowed(page_first, (uint32_t)FLASH_PAGE_BYTES)) {
        return false;
    }
    begin();
    /* Page erase: PER and the page number, then STRT [RM0440 Rev 9, Sec. 5.3.6, p. 195]. */
    FLASH->CR = (FLASH->CR & ~FLASH_CR_PNB_Msk) | ((page << FLASH_CR_PNB_Pos) & FLASH_CR_PNB_Msk) |
                FLASH_CR_PER;
    FLASH->CR |= FLASH_CR_STRT;
    bool ok = end_ok();
    FLASH->CR &= ~(FLASH_CR_PER | FLASH_CR_PNB_Msk);
    lock();
    flush_data_cache();
    return ok;
}

/**
 * @brief Programs pairs of 32-bit words from words to start, one 64-bit flash word per pair.
 *
 * The target must be erased. Refuses (returns false, touches no register and no flash) when the
 * range start to start + 8 x pairs is not inside the main flash, or overlaps the bootloader
 * region or the last-resort region (range_allowed; the length is checked against the end of
 * flash before it is multiplied, so a large pairs cannot wrap).
 *
 * @param[in]     start  address of the first flash word, 8-byte aligned
 * @param[in]     words  32-bit words to program
 * @param[in]     pairs  number of word pairs
 * @return true if the chip reported no error; false if the write was refused or the chip
 *         reported an error
 */
bool flash_program(uint32_t start, const uint32_t *words, uint32_t pairs)
{
    bool ok = true;

    if ((pairs > ((uint32_t)FLASH_BYTES / 8U)) || !range_allowed(start, pairs * 8U)) {
        return false;
    }
    begin();
    FLASH->CR |= FLASH_CR_PG;
    for (uint32_t i = 0U; (i < pairs) && ok; i++) {
        volatile uint32_t *dst = (volatile uint32_t *)(start + (i * 8U));
        /* Only a whole double word programs: first word, then second, same 8-byte unit
         * [RM0440 Rev 9, Sec. 5.3.7, p. 196]. The barrier keeps the two writes in order, as
         * ST's own driver does (stm32g4xx_hal_flash.c, FLASH_Program_DoubleWord). */
        dst[0] = words[2U * i];
        __ISB();
        dst[1] = words[(2U * i) + 1U];
        ok = end_ok();
    }
    FLASH->CR &= ~FLASH_CR_PG;
    lock();
    return ok;
}

/**
 * @brief ECCD and ECCC are cleared by writing 1; ECCIE is kept as it is.
 *
 * ECCD is only set again if both flags were cleared before [RM0440 Rev 9, Sec. 5.7.7, p. 220].
 */
static void clear_ecc_flags(void)
{
    FLASH->ECCR = (FLASH->ECCR & FLASH_ECCR_ECCIE) | FLASH_ECCR_ECCD | FLASH_ECCR_ECCC;
}

/**
 * @brief Clears the ECC flags and the error recorded by the NMI handler.
 */
void flash_ecc_clear(void)
{
    clear_ecc_flags();
    s_ecc_error = false;
}

/**
 * @brief Returns true if the NMI handler recorded an ECC error since flash_ecc_clear().
 *
 * @return true if an ECC error was recorded, false otherwise
 */
bool flash_ecc_error_seen(void)
{
    return s_ecc_error;
}

/**
 * @brief Returns a pointer to the flash byte at a chip address.
 *
 * The flash is read in place at its own address [RM0440 Rev 9, Table 28, p. 189], so this is the
 * address made a pointer. Why a function and not a cast at each read: the bootloader's image
 * checks and boot_state.c's reads all go through it, so the host tests replace this one function
 * with a RAM copy of the flash (test/controller/unit/boot_stubs) and run the real readers
 * (decision D's tests read boot-state records through it).
 *
 * @param[in]     address  chip address of the byte
 * @return pointer to that byte
 */
const uint8_t *flash_at(uint32_t address)
{
    return (const uint8_t *)(uintptr_t)address;
}

/**
 * @brief Handles the NMI. If the ECC detection flag is set, clears it, records an error and
 * returns.
 *
 * For any other cause it loops for ever.
 *
 * Replaces ST's default NMI handler (a weak symbol in startup_stm32g431xx.s).
 *
 * @note Does not return for any cause other than an ECC error.
 */
void NMI_Handler(void)
{
    if ((FLASH->ECCR & FLASH_ECCR_ECCD) != 0U) {
        clear_ecc_flags();
        s_ecc_error = true;
        return;
    }
    /* The other NMI sources, HSE clock security and SRAM parity (option bit SRAM_PE)
     * [RM0440 Rev 9, Sec. 2.4.1, p. 89], are not used here: stop as ST's default handler
     * does. This loop does not reload the independent watchdog, which the bootloader starts as
     * its first action (bootloader/main.c) and the firmware runs too (firmware/watchdog.hpp),
     * so the chip is restarted when the watchdog's time-out ends [RM0440 Rev 9, Sec. 35.3.1,
     * p. 1557]. The bootloader's time-out is about 32 s (bootloader/watchdog.c). */
    for (;;) {   /* endless: a stop for an NMI cause not used here; the watchdog ends it */
    }
}
