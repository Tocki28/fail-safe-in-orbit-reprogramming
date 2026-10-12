/*
 * uart_link.h — data link A on this bench: USART2 (PA2 TX, PA3 RX), wired to the on-board
 * ST-LINK's virtual COM port [UM2397 Rev 2, Sec. 6.6.4, Table 8, p. 16]. 115200 baud, 8 data
 * bits, no parity, 1 stop bit (Interfaces Sec. 6.2).
 *
 * Data link A's own code. REQ-099: the two data links share no software, so data link B
 * (CAN, not on this bench) gets its own driver; nothing here is written for it to reuse.
 */
#ifndef UART_LINK_H
#define UART_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "frame.h"

/* USART2, its pins and its receive interrupt. Needs core_clock_start() and tick_start(). */
void uart_link_init(void);

/* Take the bytes received so far through the frame parser. True when a frame that passed
 * every check is complete; *frame then describes it until the next call. */
bool uart_link_receive(frame_t *frame);

/* Send one whole frame (from frame_build). False if the transmitter stalled; the frame is
 * then cut short, and the receiver drops it on its check value. */
bool uart_link_send(const uint8_t *bytes, uint32_t size);

#endif /* UART_LINK_H */
