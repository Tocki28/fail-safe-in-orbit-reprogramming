/*
 * watchdog.h — the bootloader's use of the independent watchdog (IWDG).
 *
 * The bootloader starts it as the first action of main() with the longest time-out, about 32 s,
 * and reloads it only between steps, never inside a wait. A wait that never ends (a flash BSY
 * that does not clear, a fault handler's endless loop) then ends in a reset. Before it starts
 * a program, the hand-over sets the firmware's own time-out of about 2 s on the same running
 * watchdog (handover.c). Once running, the IWDG cannot be stopped [RM0440 Rev 9, Sec. 35.3.1,
 * p. 1557].
 */
#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

/* Start the IWDG with the long time-out (prescaler /256, reload 0xFFF, about 32 s). Works also
 * when the hardware-watchdog option bit has started it at power-on. Call first in main(). */
void watchdog_start_long(void);

/* Reload the counter: another full time-out from now. Call between steps, never in a wait. */
void watchdog_reload(void);

/* True if the watchdog is confirmed running: IWDG_SR is 0 (updates settled) and LSIRDY is 1
 * (decision F(b): no trial start otherwise). Waits for IWDG_SR = 0 up to 50 ms (the update takes
 * up to 43.4 ms with prescaler /256), then reads IWDG_SR and RCC_CSR once each and stores the raw
 * values it decided on in *iwdg_sr and *rcc_csr, and the time waited in microseconds in
 * *waited_us, for the log line. All pointers must be valid; a null pointer answers false. */
bool watchdog_running(uint32_t *iwdg_sr, uint32_t *rcc_csr, uint32_t *waited_us);

/* Set the firmware's time-out (about 2 s) on the running watchdog, then reload. Used by the
 * hand-over, once, just before the jump. */
void watchdog_set_firmware_timeout(void);

#endif /* WATCHDOG_H */
