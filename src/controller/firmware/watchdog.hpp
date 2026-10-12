/*
 * watchdog.h — reload the independent watchdog the bootloader started (bootloader/handover.c:
 * 1.88 s at the shortest). A reload restarts its count; the firmware cannot stop it.
 */
#ifndef WATCHDOG_H
#define WATCHDOG_H

#include "stm32g4xx.h"

/* Reload key [RM0440 Rev 9, Sec. 35.3.2, p. 1557]. */
#define IWDG_KEY_RELOAD  0xAAAAU

static inline void watchdog_reload(void)
{
    IWDG->KR = IWDG_KEY_RELOAD;
}

#endif /* WATCHDOG_H */
