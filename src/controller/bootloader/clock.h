/*
 * clock.h — the bootloader's core clock (170 MHz while it checks images) and the cycle counter
 * used to time the signature checks.
 */
#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

/* The chip's highest core clock [DS12589 Rev 6, p. 1]: the signature checks run fastest here. */
#define CORE_CLOCK_HZ    170000000U
#define CYCLES_PER_US    (CORE_CLOCK_HZ / 1000000U)

/* Start the core's cycle counter (DWT CYCCNT). Call first: clock_to_170mhz() waits on it. */
void cycles_start(void);
uint32_t cycles_now(void);

/* HSI16 -> PLL -> 170 MHz, Range 1 boost mode, 4 flash wait states. */
void clock_to_170mhz(void);

/* Back to the state after reset (HSI16, PLL off, 1 wait state, Range 1 normal mode), so the
 * next program starts from the clock it would have after a reset. */
void clock_back_to_reset(void);

#endif /* CLOCK_H */
