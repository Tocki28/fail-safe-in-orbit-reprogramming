/*
 * clock.h — the bootloader's core clock (170 MHz while it checks images) and the cycle counter
 * used to time the signature checks and to bound every wait.
 */
#ifndef CLOCK_H
#define CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* The chip's highest core clock [DS12589 Rev 6, p. 1]: the signature checks run fastest here. */
#define CORE_CLOCK_HZ    170000000U
#define CYCLES_PER_US    (CORE_CLOCK_HZ / 1000000U)
/* The clock after reset, and the one kept when the PLL fails: HSI16
 * [RM0440 Rev 9, Sec. 5.3.3, p. 190]. */
#define HSI16_HZ         16000000U
#define HZ_PER_MHZ       1000000U

/* Start the core's cycle counter (DWT CYCCNT). Call first: every wait here is bounded by it. */
void cycles_start(void);
/* Value of the cycle counter. */
uint32_t cycles_now(void);

/* HSI16 -> PLL -> 170 MHz, Range 1 boost mode, 4 flash wait states. False if a step did not
 * finish in time; the core then stays on HSI16 at 16 MHz. */
bool clock_to_170mhz(void);

/* The core clock in use: CORE_CLOCK_HZ, or HSI16_HZ after a clock fault. */
uint32_t clock_hz(void);

/* Back to the state after reset (HSI16, PLL off, 1 wait state, Range 1 normal mode), so the
 * next program starts from the clock it would have after a reset. */
void clock_back_to_reset(void);

#endif /* CLOCK_H */
