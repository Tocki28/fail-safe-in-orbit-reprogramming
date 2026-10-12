/*
 * uart.h — status text out of USART2 (PA2), which the Nucleo's ST-LINK passes on as a virtual
 * COM port [UM2397 Rev 2, Sec. 6.6.4, Table 8, p. 16]. Transmit only: the bootloader takes no
 * input. 115200 baud, 8 data bits, no parity, 1 stop bit.
 */
#ifndef UART_H
#define UART_H

#include <stdint.h>

/* Needs the core at CORE_CLOCK_HZ (clock_to_170mhz) first. */
void uart_init(void);
void uart_put(const char *text);
void uart_put_decimal(uint32_t value);
/* Wait until the last bit has left, then put USART2 and GPIOA back to their reset state. */
void uart_off(void);

#endif /* UART_H */
