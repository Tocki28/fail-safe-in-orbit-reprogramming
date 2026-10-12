/*
 * watchdog.hpp — reload the independent watchdog the bootloader started (bootloader/handover.c:
 * 1.88 s at the shortest). A reload restarts its count; the firmware cannot stop it.
 *
 * C++: class Watchdog. reload() is C's watchdog_reload(); reloadWhenDue() is main.c's
 * reload_watchdog_when_due(), with the time of the last reload as a member instead of a
 * file-static in main.c.
 */
#ifndef WATCHDOG_HPP
#define WATCHDOG_HPP

#include <cstdint>

#include "stm32g4xx.h"

class Watchdog {
public:
    /* Reload now. Static: it only writes the chip's one IWDG register, so the update port can
     * reload during a long erase (update-receiver/update_port.cpp) without holding the object. */
    static void reload()
    {
        IWDG->KR = kReloadKey;
    }

    /* Reload if kReloadPeriodMs have passed since the last reload here. */
    void reloadWhenDue(uint32_t now_ms)
    {
        /* Unsigned subtraction stays right when the millisecond count wraps. */
        if ((now_ms - last_reload_ms_) >= kReloadPeriodMs) {
            reload();
            last_reload_ms_ = now_ms;
        }
    }

private:
    /* Reload key [RM0440 Rev 9, Sec. 35.3.2, p. 1557]. */
    static constexpr uint32_t kReloadKey = 0xAAAAU;
    /* The bootloader starts the watchdog with 1.88 s at the shortest (bootloader/handover.c).
     * Reloaded every 100 ms from the main loop. The longest pass without a reload is UPLOAD
     * END: the CRC-32 and two Ed25519 checks of the slot (about 40 ms at -O2 in the bootloader,
     * test/controller/results/2026-10-12; slower at -Os here; upload_probe.py prints END's
     * time). UPLOAD BEGIN's erase (up to 489 ms) reloads after every page itself. A loop stuck
     * for 1.88 s is restarted (REQ-022). The reload follows the millisecond tick, so a stopped
     * tick also ends in a restart. */
    static constexpr uint32_t kReloadPeriodMs = 100U;

    uint32_t last_reload_ms_ = 0U;
};

#endif /* WATCHDOG_HPP */
