/*
 * uart.c — blocking USART2 transmit, register level. Never drives PB8: on this board PB8 is
 * BOOT0 [UM2397 Rev 2, p. 23] and drives the LED [UM2397 Rev 2, p. 16-17], and the design keeps
 * firmware off the boot pin.
 */
#include "uart.h"

#include "clock.h"
#include "stm32g4xx.h"

#define BAUD_RATE        115200U
/* Oversampling by 16: BRR = kernel clock / baud [RM0440 Rev 9, Sec. 40.5.7, p. 1707-1708].
 * USART2's kernel clock is PCLK1 after reset (USART2SEL = 00, Sec. 7.4.26, p. 324), and PCLK1
 * = 170 MHz with the APB1 divider left at 1. Rounded: 1476, 0.02 % fast. */
#define BRR_VALUE        ((CORE_CLOCK_HZ + (BAUD_RATE / 2U)) / BAUD_RATE)
/* PA2 is USART2_TX as alternate function 7 [DS12589 Rev 6, Table 13, p. 61]. */
#define PA2_AF_USART2    7U
#define GPIO_MODE_AF     2U
#define DECIMAL_DIGITS   10U   /* 2^32 - 1 has 10 digits */

static void pulse_reset(volatile uint32_t *reset_register, uint32_t bit)
{
    *reset_register |= bit;
    *reset_register &= ~bit;
}

void uart_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    RCC->APB1ENR1 |= RCC_APB1ENR1_USART2EN;
    /* Read back so both clocks run before the first register write (as ST's HAL does). */
    (void)RCC->APB1ENR1;

    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~GPIO_AFRL_AFSEL2_Msk) | (PA2_AF_USART2 << GPIO_AFRL_AFSEL2_Pos);
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODE2_Msk) | (GPIO_MODE_AF << GPIO_MODER_MODE2_Pos);

    /* 8N1 is the reset setting of CR1/CR2; only the speed and the enables are written. */
    USART2->BRR = BRR_VALUE;
    USART2->CR1 = USART_CR1_TE | USART_CR1_UE;
}

static void put_char(char c)
{
    while ((USART2->ISR & USART_ISR_TXE_TXFNF) == 0U) {
    }
    USART2->TDR = (uint8_t)c;
}

void uart_put(const char *text)
{
    while (*text != '\0') {
        put_char(*text);
        text++;
    }
}

void uart_put_decimal(uint32_t value)
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

void uart_off(void)
{
    /* TC: the shift register is empty, the last stop bit is out [RM0440 Rev 9, Sec. 40.5.5, p. 1699]. */
    while ((USART2->ISR & USART_ISR_TC) == 0U) {
    }
    pulse_reset(&RCC->APB1RSTR1, RCC_APB1RSTR1_USART2RST);
    /* GPIOA's reset state keeps PA13/PA14 (SWD) in alternate-function mode (MODER reset value
     * 0xABFF FFFF [RM0440 Rev 9, Sec. 9.4.1, p. 355]), so a debugger stays connected. */
    pulse_reset(&RCC->AHB2RSTR, RCC_AHB2RSTR_GPIOARST);
    RCC->APB1ENR1 &= ~RCC_APB1ENR1_USART2EN;
    RCC->AHB2ENR &= ~RCC_AHB2ENR_GPIOAEN;
}
