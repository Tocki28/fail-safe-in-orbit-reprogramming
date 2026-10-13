/*
 * backup.c — the bootloader's reads and writes of the TAMP backup registers, register level
 * (see backup.h; the map and the reasons are in common/backup_layout.h).
 *
 * The main firmware has its own copy of these few lines (firmware/mode-manager/mode_port.cpp):
 * REQ-119 asks the bootloader to share no code with the firmware.
 */
#include "backup.h"

#include "backup_layout.h"
#include "fault.h"
#include "stm32g4xx.h"

/* TAMP_BKPxR sits at offset 0x100 + 0x04 * x from the TAMP base [RM0440 Rev 9, Sec. 38.6.8,
 * p. 1623]. */
#define BKP_OFFSET  0x100U
#define BKP_STRIDE  4U
/* A pair takes two registers: the value, then its complement (common/backup_layout.h). */
#define PAIR_WORDS  2U

/**
 * @brief Returns the address of backup register `index`.
 *
 * Computed from the TAMP base, as the reference manual gives it, so the same code runs against
 * the RAM stand-in of the host test (test/controller/unit/backup_stubs), whose layout follows the
 * same offsets.
 *
 * @param[in]     index  register number, 0 to BACKUP_REGISTERS - 1
 * @return address of TAMP_BKP<index>R
 */
static volatile uint32_t *backup_register(uint32_t index)
{
    return (volatile uint32_t *)((uintptr_t)TAMP + BKP_OFFSET + (index * BKP_STRIDE));
}

/**
 * @brief Returns the register number of a pair's value word; a pair number outside the map is a
 * fault.
 *
 * Every caller passes a BACKUP_PAIR_ constant (common/backup_layout.h), so a number at or past
 * BACKUP_PAIRS is a corrupted value or a defect, never data: the fault reaction (common/fault.h).
 * The check also keeps every access inside the 16 registers of this chip.
 *
 * @param[in]     pair  BACKUP_PAIR_RESTARTS to BACKUP_PAIR_FALLBACK
 * @return register number of the value word; the complement is the next register
 * @note Does not return for a pair number outside the map.
 */
static uint32_t value_index(uint32_t pair)
{
    if (pair < BACKUP_PAIRS) {
        return pair * PAIR_WORDS;
    }
    fault_impossible_value("backup pair number");
}

/**
 * @brief Turns on the PWR and RTC APB clocks and sets DBP, so the backup registers can be read
 * and written.
 *
 * The order is the reference manual's for backup-domain access: PWR clock, then DBP in PWR_CR1
 * [RM0440 Rev 9, Sec. 6.1.3, p. 231; Sec. 6.4.1, p. 253]. The TAMP registers are clocked by the
 * RTC APB clock [RM0440 Rev 9, Table 347, p. 1612], enabled by RTCAPBEN [RM0440 Rev 9,
 * Sec. 7.4.17, p. 310]. The RTC kernel clock (RTCEN in RCC_BDCR) is not turned on: the block
 * diagram puts the backup registers in the APB clock domain [RM0440 Rev 9, Figure 533, p. 1611]
 * (not yet confirmed on the board: TBD on the bench).
 */
void backup_open(void)
{
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN | RCC_APB1ENR1_RTCAPBEN;
    (void)RCC->APB1ENR1;   /* read back: both clocks run before the next access (as uart.c) */
    PWR->CR1 |= PWR_CR1_DBP;
    (void)PWR->CR1;        /* read back: DBP is set before the first write to a backup register */
}

/**
 * @brief Clears DBP again and turns the RTC APB clock off before the hand-over.
 *
 * Why: after a reset the backup domain is write-protected [RM0440 Rev 9, Sec. 6.1.3, p. 231], and
 * the firmware should start with it so, as from a reset; it lifts the protection itself only
 * around its own writes (firmware/mode-manager/mode_port.cpp). DBP is cleared while the PWR clock
 * is still on: PWR registers ignore writes without it (bootloader/clock.c), and
 * clock_back_to_reset turns that clock off later in the hand-over.
 */
void backup_close(void)
{
    PWR->CR1 &= ~PWR_CR1_DBP;
    RCC->APB1ENR1 &= ~RCC_APB1ENR1_RTCAPBEN;
}

/**
 * @brief Reads a pair; it counts only when the second word is the complement of the first.
 *
 * Why the complement: one flipped bit breaks the pair, so one bit error cannot fake a count or a
 * request (decision B). After an RTC domain reset both words are 0, a broken pair, so a fresh
 * chip reads as empty (common/backup_layout.h).
 *
 * @param[in]     pair   BACKUP_PAIR_RESTARTS to BACKUP_PAIR_FALLBACK
 * @param[out]    value  the stored value; 0 when the pair is broken
 * @return true if the pair matches, false if it is broken (empty)
 */
bool backup_read(uint32_t pair, uint32_t *value)
{
    uint32_t index = value_index(pair);
    uint32_t word = *backup_register(index);
    uint32_t inverse = *backup_register(index + 1U);

    if (word != ~inverse) {
        *value = 0U;
        return false;
    }
    *value = word;
    return true;
}

/**
 * @brief Writes a value and its complement into a pair, then reads both back.
 *
 * A write while DBP is still 0 is ignored by the chip [RM0440 Rev 9, Sec. 38.3.3, p. 1612]; the
 * read-back then fails, so the caller learns that nothing was stored. A reset between the two
 * writes leaves a broken pair, which reads as empty, never as a wrong value.
 *
 * @param[in]     pair   BACKUP_PAIR_RESTARTS to BACKUP_PAIR_FALLBACK
 * @param[in]     value  value to store
 * @return true if both words read back as written, false otherwise
 */
bool backup_write(uint32_t pair, uint32_t value)
{
    uint32_t index = value_index(pair);
    volatile uint32_t *word = backup_register(index);
    volatile uint32_t *inverse = backup_register(index + 1U);

    *word = value;
    *inverse = ~value;
    return (*word == value) && (*inverse == ~value);
}
