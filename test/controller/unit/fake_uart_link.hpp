/*
 * fake_uart_link.hpp — data link A's sending side (src/controller/firmware/data-link/
 * uart_link.hpp) on the host: UartLink::send copies each frame into a buffer the test reads
 * back, and can be made to fail as a stalled transmitter does; UartLink::waitSent counts its
 * calls. Only the two functions FrameSender calls are defined (fake_uart_link.cpp); a test that
 * called any other UartLink function would not link.
 */
#ifndef FAKE_UART_LINK_HPP
#define FAKE_UART_LINK_HPP

#include <array>
#include <cstdint>

/* Room for every frame one test sends: up to 600 frames of the largest size (84 bytes). */
constexpr uint32_t kFakeLinkBytes = 50000U;

struct FakeLink {
    std::array<uint8_t, kFakeLinkBytes> sent;   /* every byte sent, in order */
    uint32_t sent_bytes;
    uint32_t fail_sends;    /* the next this many UartLink::send calls fail, sending nothing */
    uint32_t failed_sends;  /* UartLink::send calls that failed */
    uint32_t drains;        /* UartLink::waitSent calls */
};

extern FakeLink fake_link;

/* Empty buffer, sends succeed. */
void fakeLinkReset();

#endif /* FAKE_UART_LINK_HPP */
