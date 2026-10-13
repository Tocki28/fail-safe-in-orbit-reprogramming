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
 * code with the bootloader except the boot-state module it must use to confirm, and so it also
 * defines the fault reaction that module calls for an impossible value (common/fault.h).
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
#include "fault.h"
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

/**
 * @brief Starts USART2 transmit on PA2 at 115200 baud from HSI16.
 *
 * The same register steps as the bootloader's uart_init (bootloader/uart.c), with the divider
 * for 16 MHz: this test application never changes the clock.
 */
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

/**
 * @brief Sends one character on USART2, waiting for TXE.
 *
 * TXE: TDR has moved to the shift register [RM0440 Rev 9, Sec. 40.8.10, p. 1767]. The wait has no
 * limit of its own: a test application on the bench; the watchdog the bootloader started ends a
 * transmitter that never empties.
 *
 * @param[in]     c  character to send
 */
static void put_char(char c)
{
    while ((USART2->ISR & USART_ISR_TXE_TXFNF) == 0U) {
    }
    USART2->TDR = (uint8_t)c;
}

/**
 * @brief Sends a zero-terminated string on USART2.
 *
 * @param[in]     text  string to send
 */
static void put_text(const char *text)
{
    while (*text != '\0') {
        put_char(*text);
        text++;
    }
}

/**
 * @brief Sends a number on USART2 as decimal digits without leading zeros.
 *
 * @param[in]     value  number to send
 */
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

/**
 * @brief Sends one line: "APP <slot> v<version> <what>".
 *
 * @param[in]     what  text after the version
 */
static void say(const char *what)
{
    put_text("APP " APP_NAME " v");
    put_decimal(APP_VERSION);
    put_text(" ");
    put_text(what);
    put_text("\r\n");
}

/**
 * @brief Waits 1 s: 1000 SysTick COUNTFLAGs of 1 ms each.
 *
 * COUNTFLAG is set each time the count reaches 0 and cleared by the read [Arm DUI 0553A,
 * Sec. 4.4.1, p. 4-33 to 4-34]. The waits have no limit of their own: a stopped SysTick is ended
 * by the watchdog the bootloader started.
 */
static void wait_one_second(void)
{
    for (uint32_t ms = 0U; ms < MS_PER_SECOND; ms++) {
        while ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) == 0U) {
        }
    }
}

/**
 * @brief Confirms this slot in the boot state if it is the trial slot.
 *
 * Only the trial slot confirms itself; a slot running as confirmed or as fallback must not
 * touch the record, or it could confirm a trial that never ran.
 */
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

/**
 * @brief The fault reaction for common/boot_state.c (common/fault.h): one line, then a software
 * reset.
 *
 * The test application needs only what the bench sees: the line says where, and the reset
 * (CMSIS NVIC_SystemReset, SYSRESETREQ [Arm DUI 0553A, Sec. 4.3.5, Table 4-17, p. 4-17]) hands
 * back to the bootloader, which reads everything again. The line also carries the fault-location
 * code (decision 3, common/fault_codes.h), in decimal.
 *
 * @param[in]     location  the fault-location code (common/fault_codes.h)
 * @param[in]     where     the function that found the impossible value
 * @note Does not return.
 */
void fault_impossible_value(uint8_t location, const char *where)
{
    say("corrupted value; restarting");
    put_text(where);
    put_text(", fault location ");
    put_decimal(location);
    put_text("\r\n");
    NVIC_SystemReset();
}

/**
 * @brief Prints a line every second, reloads the watchdog, and after 3 s confirms itself or
 * hangs, by build flag.
 *
 * @note Does not return.
 */
int main(void)
{
    /* Own vector table, also when started by a debugger instead of the bootloader. */
    SCB->VTOR = (uint32_t)CODE_START;
    uart_init();
    SysTick->LOAD = SYSTICK_RELOAD;
    SysTick->VAL = 0U;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;   /* no interrupt */

    for (uint32_t second = 1U;; second++) {   /* endless: the application's main loop */
        say("running");
        if (second == DECISION_SECOND) {
            if (APP_HANG != 0) {
                say("hanging: watchdog no longer reloaded");
                for (;;) {   /* endless: the planned hang; the watchdog restarts the chip */
                }
            }
            confirm_if_trial();
        }
        IWDG->KR = IWDG_KEY_RELOAD;
        wait_one_second();
    }
}
