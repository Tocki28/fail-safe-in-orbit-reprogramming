/*
 * main.c — TEST application for the bootloader bench tests. Not the real firmware.
 *
 * One source, three link addresses (Makefile: SLOT=A, B or LR). Every second it prints
 * "APP <slot> v<version> running" on USART2 and reloads the watchdog the bootloader started.
 * After 3 s, by build flag:
 *   confirm  if it is the trial slot, it confirms itself in the boot state
 *            (confirmed = this slot, trial = none, failed starts = 0);
 *   hang     it stops reloading the watchdog and loops, so the watchdog restarts the chip
 *            and the bootloader counts a failed start.
 * It runs on HSI16, the clock the bootloader hands over (bootloader/handover.h). It shares no
 * code with the bootloader except the boot-state module it must use to confirm.
 *
 * Build flags (Makefile):
 *   APP_SLOT     BOOT_SLOT_A, BOOT_SLOT_B, or BOOT_SLOT_NONE for the last-resort build
 *   APP_NAME     "A", "B" or "LR"
 *   APP_VERSION  the version written into the image header by tools/make_image.py
 *   APP_HANG     1 = hang after 3 s, 0 = confirm after 3 s
 *   CODE_START   the layout.h address this build is linked at
 */
#include <stdint.h>

#include "boot_state.h"
#include "layout.h"
#include "stm32g4xx.h"

/* HSI16: the core clock after reset [RM0440 Rev 9, Sec. 5.3.3, p. 190]. */
#define CORE_CLOCK_HZ      16000000U
#define BAUD_RATE          115200U
/* BRR = kernel clock / baud [RM0440 Rev 9, Sec. 40.5.7, p. 1707-1708]: 139, 0.08 % slow. */
#define BRR_VALUE          ((CORE_CLOCK_HZ + (BAUD_RATE / 2U)) / BAUD_RATE)
/* PA2 = USART2_TX on alternate function 7 [DS12589 Rev 6, Table 13, p. 61]. */
#define PA2_AF_USART2      7U
#define GPIO_MODE_AF       2U
/* Reload key [RM0440 Rev 9, Sec. 35.3.2, p. 1557]. */
#define IWDG_KEY_RELOAD    0xAAAAU
/* SysTick counts processor clocks down from RELOAD and sets COUNTFLAG at 0
 * [Arm DUI 0553A, Sec. 4.4.1, p. 4-33 to 4-34]: one flag per millisecond. */
#define SYSTICK_RELOAD     ((CORE_CLOCK_HZ / 1000U) - 1U)
#define MS_PER_SECOND      1000U
/* The bench plan: confirm or hang 3 s after start. */
#define DECISION_SECOND    3U
#define DECIMAL_DIGITS     10U

static void uart_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    RCC->APB1ENR1 |= RCC_APB1ENR1_USART2EN;
    (void)RCC->APB1ENR1;
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~GPIO_AFRL_AFSEL2_Msk) | (PA2_AF_USART2 << GPIO_AFRL_AFSEL2_Pos);
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODE2_Msk) | (GPIO_MODE_AF << GPIO_MODER_MODE2_Pos);
    USART2->BRR = BRR_VALUE;
    USART2->CR1 = USART_CR1_TE | USART_CR1_UE;
}

static void put_char(char c)
{
    while ((USART2->ISR & USART_ISR_TXE_TXFNF) == 0U) {
    }
    USART2->TDR = (uint8_t)c;
}

static void put_text(const char *text)
{
    while (*text != '\0') {
        put_char(*text);
        text++;
    }
}

static void put_decimal(uint32_t value)
{
    char digits[DECIMAL_DIGITS];
    uint32_t count = 0U;

    do {
        digits[count] = (char)('0' + (value % 10U));
        count++;
        value /= 10U;
    } while (value != 0U);
    while (count > 0U) {
        count--;
        put_char(digits[count]);
    }
}

/* One line: "APP <slot> v<version> <what>". */
static void say(const char *what)
{
    put_text("APP " APP_NAME " v");
    put_decimal(APP_VERSION);
    put_text(" ");
    put_text(what);
    put_text("\r\n");
}

static void wait_one_second(void)
{
    for (uint32_t ms = 0U; ms < MS_PER_SECOND; ms++) {
        while ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) == 0U) {
        }
    }
}

/* Only the trial slot confirms itself; a slot running as confirmed or as fallback must not
 * touch the record, or it could confirm a trial that never ran. */
static void confirm_if_trial(void)
{
    boot_state_t state;

    if (APP_SLOT == BOOT_SLOT_NONE) {
        return;   /* the last-resort firmware is never a trial */
    }
    if (!boot_state_read(&state)) {
        say("no valid boot state; nothing to confirm");
        return;
    }
    if (state.trial != APP_SLOT) {
        say("not on trial; nothing to confirm");
        return;
    }
    state.confirmed = APP_SLOT;
    state.trial = BOOT_SLOT_NONE;
    state.failed_starts = 0U;
    say(boot_state_write(&state) ? "confirmed itself" : "confirm FAILED: boot state write");
}

int main(void)
{
    /* Own vector table, also when started by a debugger instead of the bootloader. */
    SCB->VTOR = (uint32_t)CODE_START;
    uart_init();
    SysTick->LOAD = SYSTICK_RELOAD;
    SysTick->VAL = 0U;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;   /* no interrupt */

    for (uint32_t second = 1U;; second++) {
        say("running");
        if (second == DECISION_SECOND) {
            if (APP_HANG != 0) {
                say("hanging: watchdog no longer reloaded");
                for (;;) {
                }
            }
            confirm_if_trial();
        }
        IWDG->KR = IWDG_KEY_RELOAD;
        wait_one_second();
    }
}
