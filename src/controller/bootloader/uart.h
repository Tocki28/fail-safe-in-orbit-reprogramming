/*
 * uart.h — status text out of USART2 (PA2), which the Nucleo's ST-LINK passes on as a virtual
 * COM port [UM2397 Rev 2, Sec. 6.6.4, Table 8, p. 16]. Transmit only: the bootloader takes no
 * input. 115200 baud, 8 data bits, no parity, 1 stop bit.
 */
#ifndef UART_H
#define UART_H

#include <stdint.h>

/* Sets the baud rate for the clock in use (clock_hz(): 170 MHz after clock_to_170mhz, 16 MHz
 * after a clock fault, HSI16 assumed if the clock is not known). Call after clock_to_170mhz. */
void uart_init(void);
/* Send a zero-terminated string. */
void uart_put(const char *text);
/* Send a number as decimal digits. */
void uart_put_decimal(uint32_t value);
/* Send a number as "0x" and eight hexadecimal digits (upper case), leading zeros kept. */
void uart_put_hex32(uint32_t value);
/* Wait, bounded, until the last bit has left; USART2 stays on. Used before a software reset. */
void uart_wait_sent(void);
/* Wait until the last bit has left, then put USART2 and GPIOA back to their reset state. */
void uart_off(void);

#endif /* UART_H */
