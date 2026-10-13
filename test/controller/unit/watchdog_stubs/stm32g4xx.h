/*
 * stm32g4xx.h — host stand-in for ST's device header, for the watchdog unit test only
 * (unit/test_watchdog.c). The bootloader's watchdog.c touches the IWDG, and RCC_CSR for
 * watchdog_running(); this header gives the IWDG a RAM structure with the registers of the real
 * header (IWDG_TypeDef in ST's stm32g431xx.h, field order KR, PR, RLR, SR, WINR; RM0440 Rev 9,
 * Sec. 35.4, p. 1559-1563).
 *
 * A write to a RAM register cannot be seen, so every use of the name IWDG goes through
 * iwdg_access(), which first appends the register KR as it stands to a history. KR then shows
 * the keys in the order they were written, once for each access in between. The test reads the
 * history to check the order of the keys.
 */
#ifndef STM32G4XX_H
#define STM32G4XX_H

#include <stdint.h>

typedef struct {
    volatile uint32_t KR;
    volatile uint32_t PR;
    volatile uint32_t RLR;
    volatile uint32_t SR;
    volatile uint32_t WINR;
} IWDG_TypeDef;

extern IWDG_TypeDef fake_iwdg;

/* Returns the fake IWDG after recording the current KR in the history. */
IWDG_TypeDef *iwdg_access(void);

#define IWDG (iwdg_access())

/* RCC_CSR only, for watchdog_running() (decision F(b)): the LSIRDY bit of the real header,
 * bit 1 [RM0440 Rev 9, Sec. 7.4.28, p. 328]. */
typedef struct {
    volatile uint32_t CSR;
} RCC_TypeDef;

extern RCC_TypeDef fake_rcc;

#define RCC (&fake_rcc)
#define RCC_CSR_LSIRDY (1UL << 1)

#endif /* STM32G4XX_H */
