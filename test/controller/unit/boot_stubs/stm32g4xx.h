/*
 * stm32g4xx.h — host stand-in for ST's device header, for the bootloader unit test only
 * (unit/test_boot.c). The bootloader's main.c reads and clears one register, RCC->CSR, in
 * log_reset_cause(); this header gives it a RAM structure with the reset-flag bit masks of the
 * real header [RM0440 Rev 9, Sec. 7.4.28, p. 327-328]. Nothing else of the device is used by
 * main.c.
 */
#ifndef STM32G4XX_H
#define STM32G4XX_H

#include <stdint.h>

typedef struct {
    uint32_t CSR;
} RCC_TypeDef;

extern RCC_TypeDef fake_rcc;

#define RCC (&fake_rcc)

#define RCC_CSR_RMVF      (1UL << 23)
#define RCC_CSR_PINRSTF   (1UL << 26)
#define RCC_CSR_BORRSTF   (1UL << 27)
#define RCC_CSR_SFTRSTF   (1UL << 28)
#define RCC_CSR_IWDGRSTF  (1UL << 29)

#endif /* STM32G4XX_H */
