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
constexpr uint32_t kCyclesPerMs = kCoreClockHz / 1000U;

class CoreClock {
public:
    /* Start the core's cycle counter, then HSI16 -> PLL -> 170 MHz (Range 1 boost mode, 4 flash
     * wait states). False if a step did not finish in time: the clock is then not usable. */
    [[nodiscard]] static bool start();

    /* Core cycles since start(); wraps every 25 s at 170 MHz. */
    static uint32_t cyclesNow();

    /* SysTick interrupt once per millisecond. Needs the core at kCoreClockHz. */
    static void startTick();

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
