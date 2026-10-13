/*
 * uart.c — blocking USART2 transmit, register level. Never drives PB8: on this board PB8 is
 * BOOT0 [UM2397 Rev 2, p. 23] and drives the LED [UM2397 Rev 2, p. 16-17], and the design keeps
 * firmware off the boot pin.
 *
 * Every wait gives up after BYTE_TIMEOUT_CYCLES on the cycle counter, and also after the same
 * number of loop passes (BYTE_TIMEOUT_PASSES), so a stopped cycle counter cannot make it
 * endless: a stalled transmitter must not hold the bootloader. After the first stall the text
 * is dropped, so a dead transmitter costs one timeout, not one per character.
 */
#include "uart.h"

#include <stdbool.h>

#include "clock.h"
#include "stm32g4xx.h"

#define BAUD_RATE        115200U
/* PA2 is USART2_TX as alternate function 7 [DS12589 Rev 6, Table 13, p. 61]. */
#define PA2_AF_USART2    7U
#define GPIO_MODE_AF     2U
#define DECIMAL_DIGITS   10U   /* 2^32 - 1 has 10 digits */
/* One byte needs 86.8 us at 115200 baud (10 bits). 1 ms of cycles at 170 MHz is over ten byte
 * times; at 16 MHz (clock fault) the same count lasts 10.6 ms. */
#define BYTE_TIMEOUT_CYCLES  (CORE_CLOCK_HZ / 1000U)
/* Backstop for a stopped cycle counter: as many passes of the wait loop as cycles above. A pass
 * reads the flag register and the counter, so it takes at least one core cycle: with a working
 * counter the cycle limit ends the wait first; with a dead counter the wait lasts at least
 * 170 000 cycles, which is 1 ms at 170 MHz and 10.6 ms at 16 MHz, the same lower bound as above.
 * The longest it can take is 170 000 x (cycles per pass): TBD, not measured. */
#define BYTE_TIMEOUT_PASSES  BYTE_TIMEOUT_CYCLES

static bool stalled = false;

/**
 * @brief Sets and then clears one bit of a peripheral reset register, which resets that
 * peripheral.
 *
 * @param[in,out] reset_register  peripheral reset register
 * @param[in]     bit             mask of the peripheral's reset bit
 */
static void pulse_reset(volatile uint32_t *reset_register, uint32_t bit)
{
    *reset_register |= bit;
    *reset_register &= ~bit;
}

/**
 * @brief Wait for a USART2_ISR flag, at most BYTE_TIMEOUT_CYCLES cycles or BYTE_TIMEOUT_PASSES
 * passes.
 *
 * @param[in]     flag  USART2_ISR flag mask
 * @return true if the flag is set, false on time-out
 */
static bool wait_flag(uint32_t flag)
{
    uint32_t start = cycles_now();

    for (uint32_t n = 0U; n < BYTE_TIMEOUT_PASSES; n++) {
        if ((USART2->ISR & flag) != 0U) {
            return true;
        }
        if ((cycles_now() - start) > BYTE_TIMEOUT_CYCLES) {
            return false;
        }
    }
    return (USART2->ISR & flag) != 0U;
}

/**
 * @brief Starts USART2 transmit on PA2 at 115200 baud.
 *
 * The baud rate divider comes from the clock in use (clock_hz(): 170 MHz, or 16 MHz after a
 * clock fault; HSI16 is assumed if the clock is not known).
 */
void uart_init(void)
{
    /* Oversampling by 16: BRR = kernel clock / baud [RM0440 Rev 9, Sec. 40.5.7, p. 1707-1708].
     * USART2's kernel clock is PCLK1 after reset (USART2SEL = 00, Sec. 7.4.26, p. 324), and
     * PCLK1 = the core clock with the APB1 divider left at 1: 1476 at 170 MHz (0.02 % fast),
     * 139 at 16 MHz (0.08 % slow). Clock not known (clock_hz() = 0): HSI16 is the best guess. */
    uint32_t hz = (clock_hz() != 0U) ? clock_hz() : HSI16_HZ;

    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    RCC->APB1ENR1 |= RCC_APB1ENR1_USART2EN;
    /* Read back so both clocks run before the first register write (as ST's HAL does). */
    (void)RCC->APB1ENR1;

    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~GPIO_AFRL_AFSEL2_Msk) | (PA2_AF_USART2 << GPIO_AFRL_AFSEL2_Pos);
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODE2_Msk) | (GPIO_MODE_AF << GPIO_MODER_MODE2_Pos);

    /* 8N1 is the reset setting of CR1/CR2; only the speed and the enables are written. */
    USART2->BRR = (hz + (BAUD_RATE / 2U)) / BAUD_RATE;
    USART2->CR1 = USART_CR1_TE | USART_CR1_UE;
}

/**
 * @brief Sends one character on USART2.
 *
 * After a wait has timed out, this and every later character is dropped.
 *
 * @param[in]     c  character to send
 */
static void put_char(char c)
{
    if (stalled) {
        return;
    }
    /* TXE: TDR has moved to the shift register and takes the next byte
     * [RM0440 Rev 9, Sec. 40.8.10, p. 1767]. */
    if (!wait_flag(USART_ISR_TXE_TXFNF)) {
        stalled = true;
        return;
    }
    USART2->TDR = (uint8_t)c;
}

/**
 * @brief Sends a zero-terminated string on USART2.
 *
 * @param[in]     text  string to send
 */
void uart_put(const char *text)
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

/**
 * @brief Waits, bounded, until the last bit has left USART2; the UART stays on.
 *
 * TC: the shift register is empty, the last stop bit is out [RM0440 Rev 9, Sec. 40.5.5,
 * p. 1699]. Why: the fault reaction (bootloader/fault.c) resets the chip right after its log
 * line, and a reset in the middle of a byte would cut the line short. The wait is bounded by
 * wait_flag (BYTE_TIMEOUT_CYCLES or BYTE_TIMEOUT_PASSES), so a stalled transmitter cannot hold
 * the chip; after a stall TC may never come and the wait just ends.
 */
void uart_wait_sent(void)
{
    /* Bounded; the caller resets or turns the UART off either way. */
    (void)wait_flag(USART_ISR_TC);
}

/**
 * @brief Waits for the last bit to leave, then resets USART2 and GPIOA and turns their clocks off.
 *
 * Leaves the SWD pins connected.
 */
void uart_off(void)
{
    uart_wait_sent();
    pulse_reset(&RCC->APB1RSTR1, RCC_APB1RSTR1_USART2RST);
    /* GPIOA's reset state keeps PA13/PA14 (SWD) in alternate-function mode (MODER reset value
     * 0xABFF FFFF [RM0440 Rev 9, Sec. 9.4.1, p. 355]), so a debugger stays connected. */
    pulse_reset(&RCC->AHB2RSTR, RCC_AHB2RSTR_GPIOARST);
    RCC->APB1ENR1 &= ~RCC_APB1ENR1_USART2EN;
    RCC->AHB2ENR &= ~RCC_AHB2ENR_GPIOAEN;
}
