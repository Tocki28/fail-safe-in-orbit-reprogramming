/*
 * backup_layout.h — what the bootloader and the main firmware keep in the TAMP backup registers:
 * a few words that survive a system reset (watchdog, software, reset pin) without a flash write.
 *
 * Definitions only, like layout.h, so both programs read one map; each program has its own
 * access code (bootloader/backup.c, firmware/mode-manager/mode_port.cpp): REQ-119, no shared
 * code between bootloader and firmware.
 *
 * ---- Why the backup registers (decision B, owner, 13 Oct 2026) -------------------------------
 * What survives: the backup registers are not affected by a system reset
 * [RM0440 Rev 9, Sec. 38.6.8, p. 1623]. A system reset is any of: NRST pin, watchdog, software
 * reset, option-byte loader, brown-out [RM0440 Rev 9, Sec. 7.1.2, p. 270]. A power reset leaves
 * the RTC domain, which holds them, as it was [RM0440 Rev 9, Sec. 7.1.1, p. 270].
 * What clears them (to 0x0000 0000):
 *   - an RTC domain reset: BDRST in RCC_BDCR, or VDD or VBAT powering on after both were off
 *     [RM0440 Rev 9, Sec. 7.1.3, p. 272]; so a power cut clears them only if VBAT goes too;
 *   - a tamper detection, in the default configuration [RM0440 Rev 9, Sec. 38.3.4, p. 1613]
 *     (no tamper input is enabled by any controller program);
 *   - a change of readout protection from level 1 to level 0 [RM0440 Rev 9, Sec. 38.3.4,
 *     p. 1613].
 * Writes need the DBP bit in PWR_CR1 [RM0440 Rev 9, Sec. 38.3.3, p. 1612; Sec. 6.4.1, p. 253],
 * set with the PWR clock on [RM0440 Rev 9, Sec. 6.1.3, p. 231], and the register interface runs
 * on the RTC APB clock, RTCAPBEN [RM0440 Rev 9, Table 347, p. 1612; Sec. 7.4.17, p. 310].
 * Rejected: a .noinit area of SRAM at a fixed address in the linker script. With the option bit
 * SRAM_PE set, reading a location never written since power-on raises an NMI
 * [RM0440 Rev 9, Sec. 2.4.1, p. 89]; common/flash.c's NMI handler stops for every cause but a
 * flash ECC error, the watchdog restarts the chip, and the location is still unwritten: a restart
 * loop. The CCM SRAM at the top of RAM is also erased at every system reset when the option bit
 * CCMSRAM_RST selects it [RM0440 Rev 9, Sec. 2.4.4, p. 90]. The backup registers need neither a
 * linker change nor a first write.
 * Open (TBD-vbat): whether VBAT stays powered when VDD is cut, on the bench board and on the
 * flight board, is not known here; if it does, these words also survive a power cut.
 *
 * ---- Where ------------------------------------------------------------------------------------
 * The STM32G431 is a category 2 device [RM0440 Rev 9, Table 1, p. 75] with 16 backup registers
 * [RM0440 Rev 9, Sec. 38.1, p. 1610; DS12589 Rev 6, Sec. 3.26, p. 37]. TAMP_BKPxR sits at offset
 * 0x100 + 4 x from the TAMP base [RM0440 Rev 9, Sec. 38.6.8, p. 1623], and the TAMP base is
 * 0x4000 2400 [RM0440 Rev 9, Table 3, p. 87]: TAMP_BKP0R is at 0x4000 2500. Pair n uses
 * TAMP_BKP(2n)R for the value and TAMP_BKP(2n+1)R for its bitwise complement, so the five pairs
 * below take TAMP_BKP0R to TAMP_BKP9R, 0x4000 2500 to 0x4000 2527.
 *
 * ---- One bit error cannot fake a value ---------------------------------------------------------
 * A pair counts only when the second word is the complement of the first. One flipped bit breaks
 * the pair, and a broken pair reads as empty. After an RTC domain reset both words are 0, a
 * broken pair, so a fresh chip reads as empty too. A broken pair is stored data that failed its
 * check, like a damaged boot-state copy: it is read as empty and overwritten, never a fault
 * (common/fault.h). The value 0 in a matching pair also means "nothing" for every pair.
 *
 * ---- The pairs -------------------------------------------------------------------------------
 * pair  name        written by                     read and cleared by
 *    0  RESTARTS    bootloader (counts)            bootloader (skip decision); main firmware
 *                                                  clears it after kNormalRunMs of running
 *    1  LAST_START  bootloader, just before each   bootloader, at the next start
 *                   hand-over
 *    2  GIVE_UP     main firmware, when the give-  bootloader, before it counts the start
 *                   up's boot-state write fails    (decision G)
 *    3  FAULT       main firmware, before a        bootloader, at the next start
 *                   counted software restart
 *    4  FALLBACK    bootloader, when it refused a  main firmware: EVENT 0x0002 once, then
 *                   copy and started a later one   cleared (Interfaces Sec. 5.4)
 */
#ifndef BACKUP_LAYOUT_H
#define BACKUP_LAYOUT_H

/* Backup registers on the STM32G431 (category 2), see above. */
#define BACKUP_REGISTERS              16U

#define BACKUP_PAIR_RESTARTS          0U
#define BACKUP_PAIR_LAST_START        1U
#define BACKUP_PAIR_GIVE_UP           2U
#define BACKUP_PAIR_FAULT             3U
#define BACKUP_PAIR_FALLBACK          4U
#define BACKUP_PAIRS                  5U

#define BACKUP_BYTE_MASK              0xFFU

/* RESTARTS: consecutive restarts of one slot started as the confirmed slot (decision B).
 * Bits 7:0 the count, bits 15:8 the slot it belongs to (BOOT_SLOT_A or BOOT_SLOT_B,
 * boot_state.h). The count stops at 255. */
#define BACKUP_RESTARTS_SLOT_SHIFT    8U

/* LAST_START: what the bootloader started last. Bits 7:0 the slot code (boot_state.h;
 * BOOT_SLOT_NONE for the last-resort firmware), bits 15:8 its role. */
#define BACKUP_START_ROLE_SHIFT       8U
#define BACKUP_ROLE_CONFIRMED         0x01U
#define BACKUP_ROLE_TRIAL             0x02U
#define BACKUP_ROLE_LAST_RESORT       0x03U

/* GIVE_UP: the slot code of the trial the firmware gave up (decision G). */

/* FAULT: why the firmware restarted itself; the bootloader counts such a software restart like
 * a watchdog restart (decision B, owner's fault decision of 13 Oct 2026). */
#define BACKUP_FAULT_IMPOSSIBLE_VALUE 0x01U   /* fault_impossible_value: EVENT 0x0005 sent */
#define BACKUP_FAULT_SELF_TEST        0x02U   /* self-test failed in OPERATE (decision L) */

/* FALLBACK: bits 7:0 the check result of the refused copy (Interfaces Sec. 5.4, "Check result"
 * table, 0x00 to 0x0A), bits 15:8 the refused copy and bits 23:16 the copy started, both in the
 * codes of the HEALTH report's running slot (Interfaces Sec. 5.4): */
#define BACKUP_FALLBACK_REFUSED_SHIFT 8U
#define BACKUP_FALLBACK_STARTED_SHIFT 16U
#define BACKUP_COPY_A                 0x01U
#define BACKUP_COPY_B                 0x02U
#define BACKUP_COPY_LAST_RESORT       0x03U

#endif /* BACKUP_LAYOUT_H */
