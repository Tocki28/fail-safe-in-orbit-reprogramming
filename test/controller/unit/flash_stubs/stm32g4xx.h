/*
 * stm32g4xx.h — host stand-in for ST's device header, for the flash unit test only
 * (unit/test_flash.c). The shared flash.c touches the FLASH registers; this header gives it a
 * RAM structure with the register order of the real header (FLASH_TypeDef in ST's
 * stm32g431xx.h, Cube FW_G4 V1.6.3) and the bit masks flash.c uses, with the values of that
 * header: FLASH_CR PNB = bits 8:3 (6 bits, mask 0x1F8), PER bit 1, PG bit 0, STRT bit 16,
 * LOCK bit 31; FLASH_SR BSY bit 16 and the error flags bits 1 and 3 to 9.
 *
 * A write of STRT cannot be seen in RAM, so FLASH_CR_STRT expands to a call that records
 * FLASH_CR as it stands at that moment (flash_strt_hook): the page number and PER the erase was
 * started with.
 */
#ifndef STM32G4XX_H
#define STM32G4XX_H

#include <stdint.h>

typedef struct {
    volatile uint32_t ACR;
    volatile uint32_t PDKEYR;
    volatile uint32_t KEYR;
    volatile uint32_t OPTKEYR;
    volatile uint32_t SR;
    volatile uint32_t CR;
    volatile uint32_t ECCR;
} FLASH_TypeDef;

extern FLASH_TypeDef fake_flash;
#define FLASH (&fake_flash)

/* Records FLASH_CR at the moment STRT is written, and returns the STRT bit. */
uint32_t flash_strt_hook(void);

#define FLASH_SR_OPERR     (1UL << 1)
#define FLASH_SR_PROGERR   (1UL << 3)
#define FLASH_SR_WRPERR    (1UL << 4)
#define FLASH_SR_PGAERR    (1UL << 5)
#define FLASH_SR_SIZERR    (1UL << 6)
#define FLASH_SR_PGSERR    (1UL << 7)
#define FLASH_SR_MISERR    (1UL << 8)
#define FLASH_SR_FASTERR   (1UL << 9)
#define FLASH_SR_BSY       (1UL << 16)

#define FLASH_CR_PG        (1UL << 0)
#define FLASH_CR_PER       (1UL << 1)
#define FLASH_CR_PNB_Pos   (3U)
#define FLASH_CR_PNB_Msk   (0x3FUL << FLASH_CR_PNB_Pos)
#define FLASH_CR_STRT      (flash_strt_hook())
#define FLASH_CR_LOCK      (1UL << 31)

#define FLASH_ACR_DCEN     (1UL << 10)
#define FLASH_ACR_DCRST    (1UL << 12)

#define FLASH_ECCR_ECCIE   (1UL << 24)
#define FLASH_ECCR_ECCC    (1UL << 30)
#define FLASH_ECCR_ECCD    (1UL << 31)

#define __ISB()            ((void)0)

#endif /* STM32G4XX_H */
