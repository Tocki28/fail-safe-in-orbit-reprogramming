/*
 * stm32g4xx.h — host stand-in for ST's device header, for the fuzz target of the bootloader's
 * backup-register reads (src/fz_backup.c). It is the union of the two stand-ins of the unit
 * tests, which stay as they are (read only): test/controller/unit/boot_stubs/stm32g4xx.h gives
 * main.c its RCC->CSR and the reset flags [RM0440 Rev 9, Sec. 7.4.28, p. 327-328], and
 * test/controller/unit/backup_stubs/stm32g4xx.h gives backup.c its RCC->APB1ENR1, PWR->CR1 and
 * the TAMP backup registers at the real offsets [RM0440 Rev 9, Sec. 38.6.8, p. 1623]. With both
 * in one header, the bootloader's main.c and its real backup.c run together on the host.
 */
#ifndef STM32G4XX_H
#define STM32G4XX_H

#include <stdint.h>

/* Registers before TAMP_BKP0R: 0x100 bytes = 64 words [RM0440 Rev 9, Sec. 38.6.8, p. 1623]. */
#define FAKE_TAMP_WORDS_BEFORE_BKP 64U
/* Backup registers of a category 2 device [RM0440 Rev 9, Sec. 38.1, p. 1610]. */
#define FAKE_TAMP_BKP_REGISTERS    16U

typedef struct {
    volatile uint32_t CSR;
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

#define RCC_CSR_RMVF      (1UL << 23)
#define RCC_CSR_PINRSTF   (1UL << 26)
#define RCC_CSR_BORRSTF   (1UL << 27)
#define RCC_CSR_SFTRSTF   (1UL << 28)
#define RCC_CSR_IWDGRSTF  (1UL << 29)

#define RCC_APB1ENR1_PWREN     (1UL << 28)
#define RCC_APB1ENR1_RTCAPBEN  (1UL << 10)
#define PWR_CR1_DBP            (1UL << 8)

#endif /* STM32G4XX_H */
