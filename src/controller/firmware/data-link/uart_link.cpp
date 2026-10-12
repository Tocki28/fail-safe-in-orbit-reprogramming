/*
 * uart_link.cpp — data link A driver: USART2, register level.
 *
 * Receive: one interrupt per byte stores the byte and the millisecond it arrived in a ring.
 * The main loop takes bytes out and feeds the frame parser (frame.cpp), which measures the
 * 10 ms gap rule on those arrival times, so a busy main loop can neither make a gap nor hide
 * one. Reception never waits for the main loop or for a send (REQ-042).
 * Send: whole frames, byte by byte from the main loop; the firmware sends only answers and
 * reports (HEALTH, from step 2c), so it never talks over the link on its own (REQ-042).
 * Never drives PB8: on this board PB8 is BOOT0 and drives the LED [UM2397 Rev 2, p. 16-17].
 */
#include "uart_link.hpp"

#include "core_clock.hpp"
#include "stm32g4xx.h"

namespace {

constexpr uint32_t kBaudRate = 115200U;
/* Oversampling by 16: BRR = kernel clock / baud [RM0440 Rev 9, Sec. 40.5.7, p. 1707-1708].
 * USART2's kernel clock is PCLK1 after reset (USART2SEL = 00, Sec. 7.4.26, p. 324), and PCLK1
 * = 170 MHz with the APB1 divider left at 1. Rounded: 1476, 0.02 % fast. */
constexpr uint32_t kBrrValue = (kCoreClockHz + (kBaudRate / 2U)) / kBaudRate;
/* PA2 = USART2_TX and PA3 = USART2_RX, both alternate function 7 [DS12589 Rev 6, Table 13,
 * p. 61]; on this board both reach the ST-LINK (SB1, SB12 ON) [UM2397 Rev 2, Table 9, p. 17]. */
constexpr uint32_t kAf7Usart2 = 7U;
constexpr uint32_t kGpioModeAf = 2U;
/* One byte needs 86.8 us on the line; 1 ms without the transmit register emptying means the
 * transmitter has stopped. */
constexpr uint32_t kSendByteTimeoutCycles = kCyclesPerMs;

bool waitTransmitEmpty()
{
    uint32_t start = CoreClock::cyclesNow();

    /* TXE: TDR has moved to the shift register and takes the next byte
     * [RM0440 Rev 9, Sec. 40.8.10, p. 1767]. */
    while ((USART2->ISR & USART_ISR_TXE_TXFNF) == 0U) {
        if ((CoreClock::cyclesNow() - start) > kSendByteTimeoutCycles) {
            return false;
        }
    }
    return true;
}

}  // namespace

void UartLink::init()
{
    parser_.reset();
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    RCC->APB1ENR1 |= RCC_APB1ENR1_USART2EN;
    /* Read back so both clocks run before the first register write (as ST's HAL does). */
    (void)RCC->APB1ENR1;

    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~(GPIO_AFRL_AFSEL2_Msk | GPIO_AFRL_AFSEL3_Msk)) |
                    (kAf7Usart2 << GPIO_AFRL_AFSEL2_Pos) | (kAf7Usart2 << GPIO_AFRL_AFSEL3_Pos);
    GPIOA->MODER = (GPIOA->MODER & ~(GPIO_MODER_MODE2_Msk | GPIO_MODER_MODE3_Msk)) |
                   (kGpioModeAf << GPIO_MODER_MODE2_Pos) | (kGpioModeAf << GPIO_MODER_MODE3_Pos);

    /* CR1 and CR2 reset to 0 [RM0440 Rev 9, Sec. 40.8.1, p. 1740; Sec. 40.8.3, p. 1747]: 8 data
     * bits, no parity, FIFO off, 1 stop bit (STOP = 00, p. 1749). With the FIFO off, RXNE means
     * one byte waits in RDR. Only the speed and the enables are written: transmitter, receiver,
     * receive interrupt [RM0440 Rev 9, Sec. 40.8.2, p. 1746]. */
    USART2->BRR = kBrrValue;
    USART2->CR1 = USART_CR1_RXNEIE_RXFNEIE | USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
    NVIC_EnableIRQ(USART2_IRQn);
}

void UartLink::ringPut(uint8_t byte, uint32_t ms)
{
    uint32_t head = ring_head_;

    if ((head - ring_tail_) >= kRingBytes) {
        /* full: this byte is lost, and its frame fails its check value */
        lost_bytes_ = lost_bytes_ + 1U;
        return;
    }
    ring_byte_[head & kRingMask] = byte;
    ring_ms_[head & kRingMask] = ms;
    ring_head_ = head + 1U;   /* last, so the main loop never sees a half-written entry */
}

void UartLink::onInterrupt(uint32_t now_ms)
{
    uint32_t status = USART2->ISR;

    /* Reading RDR clears RXNE [RM0440 Rev 9, Sec. 40.8.10, p. 1768]. A byte with a framing
     * or noise error is kept: the frame's check value rejects it. */
    if ((status & USART_ISR_RXNE_RXFNE) != 0U) {
        ringPut(static_cast<uint8_t>(USART2->RDR), now_ms);
    }
    /* Overrun: a byte arrived before the last was read, and is lost. ORE also raises this
     * interrupt [RM0440 Rev 9, Table 378, p. 1739] and stays set until ORECF is written
     * [RM0440 Rev 9, Sec. 40.5.6, p. 1703]; uncleared, the interrupt would repeat for ever. */
    if ((status & USART_ISR_ORE) != 0U) {
        USART2->ICR = USART_ICR_ORECF;
        lost_bytes_ = lost_bytes_ + 1U;
    }
}

bool UartLink::receive(Frame &frame)
{
    /* At most one ring's worth per call, so the main loop returns to its other work (the
     * watchdog) even while bytes keep coming. */
    for (uint32_t taken = 0U; (taken < kRingBytes) && (ring_tail_ != ring_head_); taken++) {
        uint32_t tail = ring_tail_;
        uint8_t byte = ring_byte_[tail & kRingMask];
        uint32_t ms = ring_ms_[tail & kRingMask];

        ring_tail_ = tail + 1U;
        if (parser_.feed(byte, ms, frame)) {
            return true;
        }
    }
    return false;
}

bool UartLink::send(const uint8_t *bytes, uint32_t size)
{
    for (uint32_t i = 0U; i < size; i++) {
        if (!waitTransmitEmpty()) {
            return false;
        }
        USART2->TDR = bytes[i];
    }
    return true;
}
