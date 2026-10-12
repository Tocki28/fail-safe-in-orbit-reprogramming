/*
 * core_clock.h — the firmware's time base: the core clock at 170 MHz, the cycle counter that
 * bounds every wait, and the millisecond tick.
 */
#ifndef CORE_CLOCK_H
#define CORE_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* The chip's highest core clock [DS12589 Rev 6, p. 1]. */
#define CORE_CLOCK_HZ    170000000U
#define CYCLES_PER_MS    (CORE_CLOCK_HZ / 1000U)

/* Start the core's cycle counter, then HSI16 -> PLL -> 170 MHz (Range 1 boost mode, 4 flash
 * wait states). False if a step did not finish in time: the clock is then not usable. */
bool core_clock_start(void);

/* Core cycles since core_clock_start(); wraps every 25 s at 170 MHz. */
uint32_t cycles_now(void);

/* SysTick interrupt once per millisecond. Needs the core at CORE_CLOCK_HZ. */
void tick_start(void);

/* Milliseconds since tick_start(); wraps after 49.7 days. */
uint32_t tick_ms(void);

#endif /* CORE_CLOCK_H */
