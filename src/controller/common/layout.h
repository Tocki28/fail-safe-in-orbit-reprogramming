/*
 * layout.h — where everything sits in the controller's flash and RAM (STM32G431KB, bench).
 *
 * One file, so the bootloader, the firmware, the linker script and the image tool cannot
 * disagree. The linker script (program.ld) reads it through the C preprocessor, so this file
 * holds #define lines only. The values carry no U suffix because the linker does not accept
 * one; every value is below 2^31, so C reads each as a positive int.
 * The Python tools read the plain-number lines of this file (tools/firmware_layout.py).
 */
#ifndef LAYOUT_H
#define LAYOUT_H

/* ---- The chip ---------------------------------------------------------------------- */

/* Main flash starts at 0x0800 0000 [RM0440 Rev 9, Table 28, p. 189]. */
#define FLASH_START             0x08000000
/* 2 KB pages: the smallest unit an erase clears [RM0440 Rev 9, Sec. 5.3.1, p. 189]. */
#define FLASH_PAGE_BYTES        0x800
/* The STM32G431KB main flash is 128 KB: pages 0 to 63 of 2 KB, 0x0800 0000 to 0x0801 FFFF
 * [RM0440 Rev 9, Table 28, p. 189]. common/flash.c refuses every address at or past its end. */
#define FLASH_BYTES             0x20000
/* SRAM1 16 KB + SRAM2 6 KB + CCM SRAM 10 KB form one block from 0x2000 0000
 * [RM0440 Rev 9, Sec. 2.4, p. 88-89]. */
#define RAM_START               0x20000000
#define RAM_BYTES               0x8000

/* ---- Flash map: 128 KB = 24 + 40 + 40 + 16 + 8 KB ---------------------------------- */

/* The chip starts here when BOOT0 selects main flash [RM0440 Rev 9, Table 5, p. 91].
 * 24 KB: room for the bootloader with the Ed25519 check (bench size; arm-none-eabi-size
 * prints the actual use at every build). */
#define BOOTLOADER_START        0x08000000
#define BOOTLOADER_BYTES        0x6000

/* Two equal slots, so a new copy never overwrites the confirmed one (REQ-007). */
#define SLOT_A_START            0x08006000
#define SLOT_B_START            0x08010000
#define SLOT_BYTES              0xA000

/* Last-resort firmware: its own region that no update writes (REQ-008). */
#define LAST_RESORT_START       0x0801A000
#define LAST_RESORT_BYTES       0x4000

/*
 * Boot state: the record that tells the bootloader which slot is confirmed, which slot is on
 * trial, and how many times the trial has started. The record is 16 bytes (boot_state.h).
 * It is kept twice, in the last 8 KB of flash: copy 0 at 0x0801 E000, copy 1 at 0x0801 F000,
 * 4 KB (two 2 KB pages) each. These are pages 60 to 63 of the 64 [RM0440 Rev 9, Table 28,
 * p. 189].
 *
 * Why two copies: a copy is rewritten by erasing it first, which turns every byte into 0xFF
 * [RM0440 Rev 9, Sec. 5.3.7, p. 197], then programming the new record. A power cut between
 * the two steps leaves that copy blank. Each write therefore goes to the older copy, and the
 * newer copy stays readable (REQ-017). Each copy has a sequence number and a CRC-32, so the
 * bootloader uses the newest copy that is intact (REQ-136).
 * Why separate pages: a page is the smallest part an erase clears [RM0440 Rev 9, Sec. 5.3.6,
 * p. 195], so two copies in one page would be erased together.
 * Why at the end of flash: the chip starts from the start of flash, so the bootloader is
 * there [RM0440 Rev 9, Table 5, p. 91]. The boot state at the other end leaves the slots and
 * the last-resort region in one piece between them.
 * Why 4 KB per copy: no technical reason. One 2 KB page per copy would hold the 16 bytes; the
 * size keeps the flash map in round numbers, and the rest of each copy is spare.
 */
#define BOOT_STATE_START        0x0801E000
#define BOOT_STATE_COPY_BYTES   0x1000
#define BOOT_STATE_COPY_0       BOOT_STATE_START
#define BOOT_STATE_COPY_1       (BOOT_STATE_START + BOOT_STATE_COPY_BYTES)

/* ---- Image placement inside a region (Interfaces Sec. 5.5, Figures 5 and 6) -------- */

/* The image header is 64 bytes (Interfaces Sec. 5.5, Figure 6). */
#define IMAGE_HEADER_BYTES      64
/* Two Ed25519 signatures of 64 bytes follow the body (Interfaces Sec. 5.5, Figure 5). */
#define IMAGE_SIGNATURE_BYTES   64
#define IMAGE_SIGNATURES_BYTES  (2 * IMAGE_SIGNATURE_BYTES)
/* The program's vector table sits 0x200 bytes into the region. VTOR needs the table aligned
 * to its size rounded up to a power of two [Arm DUI 0553A, Sec. 4.3.4, p. 4-16]; the G431
 * table has 118 words = 472 bytes (startup_stm32g431xx.s), so 512. The image tool fills the
 * 448 bytes between the header and the table with zeros, so header and body stay in one
 * piece in flash. */
#define IMAGE_CODE_OFFSET       0x200

/* Where each program is linked, and how much room its code has. A slot program's code ends
 * where the two signatures must still fit. */
#define SLOT_A_CODE_START       (SLOT_A_START + IMAGE_CODE_OFFSET)
#define SLOT_B_CODE_START       (SLOT_B_START + IMAGE_CODE_OFFSET)
#define SLOT_CODE_BYTES         (SLOT_BYTES - IMAGE_CODE_OFFSET - IMAGE_SIGNATURES_BYTES)
#define LAST_RESORT_CODE_START  (LAST_RESORT_START + IMAGE_CODE_OFFSET)
#define LAST_RESORT_CODE_BYTES  (LAST_RESORT_BYTES - IMAGE_CODE_OFFSET - IMAGE_SIGNATURES_BYTES)

#endif /* LAYOUT_H */
