/*
 * uart_link.hpp — data link A on this bench: USART2 (PA2 TX, PA3 RX), wired to the on-board
 * ST-LINK's virtual COM port [UM2397 Rev 2, Sec. 6.6.4, Table 8, p. 16]. 115200 baud, 8 data
 * bits, no parity, 1 stop bit (Interfaces Sec. 6.2).
 *
 * Data link A's own code. REQ-099: the two data links share no software, so data link B
 * (CAN, not on this bench) gets its own driver; nothing here is written for it to reuse.
 *
 * C++: class UartLink (C: uart_link.c and its file-static ring, counters and frame parser).
 * The ring and the parser are members; main.cpp owns the one object. USART2_IRQHandler moved
 * to main.cpp and forwards to onInterrupt().
 */
#ifndef UART_LINK_HPP
#define UART_LINK_HPP

#include <array>
#include <cstdint>

#include "frame.hpp"

/* Data link A's speed (Interfaces Sec. 6.2). */
constexpr uint32_t kBaudRate = 115200U;

class UartLink {
public:
    /**
     * @brief The USART2 baud-rate divider for a kernel clock: BRR = kernel clock / baud, rounded.
     *
     * Oversampling by 16: BRR = kernel clock / baud [RM0440 Rev 9, Sec. 40.5.7, p. 1707-1708].
     * USART2's kernel clock is PCLK1 after reset (USART2SEL = 00, Sec. 7.4.26, p. 324), and PCLK1
     * is the core clock with the APB1 divider left at 1. 1476 at 170 MHz (0.02 % fast), 139 at
     * 16 MHz (0.08 % slow), computed. A function of the clock, not one constant, since decision C
     * (the firmware may run at 16 MHz after a clock fault).
     *
     * @param[in] kernel_hz  USART2 kernel clock in Hz
     * @return the value for USART2_BRR
     */
    static constexpr uint32_t brrFor(uint32_t kernel_hz)
    {
        return (kernel_hz + (kBaudRate / 2U)) / kBaudRate;
    }

    /* USART2, its pins and its receive interrupt, at the core clock in use (core_hz from
     * CoreClock::start()). Needs CoreClock::startTick(). */
    void init(uint32_t core_hz);

    /* Take the bytes received so far through the frame parser. True when a frame that passed
     * every check is complete; `frame` then describes it until the next call. */
    [[nodiscard]] bool receive(Frame &frame);

    /* Send one whole frame (from buildFrame). False if the transmitter stalled; the frame is
     * then cut short, and the receiver drops it on its check value. */
    [[nodiscard]] bool send(const uint8_t *bytes, uint32_t size);

    /** @brief Wait, bounded, until the last byte sent has left the transmitter. */
    [[nodiscard]] bool waitSent();

    /* The USART2 interrupt's work (C: the body of USART2_IRQHandler). now_ms is the
     * millisecond count, read by the handler: C++ keeps the link free of a pointer to the
     * clock, which would also move this whole object from .bss to .data (main.cpp). */
    void onInterrupt(uint32_t now_ms);

private:
    /* Ring size. The main loop's longest pause in taking bytes out was a send. The largest
     * frame is 1044 bytes (frame.hpp), 90.6 ms on the line at 10 bits per byte; at most 1044
     * bytes arrive meanwhile. 2048, the next power of two, holds that, and divides 2^32, so the
     * free-running counters below index it with a mask. RAM: 2048 x (1 + 4) bytes = 10 KB of
     * the 32 KB. Step 2c: the longest pause is now UPLOAD END's image check (tens of ms; 2048
     * bytes last 178 ms on the line). During a flash erase or write this interrupt cannot run
     * at all; the upload's stop-and-wait rule keeps the sender quiet then
     * (update-receiver/update_receiver.hpp). */
    static constexpr uint32_t kRingBytes = 2048U;
    static constexpr uint32_t kRingMask = kRingBytes - 1U;

    void ringPut(uint8_t byte, uint32_t ms);

    /* Shared with the interrupt, hence volatile: the compiler must not reorder or cache them.
     * ring_head_ is written only by the interrupt, ring_tail_ only by the main loop; both count
     * bytes since start, and head - tail is the number waiting. */
    std::array<volatile uint8_t, kRingBytes> ring_byte_{};
    std::array<volatile uint32_t, kRingBytes> ring_ms_{};
    volatile uint32_t ring_head_ = 0U;
    volatile uint32_t ring_tail_ = 0U;
    /* Bytes lost to a full ring or a receiver overrun. Read with the debugger; the health
     * monitor (step 2e) will report it. */
    volatile uint32_t lost_bytes_ = 0U;

    FrameParser parser_;
};

#endif /* UART_LINK_HPP */
