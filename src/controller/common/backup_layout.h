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
 * TAMP_BKP(2n)R for the value and TAMP_BKP(2n+1)R for its bitwise complement, so the seven pairs
 * below take TAMP_BKP0R to TAMP_BKP13R, 0x4000 2500 to 0x4000 2537. Budget: 14 of the 16
 * registers; TAMP_BKP14R and TAMP_BKP15R are free. Before decision 3 (13 Oct 2026) five pairs
 * used 10; the later-faults count (owner, 13 Oct 2026, second round) added the seventh.
 * Each program checks the map against the 16 registers when it is compiled
 * (bootloader/backup.c, firmware/mode-manager/mode_port.cpp).
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
 *    4  FALLBACK    bootloader, every copy it      main firmware: one EVENT 0x0002 per
 *                   refused or skipped on this     refused copy, then cleared (Interfaces
 *                   start (decision 4)             Sec. 5.4)
 *    5  FAULT_      bootloader's fault reaction,   main firmware: EVENT 0x0005 with the code,
 *       LOCATION    before its reset, the FIRST    once, then cleared
 *                   fault only (decision 3)
 *    6  FAULT_      bootloader's fault reaction,   main firmware: EVENT 0x0005 with the count,
 *       LATER       before its reset: the faults   once, then cleared
 *                   after the first (decision 3)
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
#define BACKUP_PAIR_FAULT_LOCATION    5U
#define BACKUP_PAIR_FAULT_LATER       6U
#define BACKUP_PAIRS                  7U

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

/* FAULT_LOCATION: the fault-location code (common/fault_codes.h, 0x01 to 0x1F) of the FIRST
 * function that found an impossible value while the bootloader ran (decision 3, owner, 13 Oct
 * 2026). The bootloader's fault reaction writes it only when the pair holds no location, so a
 * stored one that no firmware has reported yet is never overwritten (the first fault is usually
 * the cause). It survives the bootloader's later starts until a main firmware reports it.
 * FAULT_LATER: how many more faults the bootloader's fault reaction found after the first, before
 * a firmware reported it. It counts up by one per fault and stops at BACKUP_FAULT_LATER_MAX. It
 * is its own pair so that counting never rewrites the location; it is set to 0 when the location
 * is first written, so a count left by an earlier cycle is never read as this one's. A firmware
 * reads it only when a location is stored. */
#define BACKUP_FAULT_LATER_MAX        0xFFU   /* one byte of EVENT 0x0005's detail */

/* FALLBACK (decision 4, owner, 13 Oct 2026): every copy refused or skipped on one start. One byte
 * per copy holds the check result it was refused or skipped with (the table below), 0x00 when it
 * was not; bits 31:24 hold the copy started, 0x00 when none was (the bootloader parked). Copies
 * are in the codes of the HEALTH report's running slot (Interfaces Sec. 5.4). Why one byte per
 * copy: a start tries each copy at most once (trial and confirmed are the two different slots,
 * common/boot_state.c, fields_valid; then the last-resort firmware), so up to three records fit
 * one pair, in the order of the copies, not of the start. Before, bits 7:0 held the check
 * result, bits 15:8 the one copy refused just before the start, bits 23:16 the copy started. */
#define BACKUP_FALLBACK_A_SHIFT            0U
#define BACKUP_FALLBACK_B_SHIFT            8U
#define BACKUP_FALLBACK_LAST_RESORT_SHIFT  16U
#define BACKUP_FALLBACK_STARTED_SHIFT      24U
#define BACKUP_COPY_A                 0x01U
#define BACKUP_COPY_B                 0x02U
#define BACKUP_COPY_LAST_RESORT       0x03U

/* Check-result codes of a refused copy: Interfaces Sec. 5.4, "Check result" table, the first
 * check the copy failed. One code per check of common/image_check.c, in the order it runs them.
 * Moved here from bootloader/main.c with decision 4, so the bootloader that writes them and the
 * firmware that checks them read one list; the last three are a refusal or skip that is not an
 * image check. */
#define CHECK_PASSED            0x00U   /* none: the copy passed */
#define CHECK_EMPTY             0x01U   /* the copy is empty */
#define CHECK_MAGIC             0x02U   /* proprietary string */
#define CHECK_FORMAT            0x03U   /* format */
#define CHECK_KIND              0x04U   /* kind */
#define CHECK_TARGET            0x05U   /* target */
#define CHECK_LENGTH            0x06U   /* length */
#define CHECK_START             0x07U   /* start address outside the region */
#define CHECK_BODY              0x08U   /* body check value */
#define CHECK_SIGNATURE_A       0x09U   /* signature A */
#define CHECK_SIGNATURE_B       0x0AU   /* signature B */
/* Added by decision 4 (owner, 13 Oct 2026): */
#define CHECK_FLASH_READ        0x0BU   /* a flash read raised an ECC error during the checks */
#define CHECK_TRIAL_STARTS      0x0CU   /* trial started too often: given up at its start limit */
#define CHECK_RESTARTS          0x0DU   /* restarted too often: decision B skipped the confirmed
                                         * slot */
/* Added by the owner's decisions of 13 Oct 2026 (second round): the check results live in this
 * header only, next to the record that holds them. Each is a trial or copy passed over without a
 * check of its image. */
#define CHECK_WATCHDOG_NOT_RUNNING 0x0EU   /* the trial was held back: the watchdog was not
                                            * confirmed running (decision F(b)) */
#define CHECK_START_NOT_COUNTED 0x0FU   /* the trial was not started: count_trial_start could not
                                         * save its start count */
#define CHECK_STATE_DAMAGED     0x10U   /* the boot state is damaged (decision D): slot A and slot B
                                         * were not checked, only the last-resort firmware is
                                         * tried */
/* The highest code of the table. */
#define CHECK_LAST              CHECK_STATE_DAMAGED

#endif /* BACKUP_LAYOUT_H */
