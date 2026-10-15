/*
 * bl_flash.c — the bootloader's own flash access: page erase, double-word programming and the
 * ECC-error NMI, register level (bl_flash.h; bl_crc32.h says why the bootloader has its own).
 *
 * What is written apart from common/flash.c, which the firmware runs:
 *   - the address barrier is an allow-list: the two windows any program may write, the two slots
 *     and the boot state (layout.h). common/flash.c is a deny-list: the main flash minus the
 *     bootloader and last-resort regions. The static assertions below prove the two give the same
 *     answer for this flash map, so the meanings match; the code paths share nothing;
 *   - a program refuses a start that is not 8-byte aligned before it touches a register (the
 *     chip would refuse it with PGAERR [RM0440 Rev 9, Sec. 5.3.7, p. 196]);
 *   - a program of zero pairs touches no register;
 *   - the operation is split as open / wait for the result / close, and the NMI handler tests
 *     for the one cause it handles and stops on every other.
 * What cannot differ: the register sequences, which RM0440 fixes (Sec. 5.3.5-5.3.7, p. 194-199,
 * category 2 devices: G431). Code keeps running from the same flash while it is erased or
 * programmed: reads stall until the operation ends [RM0440 Rev 9, Sec. 5.3.5, p. 195], so no RAM
 * routine is needed.
 */
#include "bl_flash.h"

#include "layout.h"
#include "stm32g4xx.h"

/* Unlock keys for FLASH_CR [RM0440 Rev 9, Sec. 5.3.5, p. 195]. */
#define UNLOCK_KEY_1        0x45670123U
#define UNLOCK_KEY_2        0xCDEF89ABU

/* Flags that report a failed program or erase [RM0440 Rev 9, Sec. 5.3.7, p. 198-199]. Written as
 * 1 to clear them before the next operation, or PGSERR is set [RM0440 Rev 9, Sec. 5.3.6, p. 195;
 * Sec. 5.3.7, p. 196]. */
#define OPERATION_ERRORS    (FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | \
                             FLASH_SR_PGAERR | FLASH_SR_SIZERR | FLASH_SR_PGSERR | \
                             FLASH_SR_MISERR | FLASH_SR_FASTERR)

/* The chip programs one 64-bit flash word at a time, aligned on 8 bytes
 * [RM0440 Rev 9, Sec. 5.3.7, p. 196]. */
#define DOUBLE_WORD_BYTES   8U

/* End of the main flash: one past its last byte [RM0440 Rev 9, Table 28, p. 189; layout.h]. */
#define MAIN_FLASH_END      ((uint32_t)FLASH_START + (uint32_t)FLASH_BYTES)

/* The windows the barrier allows, from layout.h: slot A and slot B (one window, they touch),
 * and the two boot-state copies. */
#define WINDOWS             2U
#define SLOTS_FIRST         ((uint32_t)SLOT_A_START)
#define SLOTS_END           ((uint32_t)SLOT_B_START + (uint32_t)SLOT_BYTES)
#define STATE_FIRST         ((uint32_t)BOOT_STATE_COPY_0)
#define STATE_END           ((uint32_t)BOOT_STATE_COPY_1 + (uint32_t)BOOT_STATE_COPY_BYTES)

/* The allow-list equals "main flash minus the bootloader and last-resort regions" (the meaning
 * in bl_flash.h and common/flash.h) only while the regions lie end to end in this order. Each
 * line stops the build if layout.h ever moves one. */
_Static_assert(BOOTLOADER_START == FLASH_START, "the bootloader starts the flash");
_Static_assert(SLOT_A_START == BOOTLOADER_START + BOOTLOADER_BYTES,
               "slot A follows the bootloader");
_Static_assert(SLOT_B_START == SLOT_A_START + SLOT_BYTES, "slot B follows slot A");
_Static_assert(LAST_RESORT_START == SLOT_B_START + SLOT_BYTES, "last resort follows slot B");
_Static_assert(BOOT_STATE_COPY_0 == LAST_RESORT_START + LAST_RESORT_BYTES,
               "boot-state copy 0 follows the last-resort region");
_Static_assert(BOOT_STATE_COPY_1 + BOOT_STATE_COPY_BYTES == FLASH_START + FLASH_BYTES,
               "boot-state copy 1 ends the flash");
/* Every page number of the main flash fits the PNB field, FLASH_CR bits 8:3 (pages 0 to 63)
 * [RM0440 Rev 9, Sec. 5.7.6, p. 220]. */
_Static_assert(((uint32_t)FLASH_BYTES / (uint32_t)FLASH_PAGE_BYTES) ==
                   (uint32_t)((FLASH_CR_PNB_Msk >> FLASH_CR_PNB_Pos) + 1U),
               "the main flash has exactly as many pages as PNB can name");

/* Set by the NMI handler; volatile because it changes outside the normal flow of code. */
static volatile bool s_ecc_seen = false;

/**
 * @brief Returns true if the bytes from first to first + bytes lie inside one allowed window.
 *
 * The windows are the two slots and the boot state (see the static assertions above: the same
 * set as the main flash minus the bootloader and last-resort regions). The two forbidden regions
 * are never written by any update or by the boot state (REQ-007, REQ-008, layout.h), so this is
 * a second barrier under the callers' own checks. A range must sit wholly in one window: the
 * last-resort region lies between the two, so no allowed range crosses from one to the other.
 * Written so that no sum can wrap past 2^32: the length is compared with the room left.
 *
 * @param[in]     first  first byte of the range
 * @param[in]     bytes  length of the range in bytes, 1 or more
 * @return true if the range is allowed, false otherwise
 */
static bool inside_window(uint32_t first, uint32_t bytes)
{
    const uint32_t window_first[WINDOWS] = {SLOTS_FIRST, STATE_FIRST};
    const uint32_t window_end[WINDOWS] = {SLOTS_END, STATE_END};

    for (uint32_t w = 0U; w < WINDOWS; w++) {
        if ((first >= window_first[w]) && (first < window_end[w]) &&
            (bytes <= (window_end[w] - first))) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Waits while the flash reports BSY.
 *
 * BSY is the chip's own statement that an erase or a program is running [RM0440 Rev 9,
 * Sec. 5.7.5, p. 217]. A page erase takes up to 24.47 ms [DS12589 Rev 6, Table 46, p. 107].
 * The wait has no time limit of its own, on purpose: leaving early would let the next register
 * write or operation start while the chip is still busy. What ends a BSY that never clears is
 * the independent watchdog, which nothing reloads inside this loop: the bootloader starts it as
 * its first action and reloads it only outside flash waits (main.c, watchdog.c), so a stuck
 * wait ends in a reset within the watchdog time-out (audit findings #3 and #4).
 *
 * @note The wait has no time limit of its own; the watchdog bounds it.
 */
static void wait_idle(void)
{
    /* endless: only while BSY stays set; the watchdog ends it (see above) */
    while ((FLASH->SR & FLASH_SR_BSY) != 0U) {
    }
}

/**
 * @brief Opens the flash for one operation: idle, earlier error flags cleared, FLASH_CR
 * unlocked.
 *
 * Steps 1 and 2 of the erase and program sequences [RM0440 Rev 9, Sec. 5.3.6, p. 195;
 * Sec. 5.3.7, p. 196], then the key sequence. The keys are written only while LOCK is set: LOCK
 * is cleared by hardware once the sequence is seen [RM0440 Rev 9, Sec. 5.7.6, p. 218], and a
 * wrong sequence locks FLASH_CR until the next reset and raises a bus error [RM0440 Rev 9,
 * Sec. 5.3.5, p. 195].
 */
static void open_for_operation(void)
{
    wait_idle();
    FLASH->SR = OPERATION_ERRORS;
    if ((FLASH->CR & FLASH_CR_LOCK) != 0U) {
        FLASH->KEYR = UNLOCK_KEY_1;
        FLASH->KEYR = UNLOCK_KEY_2;
    }
}

/**
 * @brief Waits for the end of the operation and returns its result.
 *
 * The error flags are the result. EOP is not used: it is set only when its interrupt is enabled
 * [RM0440 Rev 9, Sec. 5.7.5, p. 218].
 *
 * @return true if no error flag is set, false otherwise
 */
static bool result_ok(void)
{
    wait_idle();
    return (FLASH->SR & OPERATION_ERRORS) == 0U;
}

/**
 * @brief Closes the flash after an operation: clears the operation's bits in FLASH_CR, then
 * locks FLASH_CR again so a stray write cannot start an operation [RM0440 Rev 9, Sec. 5.3.5,
 * p. 195].
 *
 * @param[in]     operation_bits  the FLASH_CR bits the operation set (PER and PNB, or PG)
 */
static void close_after_operation(uint32_t operation_bits)
{
    FLASH->CR &= ~operation_bits;
    FLASH->CR |= FLASH_CR_LOCK;
}

/**
 * @brief Flushes the data cache after an erase, with the cache off while it is flushed.
 *
 * After an erase the data cache may still hold the old bytes; RM0440 asks for DCRST with DCEN
 * cleared [RM0440 Rev 9, Sec. 5.3.7 "Programming and caches", p. 199]. The instruction cache is
 * not flushed: the bootloader erases only data pages (the boot state), never code it runs.
 */
static void flush_data_cache(void)
{
    FLASH->ACR &= ~FLASH_ACR_DCEN;
    FLASH->ACR |= FLASH_ACR_DCRST;
    FLASH->ACR &= ~FLASH_ACR_DCRST;
    FLASH->ACR |= FLASH_ACR_DCEN;
}

/**
 * @brief Erases the 2 KB page that contains page_start, then flushes the data cache.
 *
 * Refuses (returns false, touches no register) when page_start is outside the main flash or its
 * page is not inside an allowed window (inside_window). The page is the one the chip erases: the
 * offset into the flash divided by the page size, rounded down. The page number is below 64 by
 * the checks above, and is masked to PNB anyway, so no number can reach other bits of FLASH_CR
 * [RM0440 Rev 9, Sec. 5.7.6, p. 220]. Sequence: PER and PNB, then STRT, then wait
 * [RM0440 Rev 9, Sec. 5.3.6, p. 195].
 *
 * @param[in]     page_start  an address inside the page, normally its first byte
 * @return true if the chip reported no error; false if the erase was refused or the chip reported
 *         an error
 */
bool bl_flash_erase_page(uint32_t page_start)
{
    if ((page_start < (uint32_t)FLASH_START) || (page_start >= MAIN_FLASH_END)) {
        return false;
    }
    uint32_t page = (page_start - (uint32_t)FLASH_START) / (uint32_t)FLASH_PAGE_BYTES;
    uint32_t page_first = (uint32_t)FLASH_START + (page * (uint32_t)FLASH_PAGE_BYTES);
    if (!inside_window(page_first, (uint32_t)FLASH_PAGE_BYTES)) {
        return false;
    }

    open_for_operation();
    uint32_t select = ((page << FLASH_CR_PNB_Pos) & FLASH_CR_PNB_Msk) | FLASH_CR_PER;
    FLASH->CR = (FLASH->CR & ~FLASH_CR_PNB_Msk) | select;
    FLASH->CR |= FLASH_CR_STRT;
    bool ok = result_ok();
    close_after_operation(FLASH_CR_PER | FLASH_CR_PNB_Msk);
    flush_data_cache();
    return ok;
}

/**
 * @brief Programs one 64-bit flash word: the first word, then the second, at an address aligned
 * on 8 bytes, and waits for the result.
 *
 * Only a whole double word programs: first word, then second, same 8-byte unit [RM0440 Rev 9,
 * Sec. 5.3.7, p. 196]. The instruction barrier keeps the two writes in order, as ST's own driver
 * does (stm32g4xx_hal_flash.c, FLASH_Program_DoubleWord). PG must already be set.
 *
 * @param[in]     address  chip address of the flash word, 8-byte aligned, inside a window
 * @param[in]     first    32-bit word for the lower address
 * @param[in]     second   32-bit word for the upper address
 * @return true if the chip reported no error, false otherwise
 */
static bool program_double_word(uint32_t address, uint32_t first, uint32_t second)
{
    volatile uint32_t *target = (volatile uint32_t *)(uintptr_t)address;

    target[0] = first;
    __ISB();
    target[1] = second;
    return result_ok();
}

/**
 * @brief Programs pairs of 32-bit words from words to start, one 64-bit flash word per pair; the
 * target must be erased.
 *
 * Checks, in order, before any register is touched: start inside the main flash; zero pairs
 * then ends here with true (nothing to write); start 8-byte aligned; pairs no more than the
 * flash holds, so 8 x pairs cannot wrap; the whole range inside one allowed window
 * (inside_window). A refusal returns false. Stops at the first flash word the chip reports as
 * failed.
 *
 * @param[in]     start  address of the first flash word, 8-byte aligned
 * @param[in]     words  32-bit words to program, 2 x pairs of them
 * @param[in]     pairs  number of word pairs
 * @return true if the chip reported no error (or pairs is 0 and start is in the main flash);
 *         false if the write was refused or the chip reported an error
 */
bool bl_flash_program(uint32_t start, const uint32_t *words, uint32_t pairs)
{
    if ((start < (uint32_t)FLASH_START) || (start >= MAIN_FLASH_END)) {
        return false;
    }
    if (pairs == 0U) {
        return true;
    }
    if (((start % DOUBLE_WORD_BYTES) != 0U) ||
        (pairs > ((uint32_t)FLASH_BYTES / DOUBLE_WORD_BYTES)) ||
        !inside_window(start, pairs * DOUBLE_WORD_BYTES)) {
        return false;
    }

    open_for_operation();
    FLASH->CR |= FLASH_CR_PG;
    bool ok = true;
    for (uint32_t i = 0U; (i < pairs) && ok; i++) {
        ok = program_double_word(start + (i * DOUBLE_WORD_BYTES), words[2U * i],
                                 words[(2U * i) + 1U]);
    }
    close_after_operation(FLASH_CR_PG);
    return ok;
}

/**
 * @brief Clears the two ECC flags by writing 1 to them, keeping ECCIE as it is.
 *
 * ECCD (two errors) and ECCC (one corrected error) are cleared by writing 1, and ECCD is set
 * again only if both were cleared before [RM0440 Rev 9, Sec. 5.7.7, p. 220].
 */
static void clear_ecc_flags(void)
{
    uint32_t keep = FLASH->ECCR & FLASH_ECCR_ECCIE;

    FLASH->ECCR = keep | FLASH_ECCR_ECCD | FLASH_ECCR_ECCC;
}

/**
 * @brief Clears the ECC flags and the error recorded by the NMI handler.
 *
 * Called before a read whose result must be judged (bl_boot_state.c, main.c image_passes).
 */
void bl_flash_ecc_clear(void)
{
    clear_ecc_flags();
    s_ecc_seen = false;
}

/**
 * @brief Returns true if the NMI handler recorded an ECC error since bl_flash_ecc_clear().
 *
 * @return true if an ECC error was recorded, false otherwise
 */
bool bl_flash_ecc_error_seen(void)
{
    return s_ecc_seen;
}

/**
 * @brief Returns a pointer to the flash byte at a chip address.
 *
 * The flash is read in place at its own address [RM0440 Rev 9, Table 28, p. 189], so this is the
 * address made a pointer. Why a function and not a cast at each read: every flash read of the
 * bootloader goes through it (main.c, bl_boot_state.c), so the host tests can replace this one
 * function with a RAM copy of the flash and run the real readers.
 *
 * @param[in]     address  chip address of the byte
 * @return pointer to that byte
 */
const uint8_t *bl_flash_at(uint32_t address)
{
    return (const uint8_t *)(uintptr_t)address;
}

/**
 * @brief Handles the NMI: for an ECC detection, clears the flags, records the error and
 * returns; for any other cause, stops.
 *
 * Replaces ST's default NMI handler (a weak symbol in startup_stm32g431xx.s). The other NMI
 * sources, HSE clock security and SRAM parity (option bit SRAM_PE) [RM0440 Rev 9, Sec. 2.4.1,
 * p. 89], are not used here: stop as ST's default handler does. The stop does not reload the
 * independent watchdog, which the bootloader starts as its first action (main.c), so the chip
 * restarts when the watchdog's time-out ends [RM0440 Rev 9, Sec. 35.3.1, p. 1557]; the
 * bootloader's time-out is about 32 s (watchdog.c).
 *
 * @note Does not return for any cause other than an ECC detection.
 */
void NMI_Handler(void)
{
    if ((FLASH->ECCR & FLASH_ECCR_ECCD) == 0U) {
        for (;;) {   /* endless: a stop for an NMI cause not used here; the watchdog ends it */
        }
    }
    clear_ecc_flags();
    s_ecc_seen = true;
}
