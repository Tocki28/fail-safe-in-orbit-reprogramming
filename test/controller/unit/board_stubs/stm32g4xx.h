/*
 * stm32g4xx.h — host stand-in for ST's device header, for the firmware's register-level files
 * only (unit/test_registers.cpp: firmware/core_clock.cpp, data-link/uart_link.cpp,
 * mode-manager/mode_port.cpp). C++ only.
 *
 * Every register is RAM with the field names of ST's stm32g431xx.h and Arm's core_cm4.h; every
 * bit value below is the one of those headers (STM32Cube_FW_G4_V1.6.3). Two pieces of the chip
 * are modelled, because the clock code waits for them:
 *   - the PLL: PLLRDY follows PLLON when fake_chip.pll_locks is true
 *     [RM0440 Rev 9, Sec. 7.4.1, p. 287];
 *   - the clock switch: RCC_CFGR SWS follows SW when fake_chip.sws_follows is true
 *     [RM0440 Rev 9, Sec. 7.4.3, p. 289].
 * The model steps on every read of the cycle counter (fakeChipStep), which every bounded wait of
 * core_clock.cpp does once per pass, so a wait sees the chip answer after one pass, or never.
 * The cycle counter counts one per read while fake_chip.cycles_count is true.
 */
#ifndef STM32G4XX_H
#define STM32G4XX_H

#include <cstdint>

/* The chip model's switches (see above). */
struct FakeChip {
    bool cycles_count;   /* DWT CYCCNT advances by one per read */
    bool pll_locks;      /* PLLRDY follows PLLON */
    bool sws_follows;    /* SWS follows SW */
    uint32_t nvic_enabled_irq;
    uint32_t system_resets;   /* NVIC_SystemReset() calls */
};
extern FakeChip fake_chip;

/* One step of the modelled chip: PLLRDY and SWS as the switches above say. */
void fakeChipStep();

/* DWT CYCCNT: a write sets it; a read steps the chip model and counts one. */
struct FakeCycleCounter {
    uint32_t value;

    /**
     * @brief Sets the counter, as a write of CYCCNT does.
     *
     * @param[in] v  new value
     * @return this counter
     */
    FakeCycleCounter &operator=(uint32_t v)
    {
        value = v;
        return *this;
    }

    /**
     * @brief Reads the counter: steps the chip model, then counts one if the counter runs.
     *
     * @return the counter value after the step
     */
    operator uint32_t()
    {
        fakeChipStep();
        if (fake_chip.cycles_count) {
            value = value + 1U;
        }
        return value;
    }
};

struct DWT_Type {
    FakeCycleCounter CYCCNT;
    uint32_t CTRL;
};
struct CoreDebug_Type {
    uint32_t DEMCR;
};
struct RCC_TypeDef {
    uint32_t CR;
    uint32_t CFGR;
    uint32_t PLLCFGR;
    uint32_t AHB2ENR;
    uint32_t APB1ENR1;
};
struct PWR_TypeDef {
    uint32_t CR1;
    uint32_t CR5;
};
struct FLASH_TypeDef {
    uint32_t ACR;
};
struct SysTick_Type {
    uint32_t CTRL;
    uint32_t LOAD;
    uint32_t VAL;
};
struct GPIO_TypeDef {
    uint32_t MODER;
    uint32_t AFR[2];
};
struct USART_TypeDef {
    uint32_t CR1;
    uint32_t BRR;
    uint32_t ISR;
    uint32_t ICR;
    uint32_t RDR;
    uint32_t TDR;
};
/* TAMP keeps the real offsets: 0x100 bytes, then TAMP_BKP0R to TAMP_BKP15R
 * [RM0440 Rev 9, Sec. 38.6.8, p. 1623]. */
struct TAMP_TypeDef {
    uint32_t before_bkp[64];
    uint32_t BKP[16];
};

extern DWT_Type fake_dwt;
extern CoreDebug_Type fake_coredebug;
extern RCC_TypeDef fake_rcc;
extern PWR_TypeDef fake_pwr;
extern FLASH_TypeDef fake_flash_regs;
extern SysTick_Type fake_systick;
extern GPIO_TypeDef fake_gpioa;
extern USART_TypeDef fake_usart2;
extern TAMP_TypeDef fake_tamp;

#define DWT       (&fake_dwt)
#define CoreDebug (&fake_coredebug)
#define RCC       (&fake_rcc)
#define PWR       (&fake_pwr)
#define FLASH     (&fake_flash_regs)
#define SysTick   (&fake_systick)
#define GPIOA     (&fake_gpioa)
#define USART2    (&fake_usart2)
#define TAMP_BASE (reinterpret_cast<uintptr_t>(&fake_tamp))

enum IRQn_Type { USART2_IRQn = 38 };
void NVIC_EnableIRQ(IRQn_Type irq);
void NVIC_SystemReset();

#define CoreDebug_DEMCR_TRCENA_Msk   (1U << 24)
#define DWT_CTRL_CYCCNTENA_Msk       (1U << 0)
#define SysTick_CTRL_ENABLE_Msk      (1U << 0)
#define SysTick_CTRL_TICKINT_Msk     (1U << 1)
#define SysTick_CTRL_CLKSOURCE_Msk   (1U << 2)

#define RCC_CR_PLLON                 (1U << 24)
#define RCC_CR_PLLRDY                (1U << 25)
#define RCC_CFGR_SW_Msk              (0x3U << 0)
#define RCC_CFGR_SW_HSI              (0x1U)
#define RCC_CFGR_SW_PLL              (0x3U)
#define RCC_CFGR_SWS_Msk             (0x3U << 2)
#define RCC_CFGR_SWS_HSI             (0x4U)
#define RCC_CFGR_SWS_PLL             (0xCU)
#define RCC_CFGR_HPRE_Msk            (0xFU << 4)
#define RCC_CFGR_HPRE_DIV2           (0x80U)
#define RCC_PLLCFGR_PLLSRC_HSI       (1U << 1)
#define RCC_PLLCFGR_PLLM_Pos         (4U)
#define RCC_PLLCFGR_PLLN_Pos         (8U)
#define RCC_PLLCFGR_PLLR_Pos         (25U)
#define RCC_PLLCFGR_PLLREN           (1U << 24)
#define RCC_AHB2ENR_GPIOAEN          (1U << 0)
#define RCC_APB1ENR1_RTCAPBEN        (1U << 10)
#define RCC_APB1ENR1_USART2EN        (1U << 17)
#define RCC_APB1ENR1_PWREN           (1U << 28)
#define PWR_CR1_DBP                  (1U << 8)
#define PWR_CR5_R1MODE               (1U << 8)
#define FLASH_ACR_LATENCY_Msk        (0xFU << 0)
#define FLASH_ACR_LATENCY_4WS        (0x4U)
#define FLASH_ACR_PRFTEN             (1U << 8)

#define GPIO_AFRL_AFSEL2_Pos         (8U)
#define GPIO_AFRL_AFSEL2_Msk         (0xFU << GPIO_AFRL_AFSEL2_Pos)
#define GPIO_AFRL_AFSEL3_Pos         (12U)
#define GPIO_AFRL_AFSEL3_Msk         (0xFU << GPIO_AFRL_AFSEL3_Pos)
#define GPIO_MODER_MODE2_Pos         (4U)
#define GPIO_MODER_MODE2_Msk         (0x3U << GPIO_MODER_MODE2_Pos)
#define GPIO_MODER_MODE3_Pos         (6U)
#define GPIO_MODER_MODE3_Msk         (0x3U << GPIO_MODER_MODE3_Pos)
#define USART_CR1_UE                 (1U << 0)
#define USART_CR1_RE                 (1U << 2)
#define USART_CR1_TE                 (1U << 3)
#define USART_CR1_RXNEIE_RXFNEIE     (1U << 5)
#define USART_ISR_ORE                (1U << 3)
#define USART_ISR_RXNE_RXFNE         (1U << 5)
#define USART_ISR_TC                 (1U << 6)
#define USART_ISR_TXE_TXFNF          (1U << 7)
#define USART_ICR_ORECF              (1U << 3)

#endif /* STM32G4XX_H */
