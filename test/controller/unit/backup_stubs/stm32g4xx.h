/*
 * stm32g4xx.h — host stand-in for ST's device header, for the backup-register unit test only
 * (unit/test_backup.c). The bootloader's backup.c touches three peripherals: RCC_APB1ENR1 (PWR
 * and RTC APB clocks), PWR_CR1 (DBP) and the TAMP backup registers. This header gives each a RAM
 * structure with the bits of the real header (ST's stm32g431xx.h):
 *   PWREN    RCC_APB1ENR1 bit 28 [RM0440 Rev 9, Sec. 7.4.17, p. 309];
 *   RTCAPBEN RCC_APB1ENR1 bit 10 [RM0440 Rev 9, Sec. 7.4.17, p. 310];
 *   DBP      PWR_CR1 bit 8 [RM0440 Rev 9, Sec. 6.4.1, p. 253].
 * The TAMP stand-in keeps the real offsets: 0x100 bytes before TAMP_BKP0R, then the 16 backup
 * registers, 4 bytes apart [RM0440 Rev 9, Sec. 38.6.8, p. 1623], so backup.c's address arithmetic
 * lands on the same register it would on the chip.
 */
#ifndef STM32G4XX_H
#define STM32G4XX_H

#include <stdint.h>

/* Registers before TAMP_BKP0R: 0x100 bytes = 64 words. */
#define FAKE_TAMP_WORDS_BEFORE_BKP 64U
/* Backup registers of a category 2 device [RM0440 Rev 9, Sec. 38.1, p. 1610]. */
#define FAKE_TAMP_BKP_REGISTERS    16U

typedef struct {
    volatile uint32_t APB1ENR1;
} RCC_TypeDef;

typedef struct {
    volatile uint32_t CR1;
} PWR_TypeDef;

typedef struct {
    volatile uint32_t before_bkp[FAKE_TAMP_WORDS_BEFORE_BKP];
    volatile uint32_t BKP[FAKE_TAMP_BKP_REGISTERS];
} TAMP_TypeDef;

extern RCC_TypeDef fake_rcc;
extern PWR_TypeDef fake_pwr;
extern TAMP_TypeDef fake_tamp;

#define RCC  (&fake_rcc)
#define PWR  (&fake_pwr)
#define TAMP (&fake_tamp)

#define RCC_APB1ENR1_PWREN     (1UL << 28)
#define RCC_APB1ENR1_RTCAPBEN  (1UL << 10)
#define PWR_CR1_DBP            (1UL << 8)

#endif /* STM32G4XX_H */
