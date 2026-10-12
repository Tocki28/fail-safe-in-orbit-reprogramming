/*
 * core_clock.c — core clock up to 170 MHz, cycle counter, millisecond tick; register level.
 *
 * The clock steps are the bootloader's (bootloader/clock.c), copied, not shared: REQ-119 asks
 * the bootloader to share no code with the main firmware. Two differences: every wait here
 * gives up after WAIT_CYCLES, and there is no way back to the reset clock (the firmware hands
 * over to nothing).
 *
 * The firmware starts on HSI16 at 16 MHz, 1 flash wait state: the bootloader puts the clock
 * back to its reset state before the jump (bootloader/handover.h).
 */
#include "core_clock.h"

#include "stm32g4xx.h"

/* PLL from HSI16: / M = 4 MHz into the VCO (2.66-16 MHz allowed), x N = 340 MHz VCO
 * (96-344 MHz allowed), / R = 170 MHz [DS12589 Rev 6, Table 45, p. 106]. */
#define PLL_M               4U
#define PLL_N               85U
/* PLLM field holds M - 1; PLLR field 00 means divide by 2 [RM0440 Rev 9, Sec. 7.4.4, p. 292-293]. */
#define PLLM_FIELD          (PLL_M - 1U)
#define PLLR_DIV2_FIELD     0U
/* Boost-mode entry waits at least 1 us before the AHB divider goes back to 1
 * [RM0440 Rev 9, Sec. 6.1.5, p. 233]; 170 cycles is >= 1 us at any clock up to 170 MHz. */
#define SETTLE_CYCLES       (CORE_CLOCK_HZ / 1000000U)
/* Longest wait here: PLL lock, 40 us at most [DS12589 Rev 6, Table 45, p. 106]. 16 000 cycles
 * last at least 94 us (at 170 MHz), over twice that, and at most 2 ms (at the 8 MHz the core
 * runs at during the change), far inside the watchdog's 1.88 s (bootloader/handover.c). */
#define WAIT_CYCLES         16000U

static volatile uint32_t milliseconds;

uint32_t cycles_now(void)
{
    return DWT->CYCCNT;
}

static void cycles_start(void)
{
    /* The DWT cycle counter runs only with TRCENA set in DEMCR [RM0440 Rev 9, Sec. 47.10,
     * p. 2102] and CYCCNTENA (bit 0) set in DWT_CTRL [RM0440 Rev 9, Sec. 47.13, p. 2104]. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* Wait until (*reg & mask) == want, at most WAIT_CYCLES. */
static bool wait_bits(const volatile uint32_t *reg, uint32_t mask, uint32_t want)
{
    uint32_t start = cycles_now();

    /* Unsigned subtraction stays right when the counter wraps. */
    while ((*reg & mask) != want) {
        if ((cycles_now() - start) > WAIT_CYCLES) {
            return false;
        }
    }
    return true;
}

static void wait_cycles(uint32_t cycles)
{
    uint32_t start = cycles_now();

    while ((cycles_now() - start) < cycles) {
    }
}

static bool set_flash_latency(uint32_t latency)
{
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY_Msk) | latency;
    /* Read back until the new value is in use [RM0440 Rev 9, Sec. 5.3.3, p. 191, step 2]. */
    return wait_bits(&FLASH->ACR, FLASH_ACR_LATENCY_Msk, latency);
}

bool core_clock_start(void)
{
    cycles_start();

    /* PWR registers ignore writes until their bus clock is on. */
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    (void)RCC->APB1ENR1;

    /* Range 1 normal -> boost mode, steps 1-5 [RM0440 Rev 9, Sec. 6.1.5, p. 233];
     * above 150 MHz boost mode is needed [RM0440 Rev 9, Table 38, p. 232]. */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_HPRE_Msk) | RCC_CFGR_HPRE_DIV2;   /* 1: AHB / 2 */
    PWR->CR5 &= ~PWR_CR5_R1MODE;                                        /* 2: boost mode */
    /* 3: 4 wait states for HCLK <= 170 MHz in boost mode [RM0440 Rev 9, Table 29, p. 190];
     * prefetch on, to hide most of those wait states [RM0440 Rev 9, Sec. 5.3.4, p. 192]. */
    FLASH->ACR |= FLASH_ACR_PRFTEN;
    if (!set_flash_latency(FLASH_ACR_LATENCY_4WS)) {
        return false;
    }

    /* 4: PLL on, then make it the system clock. Fields can change only while it is off. */
    RCC->PLLCFGR = RCC_PLLCFGR_PLLSRC_HSI | (PLLM_FIELD << RCC_PLLCFGR_PLLM_Pos) |
                   (PLL_N << RCC_PLLCFGR_PLLN_Pos) | (PLLR_DIV2_FIELD << RCC_PLLCFGR_PLLR_Pos) |
                   RCC_PLLCFGR_PLLREN;
    RCC->CR |= RCC_CR_PLLON;
    if (!wait_bits(&RCC->CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY)) {
        return false;
    }
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW_Msk) | RCC_CFGR_SW_PLL;
    if (!wait_bits(&RCC->CFGR, RCC_CFGR_SWS_Msk, RCC_CFGR_SWS_PLL)) {
        return false;
    }

    wait_cycles(SETTLE_CYCLES);                                         /* 5: wait 1 us, */
    RCC->CFGR &= ~RCC_CFGR_HPRE_Msk;                                    /*    then AHB / 1 */
    return true;
}

/* SysTick counts processor clocks down from RELOAD to 0 [Arm DUI 0553A, Sec. 4.4.1,
 * p. 4-33 to 4-34]; RELOAD = 169 999 fits its 24 bits (SysTick_LOAD_RELOAD_Msk, core_cm4.h).
 * No interrupt priority is set: this handler and USART2's are a few instructions each, so
 * neither delays the other by more than a fraction of one byte time (86.8 us). */
void tick_start(void)
{
    SysTick->LOAD = CYCLES_PER_MS - 1U;
    SysTick->VAL = 0U;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;
}

/* Name fixed by ST's vector table (startup_stm32g431xx.s). */
void SysTick_Handler(void)
{
    milliseconds++;
}

uint32_t tick_ms(void)
{
    /* One aligned 32-bit read: the interrupt cannot split it. */
    return milliseconds;
}
