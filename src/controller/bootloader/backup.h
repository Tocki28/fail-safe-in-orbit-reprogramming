/*
 * backup.h — the bootloader's access to the TAMP backup registers: words that survive a system
 * reset (common/backup_layout.h has the map and says why these registers).
 *
 * Each value is stored as a pair, the value and its bitwise complement; a pair that does not
 * match reads as empty, so one bit error cannot fake a value (decision B). The decisions on what
 * to store are in main.c; this module only reads and writes pairs.
 */
#ifndef BACKUP_H
#define BACKUP_H

#include <stdbool.h>
#include <stdint.h>

/* Turn on the PWR and RTC APB clocks and lift the backup-domain write protection (DBP). Call
 * before the first backup_read or backup_write. */
void backup_open(void);

/* Put the write protection back (DBP = 0) and turn the RTC APB clock off. Call before the
 * hand-over, while the PWR clock is still on (clock_back_to_reset turns it off). */
void backup_close(void);

/* Read pair `pair` (BACKUP_PAIR_...). True and *value set if the two words are complements;
 * false (and *value 0) if they are not: the pair is empty. */
bool backup_read(uint32_t pair, uint32_t *value);

/* Write `value` and its complement into pair `pair`, then read both back. True if they read back
 * as written. */
bool backup_write(uint32_t pair, uint32_t value);

#endif /* BACKUP_H */
