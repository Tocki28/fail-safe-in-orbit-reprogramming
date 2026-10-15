/*
 * main.c — TEST application for the bootloader bench tests. Not the real firmware.
 *
 * One source, three link addresses (Makefile: SLOT=A, B or LR). Every second it prints
 * "APP <slot> v<version> running" on USART2 and reloads the watchdog the bootloader started.
 * After 3 s, by build flag:
 *   confirm  if it is the trial slot, it confirms itself in the boot state
 *            (confirmed = this slot, trial = none, failed starts = 0);
 *   hang     it stops reloading the watchdog and loops, so the watchdog restarts the chip
 *            and the bootloader counts a failed start.
 *   trial-forever  (a different programme, not a decision at 3 s) it starts on trial and says so
 *            in framed HEALTH reports for the onboard computer's trial watch (decision H, trial
 *            time 120 s + margin 30 s): see trial_forever. It prints no text, reloads the
 *            watchdog every second, and never confirms and never gives up, so the onboard
 *            computer resets the controller and the bootloader counts a failed start.
 * It runs on HSI16, the clock the bootloader hands over (bootloader/handover.h). It shares no
 * code with the bootloader except the boot-state module it must use to confirm, and so it also
 * defines the fault reaction that module calls for an impossible value (common/fault.h).
 *
 * Build flags (Makefile):
 *   APP_SLOT     BOOT_SLOT_A, BOOT_SLOT_B, or BOOT_SLOT_NONE for the last-resort build
 *   APP_NAME     "A", "B" or "LR"
 *   APP_VERSION  the version written into the image header by tools/make_image.py
 *   APP_HANG     1 = hang after 3 s, 0 = confirm after 3 s
 *   APP_TRIAL_FOREVER  1 = the trial-forever programme (HEALTH reports, never ends), 0 = not
 *   APP_REPORT_SLOT    the running-slot code of the HEALTH report: 0x01 A, 0x02 B, 0x03 last-resort
 *   APP_REPORT_KIND    the firmware kind of the HEALTH report: IMAGE_KIND_MAIN or
 *                      IMAGE_KIND_LAST_RESORT (common/image_format.h)
 *   CODE_START   the layout.h address this build is linked at
 */
#include <stdint.h>

#include "boot_state.h"
#include "crc32.h"
#include "fault.h"
#include "image_format.h"
#include "layout.h"
#include "stm32g4xx.h"

/* HSI16: the core clock after reset [RM0440 Rev 9, Sec. 5.3.3, p. 190]. */
#define CORE_CLOCK_HZ      16000000U
#define BAUD_RATE          115200U
/* BRR = kernel clock / baud [RM0440 Rev 9, Sec. 40.5.7, p. 1707-1708]: 139, 0.08 % slow. */
#define BRR_VALUE          ((CORE_CLOCK_HZ + (BAUD_RATE / 2U)) / BAUD_RATE)
/* PA2 = USART2_TX on alternate function 7 [DS12589 Rev 6, Table 13, p. 61]. */
#define PA2_AF_USART2      7U
#define GPIO_MODE_AF       2U
/* Reload key [RM0440 Rev 9, Sec. 35.3.2, p. 1557]. */
#define IWDG_KEY_RELOAD    0xAAAAU
/* SysTick counts processor clocks down from RELOAD and sets COUNTFLAG at 0
 * [Arm DUI 0553A, Sec. 4.4.1, p. 4-33 to 4-34]: one flag per millisecond. */
#define SYSTICK_RELOAD     ((CORE_CLOCK_HZ / 1000U) - 1U)
#define MS_PER_SECOND      1000U
/* The bench plan: confirm or hang 3 s after start. */
#define DECISION_SECOND    3U
#define DECIMAL_DIGITS     10U

/* The trial-forever programme sends the controller's HEALTH report as the operating firmware
 * does (src/controller/firmware/health-monitor/health_monitor.cpp, data-link/frame_sender.cpp,
 * data-link/frame.cpp). Frame fields [Interfaces Sec. 4.0, frame table]: */
#define FRAME_SYNC_0       0xA5U
#define FRAME_SYNC_1       0x5AU
#define FRAME_VERSION      0x01U
#define FRAME_TYPE_REPORT  0x03U
/* Destination 0x01 = onboard computer [Interfaces Sec. 4.0, frame table]; every frame the
 * operating firmware sends goes there (frame_sender.cpp, FrameSender::send). */
#define FRAME_DEST_ONBOARD 0x01U
/* Frame offsets [Interfaces Sec. 4.0, Figure 2]; the flags (offset 5) and the expiry (offset 12)
 * are 0 in a report and are left as the zero fill. */
#define FRAME_AT_VERSION   2U
#define FRAME_AT_TYPE      3U
#define FRAME_AT_DEST      4U
#define FRAME_AT_LENGTH    6U
#define FRAME_AT_SEQUENCE  8U
#define FRAME_HEADER_BYTES 16U
#define FRAME_CHECK_BYTES  4U
/* HEALTH payload: 24 bytes; offsets of the fields [Interfaces Sec. 5.4, report table;
 * health_monitor.hpp, layout comment]. The report number (0) and the onboard computer clock
 * time (4) are sent as 0: the onboard computer fills them (health_monitor.cpp, report). */
#define HEALTH_BYTES       24U
#define HEALTH_AT_CLOCK    8U
#define HEALTH_AT_KIND     12U
#define HEALTH_AT_MODE     13U
#define HEALTH_AT_FW_KIND  14U
#define HEALTH_AT_VERSION  15U
#define HEALTH_AT_SLOT     19U
#define HEALTH_AT_FAILED   20U
#define HEALTH_AT_TRIAL    21U
#define HEALTH_AT_SELFTEST 23U
#define HEALTH_KIND_HEALTH 0x01U   /* report kind HEALTH [Interfaces Sec. 5.4, report table] */
#define HEALTH_MODE_TRIAL  0x02U   /* mode TRIAL [Interfaces Sec. 5.4, report table] */
#define HEALTH_SELF_PASSED 0x00U   /* last self-test PASSED [Interfaces Sec. 5.4, report table] */
/* The whole frame: header, payload, check value [Interfaces Sec. 4.0, Figure 2]. */
#define HEALTH_FRAME_BYTES (FRAME_HEADER_BYTES + HEALTH_BYTES + FRAME_CHECK_BYTES)

/**
 * @brief Starts USART2 transmit on PA2 at 115200 baud from HSI16.
 *
 * The same register steps as the bootloader's uart_init (bootloader/uart.c), with the divider
 * for 16 MHz: this test application never changes the clock.
 */
static void uart_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    RCC->APB1ENR1 |= RCC_APB1ENR1_USART2EN;
    (void)RCC->APB1ENR1;
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~GPIO_AFRL_AFSEL2_Msk) | (PA2_AF_USART2 << GPIO_AFRL_AFSEL2_Pos);
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODE2_Msk) | (GPIO_MODE_AF << GPIO_MODER_MODE2_Pos);
    USART2->BRR = BRR_VALUE;
    USART2->CR1 = USART_CR1_TE | USART_CR1_UE;
}

/**
 * @brief Sends one character on USART2, waiting for TXE.
 *
 * TXE: TDR has moved to the shift register [RM0440 Rev 9, Sec. 40.8.10, p. 1767]. The wait has no
 * limit of its own: a test application on the bench; the watchdog the bootloader started ends a
 * transmitter that never empties.
 *
 * @param[in]     c  character to send
 */
static void put_char(char c)
{
    while ((USART2->ISR & USART_ISR_TXE_TXFNF) == 0U) {
    }
    USART2->TDR = (uint8_t)c;
}

/**
 * @brief Sends a zero-terminated string on USART2.
 *
 * @param[in]     text  string to send
 */
static void put_text(const char *text)
{
    while (*text != '\0') {
        put_char(*text);
        text++;
    }
}

/**
 * @brief Sends a number on USART2 as decimal digits without leading zeros.
 *
 * @param[in]     value  number to send
 */
static void put_decimal(uint32_t value)
{
    char digits[DECIMAL_DIGITS];
    uint32_t count = 0U;

    do {
        digits[count] = (char)('0' + (value % 10U));
        count++;
        value /= 10U;
    } while (value != 0U);
    while (count > 0U) {
        count--;
        put_char(digits[count]);
    }
}

/**
 * @brief Sends one line: "APP <slot> v<version> <what>".
 *
 * @param[in]     what  text after the version
 */
static void say(const char *what)
{
    put_text("APP " APP_NAME " v");
    put_decimal(APP_VERSION);
    put_text(" ");
    put_text(what);
    put_text("\r\n");
}

/**
 * @brief Waits 1 s: 1000 SysTick COUNTFLAGs of 1 ms each.
 *
 * COUNTFLAG is set each time the count reaches 0 and cleared by the read [Arm DUI 0553A,
 * Sec. 4.4.1, p. 4-33 to 4-34]. The waits have no limit of their own: a stopped SysTick is ended
 * by the watchdog the bootloader started.
 */
static void wait_one_second(void)
{
    for (uint32_t ms = 0U; ms < MS_PER_SECOND; ms++) {
        while ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) == 0U) {
        }
    }
}

/**
 * @brief Confirms this slot in the boot state if it is the trial slot.
 *
 * Only the trial slot confirms itself; a slot running as confirmed or as fallback must not
 * touch the record, or it could confirm a trial that never ran.
 */
static void confirm_if_trial(void)
{
    boot_state_t state;

    if (APP_SLOT == BOOT_SLOT_NONE) {
        return;   /* the last-resort firmware is never a trial */
    }
    if (!boot_state_read(&state)) {
        say("no valid boot state; nothing to confirm");
        return;
    }
    if (state.trial != APP_SLOT) {
        say("not on trial; nothing to confirm");
        return;
    }
    state.confirmed = APP_SLOT;
    state.trial = BOOT_SLOT_NONE;
    state.failed_starts = 0U;
    say(boot_state_write(&state) ? "confirmed itself" : "confirm FAILED: boot state write");
}

/**
 * @brief The fault reaction for common/boot_state.c (common/fault.h): one line, then a software
 * reset.
 *
 * The test application needs only what the bench sees: the line says where, and the reset
 * (CMSIS NVIC_SystemReset, SYSRESETREQ [Arm DUI 0553A, Sec. 4.3.5, Table 4-17, p. 4-17]) hands
 * back to the bootloader, which reads everything again. The line also carries the fault-location
 * code (decision 3, common/fault_codes.h), in decimal.
 *
 * @param[in]     location  the fault-location code (common/fault_codes.h)
 * @param[in]     where     the function that found the impossible value
 * @note Does not return.
 */
void fault_impossible_value(uint8_t location, const char *where)
{
    say("corrupted value; restarting");
    put_text(where);
    put_text(", fault location ");
    put_decimal(location);
    put_text("\r\n");
    NVIC_SystemReset();
}

/**
 * @brief Writes a 16-bit value little-endian.
 *
 * Little-endian: Interfaces Sec. 4.0.
 *
 * @param[out]    bytes  two bytes, least significant first
 * @param[in]     value  the value to write
 */
static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

/**
 * @brief Writes a 32-bit value little-endian.
 *
 * Little-endian: Interfaces Sec. 4.0.
 *
 * @param[out]    bytes  four bytes, least significant first
 * @param[in]     value  the value to write
 */
static void write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

/**
 * @brief Builds one framed HEALTH report: mode TRIAL, this build's slot, kind and version.
 *
 * The same bytes the operating firmware sends in a HEALTH report in trial (health_monitor.cpp,
 * HealthMonitor::report, and frame.cpp, buildFrame): Type 0x03 report, Destination 0x01, flags 0,
 * length 24, expiry 0, the check value a CRC-32 of every byte before it [Interfaces Sec. 4.0].
 * The trial time left is 0: this test application keeps no trial time. The self-test is
 * reported as passed: it runs none.
 *
 * @param[out]    out           the frame, HEALTH_FRAME_BYTES bytes
 * @param[in]     sequence      the frame's sequence number, 1 to 2^32 - 1 [Interfaces Sec. 4.0]
 * @param[in]     clock_s       the controller clock time, seconds since start
 * @param[in]     failed_starts the failed starts of the new copy, from the boot state
 */
static void build_health_frame(uint8_t *out, uint32_t sequence, uint32_t clock_s,
                               uint8_t failed_starts)
{
    uint8_t *payload = &out[FRAME_HEADER_BYTES];

    for (uint32_t i = 0U; i < HEALTH_FRAME_BYTES; i++) {
        out[i] = 0U;
    }
    out[0] = FRAME_SYNC_0;
    out[1] = FRAME_SYNC_1;
    out[FRAME_AT_VERSION] = FRAME_VERSION;
    out[FRAME_AT_TYPE] = FRAME_TYPE_REPORT;
    out[FRAME_AT_DEST] = FRAME_DEST_ONBOARD;
    write_u16(&out[FRAME_AT_LENGTH], (uint16_t)HEALTH_BYTES);
    write_u32(&out[FRAME_AT_SEQUENCE], sequence);

    write_u32(&payload[HEALTH_AT_CLOCK], clock_s);
    payload[HEALTH_AT_KIND] = HEALTH_KIND_HEALTH;
    payload[HEALTH_AT_MODE] = HEALTH_MODE_TRIAL;
    payload[HEALTH_AT_FW_KIND] = (uint8_t)APP_REPORT_KIND;
    write_u32(&payload[HEALTH_AT_VERSION], APP_VERSION);
    payload[HEALTH_AT_SLOT] = (uint8_t)APP_REPORT_SLOT;
    payload[HEALTH_AT_FAILED] = failed_starts;
    write_u16(&payload[HEALTH_AT_TRIAL], 0U);
    payload[HEALTH_AT_SELFTEST] = HEALTH_SELF_PASSED;

    write_u32(&out[FRAME_HEADER_BYTES + HEALTH_BYTES],
              crc32_compute(out, FRAME_HEADER_BYTES + HEALTH_BYTES));
}

/**
 * @brief The trial-forever programme: a HEALTH report in TRIAL every second, for ever.
 *
 * Exercises the onboard computer's trial watch (decision H): the watch starts at the first
 * HEALTH report showing TRIAL (trial_watch.hpp) and, when no "confirmed" or "given up" event
 * comes within 120 s + 30 s, the onboard computer resets the controller. This programme never
 * writes the boot state (never confirms, never gives up), so the bootloader counts each such
 * start as a failed start and, after BOOT_STATE_MAX_STARTS (3), returns to the previous firmware.
 * It prints no text: the onboard computer reads USART2 as data link A and would only drop text.
 * The failed starts are read once, from the boot state; if it cannot be read, 0 is reported, as
 * the operating firmware does (health_monitor.cpp). The sequence number starts at 1 and rises by
 * one per frame [Interfaces Sec. 4.0]; it would wrap to 0 after 136 years, which a bench run
 * does not reach. The watchdog is reloaded on every pass (code rule B6); the pass lasts 1 s, well
 * inside the 2 s watchdog (code rule B6).
 *
 * @note Does not return.
 */
static void trial_forever(void)
{
    uint8_t frame[HEALTH_FRAME_BYTES];
    boot_state_t state;
    uint8_t failed_starts = 0U;

    if (boot_state_read(&state)) {
        failed_starts = state.failed_starts;
    }
    for (uint32_t second = 0U;; second++) {   /* endless: the programme never ends; the watchdog
                                                 and the onboard computer's reset end it */
        IWDG->KR = IWDG_KEY_RELOAD;
        build_health_frame(frame, second + 1U, second, failed_starts);
        for (uint32_t i = 0U; i < HEALTH_FRAME_BYTES; i++) {
            put_char((char)frame[i]);
        }
        wait_one_second();
    }
}

/**
 * @brief Prints a line every second, reloads the watchdog, and after 3 s confirms itself or
 * hangs, by build flag.
 *
 * With APP_TRIAL_FOREVER it runs trial_forever instead and prints nothing.
 *
 * @note Does not return.
 */
int main(void)
{
    /* Own vector table, also when started by a debugger instead of the bootloader. */
    SCB->VTOR = (uint32_t)CODE_START;
    uart_init();
    SysTick->LOAD = SYSTICK_RELOAD;
    SysTick->VAL = 0U;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;   /* no interrupt */

    if (APP_TRIAL_FOREVER != 0) {
        trial_forever();   /* does not return */
    }

    for (uint32_t second = 1U;; second++) {   /* endless: the application's main loop */
        say("running");
        if (second == DECISION_SECOND) {
            if (APP_HANG != 0) {
                say("hanging: watchdog no longer reloaded");
                for (;;) {   /* endless: the planned hang; the watchdog restarts the chip */
                }
            }
            confirm_if_trial();
        }
        IWDG->KR = IWDG_KEY_RELOAD;
        wait_one_second();
    }
}
