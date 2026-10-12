/*
 * fake_port.c — see fake_port.h. Chip rules copied from RM0440 Rev 9:
 *   erase: a 2 KB page at a page boundary, to 0xFF (Sec. 5.3.6, p. 195);
 *   program: 8 bytes at an 8-byte aligned address, only over erased bytes (Sec. 5.3.7, p. 196).
 */
#include "fake_port.h"

#include <stdio.h>
#include <string.h>

#include "update_port.h"

#define ERASED        0xFFU
#define WORD_BYTES    8U
/* Region patterns, so a stray write is visible. */
#define FILL_BOOTLOADER   0xB0U
#define FILL_SLOT_A       0xAAU
#define FILL_SLOT_B       0xBBU
#define FILL_LAST_RESORT  0x1EU
#define FILL_BOOT_STATE   0x5EU

fake_t fake;

static void fill(uint32_t start, uint32_t bytes, uint8_t value)
{
    memset(&fake.flash[start - (uint32_t)FLASH_START], value, bytes);
}

void fake_reset(void)
{
    memset(&fake, 0, sizeof fake);
    fill(BOOTLOADER_START, BOOTLOADER_BYTES, FILL_BOOTLOADER);
    fill(SLOT_A_START, SLOT_BYTES, FILL_SLOT_A);
    fill(SLOT_B_START, SLOT_BYTES, FILL_SLOT_B);
    fill(LAST_RESORT_START, LAST_RESORT_BYTES, FILL_LAST_RESORT);
    fill(BOOT_STATE_START, 2U * BOOT_STATE_COPY_BYTES, FILL_BOOT_STATE);
    fake.state_valid = true;
    fake.state.confirmed = BOOT_SLOT_A;
    fake.state.trial = BOOT_SLOT_NONE;
    fake.allowed_slot = BOOT_SLOT_B;
}

uint32_t fake_count_not(uint32_t start, uint32_t bytes, uint8_t value)
{
    uint32_t count = 0U;

    for (uint32_t i = 0U; i < bytes; i++) {
        count += (fake.flash[start - (uint32_t)FLASH_START + i] != value) ? 1U : 0U;
    }
    return count;
}

/* True if [address, address + bytes) is inside the slot the test allows. */
static bool in_allowed_slot(uint32_t address, uint32_t bytes)
{
    uint32_t start = (fake.allowed_slot == BOOT_SLOT_A) ? (uint32_t)SLOT_A_START
                                                        : (uint32_t)SLOT_B_START;
    return (address >= start) && (bytes <= (uint32_t)SLOT_BYTES) &&
           ((address - start) <= ((uint32_t)SLOT_BYTES - bytes));
}

static bool in_flash(uint32_t address, uint32_t bytes)
{
    return (address >= (uint32_t)FLASH_START) && (bytes <= FAKE_FLASH_BYTES) &&
           ((address - (uint32_t)FLASH_START) <= (FAKE_FLASH_BYTES - bytes));
}

bool update_port_boot_state(boot_state_t *state)
{
    *state = fake.state;
    return fake.state_valid;
}

bool update_port_erase_page(uint32_t page_start)
{
    if (!in_allowed_slot(page_start, FLASH_PAGE_BYTES)) {
        fake.outside++;
        return false;
    }
    if (fake.fail_erase || (((page_start - (uint32_t)FLASH_START) % FLASH_PAGE_BYTES) != 0U)) {
        return false;
    }
    fill(page_start, FLASH_PAGE_BYTES, ERASED);
    fake.erases++;
    return true;
}

bool update_port_program(uint32_t start, const uint32_t *words, uint32_t pairs)
{
    uint32_t bytes = pairs * WORD_BYTES;

    if (!in_allowed_slot(start, bytes)) {
        fake.outside++;
        return false;
    }
    if (fake.fail_program || ((start % WORD_BYTES) != 0U) ||
        (fake_count_not(start, bytes, ERASED) != 0U)) {
        return false;   /* PGAERR, or PROGERR: written over bytes that were not erased */
    }
    memcpy(&fake.flash[start - (uint32_t)FLASH_START], words, bytes);
    fake.programs++;
    return true;
}

const uint8_t *update_port_flash(uint32_t address)
{
    if (!in_flash(address, 1U)) {
        fake.bad_reads++;
        printf("fake_port: read of 0x%08x, outside the flash\n", address);
        return fake.flash;
    }
    return &fake.flash[address - (uint32_t)FLASH_START];
}

void update_port_ecc_clear(void)
{
}

bool update_port_ecc_error_seen(void)
{
    return fake.ecc_error;
}

void update_port_watchdog_reload(void)
{
    fake.reloads++;
}
