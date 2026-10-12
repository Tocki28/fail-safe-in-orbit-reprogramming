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

static void wait_not_busy(void)
{
    while ((FLASH->SR & FLASH_SR_BSY) != 0U) {
    }
}

/* Start of every operation: idle, old errors cleared, FLASH_CR unlocked (p. 195-196). */
static void begin(void)
{
    wait_not_busy();
    FLASH->SR = FLASH_OP_ERRORS;
    if ((FLASH->CR & FLASH_CR_LOCK) != 0U) {
        FLASH->KEYR = FLASH_KEY1;
        FLASH->KEYR = FLASH_KEY2;
    }
}

/* Wait for the end and report it. EOP is not used: it is only set when its interrupt is
 * enabled [RM0440 Rev 9, Sec. 5.7.5, p. 218], so the error flags are the result. */
static bool end_ok(void)
{
    wait_not_busy();
    return (FLASH->SR & FLASH_OP_ERRORS) == 0U;
}

/* Lock again, so a stray write cannot start an operation [RM0440 Rev 9, Sec. 5.3.5, p. 195]. */
static void lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

/* After an erase the data cache may still hold the old bytes; flush it, with the cache off
 * while it is flushed [RM0440 Rev 9, Sec. 5.3.7 "Programming and caches", p. 199]. */
static void flush_data_cache(void)
{
    FLASH->ACR &= ~FLASH_ACR_DCEN;
    FLASH->ACR |= FLASH_ACR_DCRST;
    FLASH->ACR &= ~FLASH_ACR_DCRST;
    FLASH->ACR |= FLASH_ACR_DCEN;
}

bool flash_erase_page(uint32_t page_start)
{
    uint32_t page = (page_start - (uint32_t)FLASH_START) / (uint32_t)FLASH_PAGE_BYTES;

    begin();
    /* Page erase: PER and the page number, then STRT [RM0440 Rev 9, Sec. 5.3.6, p. 195]. */
    FLASH->CR = (FLASH->CR & ~FLASH_CR_PNB_Msk) | (page << FLASH_CR_PNB_Pos) | FLASH_CR_PER;
    FLASH->CR |= FLASH_CR_STRT;
    bool ok = end_ok();
    FLASH->CR &= ~(FLASH_CR_PER | FLASH_CR_PNB_Msk);
    lock();
    flush_data_cache();
    return ok;
}

bool flash_program(uint32_t start, const uint32_t *words, uint32_t pairs)
{
    bool ok = true;

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

/* ECCD and ECCC are cleared by writing 1; ECCIE is kept as it is. ECCD is only set again if
 * both flags were cleared before [RM0440 Rev 9, Sec. 5.7.7, p. 220]. */
static void clear_ecc_flags(void)
{
    FLASH->ECCR = (FLASH->ECCR & FLASH_ECCR_ECCIE) | FLASH_ECCR_ECCD | FLASH_ECCR_ECCC;
}

void flash_ecc_clear(void)
{
    clear_ecc_flags();
    s_ecc_error = false;
}

bool flash_ecc_error_seen(void)
{
    return s_ecc_error;
}

/* Replaces ST's default NMI handler (a weak symbol in startup_stm32g431xx.s). */
void NMI_Handler(void)
{
    if ((FLASH->ECCR & FLASH_ECCR_ECCD) != 0U) {
        clear_ecc_flags();
        s_ecc_error = true;
        return;
    }
    /* The other NMI sources, HSE clock security and SRAM parity (option bit SRAM_PE)
     * [RM0440 Rev 9, Sec. 2.4.1, p. 89], are not used here: stop as ST's default handler
     * does; a running watchdog restarts the chip. */
    for (;;) {
    }
}
