/*
 * uart_link.c — data link A driver: USART2, register level.
 *
 * Receive: one interrupt per byte stores the byte and the millisecond it arrived in a ring.
 * The main loop takes bytes out and feeds the frame parser (frame.c), which measures the
 * 10 ms gap rule on those arrival times, so a busy main loop can neither make a gap nor hide
 * one. Reception never waits for the main loop or for a send (REQ-042).
 * Send: whole frames, byte by byte from the main loop; the firmware sends only answers (and,
 * from step 2e, reports), so it never talks over the link on its own (REQ-042).
 * Never drives PB8: on this board PB8 is BOOT0 and drives the LED [UM2397 Rev 2, p. 16-17].
 */
#include "uart_link.h"

#include "core_clock.h"
#include "stm32g4xx.h"

#define BAUD_RATE         115200U
/* Oversampling by 16: BRR = kernel clock / baud [RM0440 Rev 9, Sec. 40.5.7, p. 1707-1708].
 * USART2's kernel clock is PCLK1 after reset (USART2SEL = 00, Sec. 7.4.26, p. 324), and PCLK1
 * = 170 MHz with the APB1 divider left at 1. Rounded: 1476, 0.02 % fast. */
#define BRR_VALUE         ((CORE_CLOCK_HZ + (BAUD_RATE / 2U)) / BAUD_RATE)
/* PA2 = USART2_TX and PA3 = USART2_RX, both alternate function 7 [DS12589 Rev 6, Table 13,
 * p. 61]; on this board both reach the ST-LINK (SB1, SB12 ON) [UM2397 Rev 2, Table 9, p. 17]. */
#define AF7_USART2        7U
#define GPIO_MODE_AF      2U
/* Ring size. The main loop's longest pause in taking bytes out is a send. The largest frame
 * is 1044 bytes (frame.h), 90.6 ms on the line at 10 bits per byte; at most 1044 bytes arrive
 * meanwhile. 2048, the next power of two, holds that, and divides 2^32, so the free-running
 * counters below index it with a mask. (This step's longest send: two 26-byte answers, 4.5 ms.)
 * RAM: 2048 x (1 + 4) bytes = 10 KB of the 32 KB. */
#define RING_BYTES        2048U
#define RING_MASK         (RING_BYTES - 1U)
/* One byte needs 86.8 us on the line; 1 ms without the transmit register emptying means the
 * transmitter has stopped. */
#define SEND_BYTE_TIMEOUT_CYCLES  CYCLES_PER_MS

/* Shared with the interrupt, hence volatile: the compiler must not reorder or cache them.
 * ring_head is written only by the interrupt, ring_tail only by the main loop; both count
 * bytes since start, and head - tail is the number waiting. */
static volatile uint8_t ring_byte[RING_BYTES];
static volatile uint32_t ring_ms[RING_BYTES];
static volatile uint32_t ring_head;
static volatile uint32_t ring_tail;
/* Bytes lost to a full ring or a receiver overrun. Read with the debugger; the health
 * monitor (step 2e) will report it. */
static volatile uint32_t lost_bytes;

static frame_parser_t parser;

void uart_link_init(void)
{
    frame_parser_init(&parser);
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    RCC->APB1ENR1 |= RCC_APB1ENR1_USART2EN;
    /* Read back so both clocks run before the first register write (as ST's HAL does). */
    (void)RCC->APB1ENR1;

    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~(GPIO_AFRL_AFSEL2_Msk | GPIO_AFRL_AFSEL3_Msk)) |
                    (AF7_USART2 << GPIO_AFRL_AFSEL2_Pos) | (AF7_USART2 << GPIO_AFRL_AFSEL3_Pos);
    GPIOA->MODER = (GPIOA->MODER & ~(GPIO_MODER_MODE2_Msk | GPIO_MODER_MODE3_Msk)) |
                   (GPIO_MODE_AF << GPIO_MODER_MODE2_Pos) | (GPIO_MODE_AF << GPIO_MODER_MODE3_Pos);

    /* CR1 and CR2 reset to 0 [RM0440 Rev 9, Sec. 40.8.1, p. 1740; Sec. 40.8.3, p. 1747]: 8 data
     * bits, no parity, FIFO off, 1 stop bit (STOP = 00, p. 1749). With the FIFO off, RXNE means
     * one byte waits in RDR. Only the speed and the enables are written: transmitter, receiver,
     * receive interrupt [RM0440 Rev 9, Sec. 40.8.2, p. 1746]. */
    USART2->BRR = BRR_VALUE;
    USART2->CR1 = USART_CR1_RXNEIE_RXFNEIE | USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
    NVIC_EnableIRQ(USART2_IRQn);
}

static void ring_put(uint8_t byte, uint32_t ms)
{
    uint32_t head = ring_head;

    if ((head - ring_tail) >= RING_BYTES) {
        lost_bytes++;   /* full: this byte is lost, and its frame fails its check value */
        return;
    }
    ring_byte[head & RING_MASK] = byte;
    ring_ms[head & RING_MASK] = ms;
    ring_head = head + 1U;   /* last, so the main loop never sees a half-written entry */
}

/* Name fixed by ST's vector table (startup_stm32g431xx.s). */
void USART2_IRQHandler(void)
{
    uint32_t status = USART2->ISR;

    /* Reading RDR clears RXNE [RM0440 Rev 9, Sec. 40.8.10, p. 1768]. A byte with a framing
     * or noise error is kept: the frame's check value rejects it. */
    if ((status & USART_ISR_RXNE_RXFNE) != 0U) {
        ring_put((uint8_t)USART2->RDR, tick_ms());
    }
    /* Overrun: a byte arrived before the last was read, and is lost. ORE also raises this
     * interrupt [RM0440 Rev 9, Table 378, p. 1739] and stays set until ORECF is written
     * [RM0440 Rev 9, Sec. 40.5.6, p. 1703]; uncleared, the interrupt would repeat for ever. */
    if ((status & USART_ISR_ORE) != 0U) {
        USART2->ICR = USART_ICR_ORECF;
        lost_bytes++;
    }
}

bool uart_link_receive(frame_t *frame)
{
    /* At most one ring's worth per call, so the main loop returns to its other work (the
     * watchdog) even while bytes keep coming. */
    for (uint32_t taken = 0U; (taken < RING_BYTES) && (ring_tail != ring_head); taken++) {
        uint32_t tail = ring_tail;
        uint8_t byte = ring_byte[tail & RING_MASK];
        uint32_t ms = ring_ms[tail & RING_MASK];

        ring_tail = tail + 1U;
        if (frame_parser_feed(&parser, byte, ms, frame)) {
            return true;
        }
    }
    return false;
}

static bool wait_transmit_empty(void)
{
    uint32_t start = cycles_now();

    /* TXE: TDR has moved to the shift register and takes the next byte
     * [RM0440 Rev 9, Sec. 40.8.10, p. 1767]. */
    while ((USART2->ISR & USART_ISR_TXE_TXFNF) == 0U) {
        if ((cycles_now() - start) > SEND_BYTE_TIMEOUT_CYCLES) {
            return false;
        }
    }
    return true;
}

bool uart_link_send(const uint8_t *bytes, uint32_t size)
{
    for (uint32_t i = 0U; i < size; i++) {
        if (!wait_transmit_empty()) {
            return false;
        }
        USART2->TDR = bytes[i];
    }
    return true;
}
