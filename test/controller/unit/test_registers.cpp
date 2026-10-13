/*
 * test_registers.cpp — host unit tests of the firmware's register-level files, with the chip
 * replaced by RAM registers and a small model of the PLL and the clock switch
 * (unit/board_stubs/stm32g4xx.h):
 *   firmware/core_clock.cpp      decision C: the clock in use, and the SysTick reload from it;
 *   data-link/uart_link.cpp      decision C: the baud-rate divider from the clock in use;
 *   mode-manager/mode_port.cpp   the firmware's TAMP backup-register access (decisions B, G, L,
 *                                and the fall-back record).
 * Each test's comment says whether it failed on the code before 13 Oct 2026.
 *
 * Built with the Mac's clang++, the undefined-behaviour sanitizer and libc++'s debug hardening.
 * Run: make -C test/controller unit. Prints PASS or FAIL per test; exit status 1 on a failure.
 */
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "backup_layout.h"
#include "boot_state.h"
#include "core_clock.hpp"
#include "fake_fault.hpp"
#include "flash.h"
#include "mode_port.hpp"
#include "stm32g4xx.h"
#include "uart_link.hpp"

FakeChip fake_chip;
DWT_Type fake_dwt;
CoreDebug_Type fake_coredebug;
RCC_TypeDef fake_rcc;
PWR_TypeDef fake_pwr;
FLASH_TypeDef fake_flash_regs;
SysTick_Type fake_systick;
GPIO_TypeDef fake_gpioa;
USART_TypeDef fake_usart2;
TAMP_TypeDef fake_tamp;

/**
 * @brief One step of the modelled chip (board_stubs/stm32g4xx.h): PLLRDY follows PLLON when the
 * PLL locks; SWS follows SW when the switch works.
 */
void fakeChipStep()
{
    if ((fake_rcc.CR & RCC_CR_PLLON) == 0U) {
        fake_rcc.CR &= ~RCC_CR_PLLRDY;
    } else if (fake_chip.pll_locks) {
        fake_rcc.CR |= RCC_CR_PLLRDY;
    }
    if (fake_chip.sws_follows) {
        fake_rcc.CFGR =
            (fake_rcc.CFGR & ~RCC_CFGR_SWS_Msk) | ((fake_rcc.CFGR & RCC_CFGR_SW_Msk) << 2U);
    }
}

/**
 * @brief Stand-in for the NVIC enable: records the interrupt number.
 *
 * @param[in] irq  interrupt number
 */
void NVIC_EnableIRQ(IRQn_Type irq)
{
    fake_chip.nvic_enabled_irq = static_cast<uint32_t>(irq);
}

/**
 * @brief Stand-in for the system reset request: counts it and returns.
 */
void NVIC_SystemReset()
{
    fake_chip.system_resets++;
}

/**
 * @brief Stand-in for boot_state_read (not under test here).
 *
 * @param[out] state  not written
 * @return false
 */
extern "C" bool boot_state_read(boot_state_t *state)
{
    (void)state;
    return false;
}

/**
 * @brief Stand-in for boot_state_write (not under test here).
 *
 * @param[in,out] state  not written
 * @return false
 */
extern "C" bool boot_state_write(boot_state_t *state)
{
    (void)state;
    return false;
}

/**
 * @brief Stand-in for flash_ecc_clear (not under test here).
 */
extern "C" void flash_ecc_clear(void)
{
}

/**
 * @brief Stand-in for flash_ecc_error_seen (not under test here).
 *
 * @return false
 */
extern "C" bool flash_ecc_error_seen(void)
{
    return false;
}

namespace {

uint32_t failures;
uint32_t tests;

/**
 * @brief Counts and prints one test result.
 *
 * @param[in]     ok    true if the test passed
 * @param[in]     name  what was tested
 */
void report(bool ok, const char *name)
{
    tests++;
    if (!ok) {
        failures++;
    }
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
}

/**
 * @brief Puts the registers as they are after a reset: HSI16 selected and in use, PLL off; the
 * cycle counter counts, the PLL locks, the switch works; backup registers 0.
 */
void resetChip()
{
    fake_chip = FakeChip{true, true, true, 0U, 0U};
    fake_dwt = DWT_Type{};
    fake_coredebug = CoreDebug_Type{};
    fake_rcc = RCC_TypeDef{};
    fake_rcc.CFGR = RCC_CFGR_SW_HSI | RCC_CFGR_SWS_HSI;
    fake_pwr = PWR_TypeDef{};
    fake_flash_regs = FLASH_TypeDef{};
    fake_systick = SysTick_Type{};
    fake_gpioa = GPIO_TypeDef{};
    fake_usart2 = USART_TypeDef{};
    fake_usart2.ISR = USART_ISR_TXE_TXFNF | USART_ISR_TC;   /* the transmitter is always ready */
    fake_tamp = TAMP_TypeDef{};
    faultClear();
}

/**
 * @brief Decision C: CoreClock::start() reports the clock in use; a PLL that does not start
 * leaves the core on HSI16 at 16 MHz instead of a failure.
 *
 * Fails on the old code: start() returned false for every failed step and left the clock at the
 * failed step (AHB / 2, PLL on), and main() stopped.
 */
void testClock()
{
    resetChip();
    report((CoreClock::start() == kCoreClockHz) &&
               ((fake_rcc.CFGR & RCC_CFGR_SW_Msk) == RCC_CFGR_SW_PLL) &&
               ((fake_rcc.CFGR & RCC_CFGR_HPRE_Msk) == 0U),
           "C: the PLL starts -> 170 MHz, PLL selected, AHB / 1");

    resetChip();
    fake_chip.pll_locks = false;
    report((CoreClock::start() == kHsi16Hz) &&
               ((fake_rcc.CFGR & RCC_CFGR_SW_Msk) == RCC_CFGR_SW_HSI) &&
               ((fake_rcc.CFGR & RCC_CFGR_SWS_Msk) == RCC_CFGR_SWS_HSI) &&
               ((fake_rcc.CFGR & RCC_CFGR_HPRE_Msk) == 0U) && ((fake_rcc.CR & RCC_CR_PLLON) == 0U),
           "C: the PLL does not lock -> 16 MHz on HSI16, AHB / 1, PLL off (was: false, AHB / 2, "
           "firmware stopped)");

    resetChip();
    fake_chip.sws_follows = false;   /* SWS stays at HSI16: the switch to the PLL never shows */
    report((CoreClock::start() == kHsi16Hz) &&
               ((fake_rcc.CFGR & RCC_CFGR_SW_Msk) == RCC_CFGR_SW_HSI) &&
               ((fake_rcc.CR & RCC_CR_PLLON) == 0U),
           "C: the switch to the PLL does not finish -> back on HSI16, 16 MHz");

    resetChip();
    fake_chip.sws_follows = false;
    fake_chip.pll_locks = false;
    fake_rcc.CFGR = RCC_CFGR_SW_PLL | RCC_CFGR_SWS_PLL;   /* stuck showing the PLL */
    report(CoreClock::start() == 0U,
           "C: the switch back to HSI16 does not finish either -> 0, clock not known");

    resetChip();
    fake_chip.cycles_count = false;
    uint32_t cfgr = fake_rcc.CFGR;
    report((CoreClock::start() == 0U) && (fake_rcc.CFGR == cfgr) && (fake_rcc.CR == 0U),
           "cycle counter dead -> 0 (no usable clock), no clock register touched");
}

/**
 * @brief Decision C: the SysTick reload comes from the clock in use; any other clock value is
 * the fault reaction.
 *
 * Fails on the old code: startTick() took no clock and always loaded 169 999 (a 10.6 ms tick at
 * 16 MHz).
 */
void testTick()
{
    resetChip();
    CoreClock::startTick(kCoreClockHz);
    bool fast = (fake_systick.LOAD == 169999U) && (fake_systick.VAL == 0U) &&
                (fake_systick.CTRL == (SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk |
                                       SysTick_CTRL_ENABLE_Msk));
    CoreClock::startTick(kHsi16Hz);
    bool slow = (fake_systick.LOAD == 15999U);
    report(fast && slow, "C: SysTick reload 169 999 at 170 MHz, 15 999 at 16 MHz (1 ms each)");

    bool faulted = false;
    fake_systick.LOAD = 7U;
    FAULT_CATCH(faulted, CoreClock::startTick(1000000U));
    report(faulted && (fake_systick.LOAD == 7U) && (fault_calls == 1U),
           "startTick(1 MHz), a clock start() never returns -> fault reaction, SysTick untouched");
}

/**
 * @brief Decision C: the UART's divider comes from the clock in use; any other clock value is the
 * fault reaction.
 *
 * The expected dividers are computed here by hand: (16 000 000 + 57 600) / 115 200 = 139 and
 * (170 000 000 + 57 600) / 115 200 = 1476 (integer division, RM0440 Rev 9, Sec. 40.5.7). Fails on
 * the old code: init() took no clock and always wrote 1476 (13 824 baud at 16 MHz).
 */
void testUart()
{
    static UartLink link;

    report((UartLink::brrFor(kCoreClockHz) == 1476U) && (UartLink::brrFor(kHsi16Hz) == 139U),
           "C: USART2 divider 1476 at 170 MHz, 139 at 16 MHz");
    resetChip();
    link.init(kHsi16Hz);
    report((fake_usart2.BRR == 139U) &&
               (fake_usart2.CR1 == (USART_CR1_RXNEIE_RXFNEIE | USART_CR1_TE | USART_CR1_RE |
                                    USART_CR1_UE)) &&
               (fake_chip.nvic_enabled_irq == 38U),
           "C: init at 16 MHz -> BRR 139, transmitter, receiver and receive interrupt on");

    bool faulted = false;
    resetChip();
    fake_usart2.BRR = 7U;
    FAULT_CATCH(faulted, link.init(12345U));
    report(faulted && (fake_usart2.BRR == 7U) && (fault_calls == 1U),
           "init(12 345 Hz), a clock start() never returns -> fault reaction, USART2 untouched");
}

/**
 * @brief The firmware's backup-register access (mode_port.cpp): give-up request (G), fall-back
 * record, restart-count clear (B), counted restart (L and the fault reaction).
 *
 * Fails on the old code: none of these functions existed.
 */
void testBackup()
{
    resetChip();
    bool written = ModePort::leaveGiveUpRequest(BOOT_SLOT_B);
    report(written && (fake_tamp.BKP[4] == BOOT_SLOT_B) && (fake_tamp.BKP[5] == ~uint32_t{2U}) &&
               ((fake_pwr.CR1 & PWR_CR1_DBP) == 0U) &&
               ((fake_rcc.APB1ENR1 & RCC_APB1ENR1_RTCAPBEN) != 0U),
           "G: give-up request for B -> BKP4R = 2, BKP5R = its complement; DBP cleared again");

    FallbackRecord record{};
    fake_tamp.BKP[8] = 0x00010209U;
    fake_tamp.BKP[9] = ~0x00010209U;
    bool read = ModePort::readFallbackRecord(record);
    bool fields = (record.started == 0x01U) && (record.refused == 0x02U) && (record.check == 0x09U);
    fake_tamp.BKP[9] ^= 0x10U;   /* one flipped bit */
    bool broken = !ModePort::readFallbackRecord(record);
    bool cleared = ModePort::clearFallbackRecord() && (fake_tamp.BKP[8] == 0U) &&
                   (fake_tamp.BKP[9] == 0xFFFFFFFFU) && !ModePort::readFallbackRecord(record);
    report(read && fields && broken && cleared,
           "fall-back record: read as started 01, refused 02, check 09; one flipped bit -> none; "
           "cleared -> none");

    fake_tamp.BKP[0] = (BOOT_SLOT_A << BACKUP_RESTARTS_SLOT_SHIFT) | 2U;
    fake_tamp.BKP[1] = ~fake_tamp.BKP[0];
    bool other_kept = ModePort::clearRestartCount(BOOT_SLOT_B) &&
                      (fake_tamp.BKP[0] == ((BOOT_SLOT_A << BACKUP_RESTARTS_SLOT_SHIFT) | 2U));
    bool own_cleared = ModePort::clearRestartCount(BOOT_SLOT_A) && (fake_tamp.BKP[0] == 0U) &&
                       (fake_tamp.BKP[1] == 0xFFFFFFFFU);
    report(other_kept && own_cleared,
           "B: restart count of A left by firmware of B, cleared by firmware of A");

    ModePort::restartCounted(BACKUP_FAULT_SELF_TEST);
    report((fake_tamp.BKP[6] == BACKUP_FAULT_SELF_TEST) &&
               (fake_tamp.BKP[7] == ~uint32_t{BACKUP_FAULT_SELF_TEST}) &&
               (fake_chip.system_resets == 1U),
           "L / fault reaction: restartCounted -> fault mark in BKP6R/BKP7R, then the reset "
           "request");
}

}  // namespace

/**
 * @brief Runs every register-level test.
 *
 * @return 0 if all passed, 1 otherwise
 */
int main()
{
    testClock();
    testTick();
    testUart();
    testBackup();
    if (failures == 0U) {
        std::printf("register unit tests: all %u passed\n", tests);
        return 0;
    }
    std::printf("register unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
