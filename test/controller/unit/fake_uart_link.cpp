/*
 * fake_uart_link.cpp — see fake_uart_link.hpp.
 */
#include "fake_uart_link.hpp"

#include <cstring>

#include "uart_link.hpp"

FakeLink fake_link;

/**
 * @brief Empties the buffer and makes sends succeed.
 */
void fakeLinkReset()
{
    fake_link = FakeLink{};
}

/**
 * @brief Host stand-in for UartLink::send: copies the frame into fake_link.sent.
 *
 * @param[in]     bytes  the frame
 * @param[in]     size   its size in bytes
 * @return false while fake_link.fail_sends is above 0 (and counts it down), or when the buffer
 *         is full; true otherwise
 */
bool UartLink::send(const uint8_t *bytes, uint32_t size)
{
    if (fake_link.fail_sends > 0U) {
        fake_link.fail_sends--;
        fake_link.failed_sends++;
        return false;
    }
    if (size > (kFakeLinkBytes - fake_link.sent_bytes)) {
        fake_link.failed_sends++;
        return false;
    }
    std::memcpy(&fake_link.sent[fake_link.sent_bytes], bytes, size);
    fake_link.sent_bytes += size;
    return true;
}

/**
 * @brief Host stand-in for UartLink::waitSent: counts the call.
 *
 * @return true
 */
bool UartLink::waitSent()
{
    fake_link.drains++;
    return true;
}
