/*
 * core_clock.hpp — the firmware's time base: the core clock at 170 MHz, the cycle counter that
 * bounds every wait, and the millisecond tick.
 *
 * C++: class CoreClock (C: core_clock.c and its file-static millisecond count). The count is
 * the object's only state; main.cpp owns the object. Functions that only touch chip registers
 * (there is one RCC, one DWT, one SysTick) are static; the two that touch the count are not.
 * SysTick_Handler moved to main.cpp, next to the object it forwards to.
 */
#ifndef CORE_CLOCK_HPP
#define CORE_CLOCK_HPP

#include <cstdint>

/* The chip's highest core clock [DS12589 Rev 6, p. 1]. */
constexpr uint32_t kCoreClockHz = 170000000U;
/* The clock after reset, and the one the firmware stays on when the PLL does not start
 * (decision C): HSI16 [RM0440 Rev 9, Sec. 5.3.3, p. 190]. */
constexpr uint32_t kHsi16Hz = 16000000U;
constexpr uint32_t kMsPerSecond = 1000U;
/* Cycles in 1 ms at 170 MHz. The waits that count cycles (core_clock.cpp, data-link/
 * uart_link.cpp) are sized with it; at 16 MHz after a clock fault the same count lasts 10.6 times
 * longer (computed: 170 / 16), still bounded. */
constexpr uint32_t kCyclesPerMs = kCoreClockHz / kMsPerSecond;

class CoreClock {
public:
    /* Start the core's cycle counter, then HSI16 -> PLL -> 170 MHz (Range 1 boost mode, 4 flash
     * wait states). Returns the core clock now in use: kCoreClockHz; kHsi16Hz if a step towards
     * 170 MHz did not finish and the core went back to HSI16 (decision C); 0 if the cycle counter
     * does not count or the switch back to HSI16 did not finish: no usable clock. */
    [[nodiscard]] static uint32_t start();

    /* Core cycles since start(); wraps every 25 s at 170 MHz. */
    static uint32_t cyclesNow();

    /* SysTick interrupt once per millisecond at core_hz, the clock start() returned (kCoreClockHz
     * or kHsi16Hz; decision C). */
    static void startTick(uint32_t core_hz);

    /* Milliseconds since startTick(); wraps after 49.7 days. */
    uint32_t tickMs() const;

    /* The SysTick interrupt's work (C: the body of SysTick_Handler). */
    void onTick();

private:
    /* Written by the interrupt, read by the main loop, hence volatile: the compiler must read
     * it every time, not keep a copy in a register. */
    volatile uint32_t milliseconds_ = 0U;
};

#endif /* CORE_CLOCK_HPP */
