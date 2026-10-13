/*
 * fault_codes.h — the numbered codes the controller sends as the detail of two EVENT reports
 * (Interfaces Sec. 5.4): the fault kind of EVENT 0x0001 "fault detected", and the fault location
 * of EVENT 0x0005 "change in memory detected" (with 0x80 added when the bootloader found it).
 * Owner's decisions 2 and 3, 13 Oct 2026.
 *
 * One shared header, so the bootloader, the firmware and the ground decode one list (code rule
 * A11). Definitions only, like layout.h: no code is shared (REQ-119). Codes are never reused: a
 * code whose check is removed stays in this file, marked retired (code rule C4).
 *
 * ---- Fault location (decision 3) ---------------------------------------------------------------
 * One code per function that contains a fault site: a call of the fault reaction
 * (common/fault.h, firmware/fault.hpp). Two sites in one function share its code; two functions
 * never share one (test/controller/check_fault_codes.py checks both). The code is the function's,
 * not the program's: a shared function in common/ has one code whether the bootloader or the
 * firmware runs it. Each code is named FAULT_AT_ and the function's name (class and function for
 * a C++ member), in capitals.
 *   0x01-0x0F  bootloader/         (the bootloader leaves its code in the backup registers;
 *                                   the firmware that runs next sends it once)
 *   0x10-0x1F  common/             (run by both programs)
 *   0x20-0x4F  firmware/           (sent by the firmware's fault reaction before its restart)
 * Above each code: the file and function, then the value the check finds impossible.
 */
#ifndef FAULT_CODES_H
#define FAULT_CODES_H

/* ---- Fault kind: the 1-byte detail of EVENT 0x0001 (decision 2) ------------------------------ */

/* firmware/core_clock.cpp, CoreClock::start: the core clock failed; the controller runs at
 * 16 MHz from HSI16 (decision C). */
#define FAULT_KIND_CORE_CLOCK                   0x01U

/* ---- Fault location: the first byte of EVENT 0x0005's 2-byte detail (decision 3) ------------- */

/* Bit 7 of the first detail byte: set by the firmware, never by a location code, when the fault
 * was found by the BOOTLOADER at the last start (owner, 13 Oct 2026, second round: 0x80 + code).
 * A location the firmware finds itself is sent as it is. The second byte is the number of
 * faults found after the first (backup_layout.h, FAULT_LATER); 0 for the firmware's own. */
#define FAULT_FOUND_BY_BOOTLOADER               0x80U

/* The ranges, as the owner set them (decision 3). */
#define FAULT_RANGE_BOOTLOADER_FIRST            0x01U
#define FAULT_RANGE_BOOTLOADER_LAST             0x0FU
#define FAULT_RANGE_COMMON_FIRST                0x10U
#define FAULT_RANGE_COMMON_LAST                 0x1FU
#define FAULT_RANGE_FIRMWARE_FIRST              0x20U
#define FAULT_RANGE_FIRMWARE_LAST               0x4FU

/* Bit 7 is the bootloader mark, so no location code may use it. The ranges stop at 0x4F; this
 * stops the build if a range is ever moved up to 0x80 or more. check_fault_codes.py checks each
 * FAULT_AT_ code the same way. */
#ifdef __cplusplus
static_assert((FAULT_RANGE_FIRMWARE_LAST & FAULT_FOUND_BY_BOOTLOADER) == 0U,
              "fault-location codes must not use bit 7 (the bootloader mark)");
static_assert(FAULT_RANGE_FIRMWARE_LAST < FAULT_FOUND_BY_BOOTLOADER,
              "every fault-location range lies below 0x80");
#else
_Static_assert((FAULT_RANGE_FIRMWARE_LAST & FAULT_FOUND_BY_BOOTLOADER) == 0U,
               "fault-location codes must not use bit 7 (the bootloader mark)");
_Static_assert(FAULT_RANGE_FIRMWARE_LAST < FAULT_FOUND_BY_BOOTLOADER,
               "every fault-location range lies below 0x80");
#endif

/* -- bootloader/ -- */

/* bootloader/backup.c, value_index: a backup pair number at or past BACKUP_PAIRS. */
#define FAULT_AT_VALUE_INDEX                    0x01U
/* bootloader/main.c, slot_letter: a slot code that is not A, B or none. */
#define FAULT_AT_SLOT_LETTER                    0x02U
/* bootloader/main.c, reset_cause_text: a reset cause outside reset_cause_t. */
#define FAULT_AT_RESET_CAUSE_TEXT               0x03U
/* bootloader/main.c, load_state: a boot_state_load result outside boot_state_load_t. */
#define FAULT_AT_LOAD_STATE                     0x04U
/* bootloader/main.c, slot_region: a slot code that is not A or B. */
#define FAULT_AT_SLOT_REGION                    0x05U
/* bootloader/main.c, role_text: a start role that is not confirmed, trial or last resort. */
#define FAULT_AT_ROLE_TEXT                      0x06U
/* bootloader/main.c, check_result_code: an image check result outside image_result_t. */
#define FAULT_AT_CHECK_RESULT_CODE              0x07U

/* -- common/ -- */

/* common/image_check.c, image_result_text: an image check result outside image_result_t. */
#define FAULT_AT_IMAGE_RESULT_TEXT              0x10U
/* common/boot_state.c, boot_state_other_slot: a slot code that is not A or B. */
#define FAULT_AT_BOOT_STATE_OTHER_SLOT          0x11U

/* -- firmware/ -- */

/* firmware/boot_slot.hpp, toSlot: a stored slot code that is not none, A or B. */
#define FAULT_AT_TO_SLOT                        0x20U
/* firmware/boot_slot.hpp, otherSlot: Slot::kNone, or a value outside Slot. */
#define FAULT_AT_OTHER_SLOT                     0x21U
/* firmware/boot_slot.hpp, stateCode: a value outside Slot. */
#define FAULT_AT_STATE_CODE                     0x22U
/* firmware/core_clock.cpp, CoreClock::startTick: a core clock that is not 170 MHz or 16 MHz. */
#define FAULT_AT_CORE_CLOCK_START_TICK          0x23U
/* firmware/data-link/uart_link.cpp, UartLink::init: a core clock that is not 170 MHz or
 * 16 MHz. */
#define FAULT_AT_UART_LINK_INIT                 0x24U
/* firmware/health-monitor/health_monitor.cpp, slotCode: a value outside Slot. */
#define FAULT_AT_SLOT_CODE                      0x25U
/* firmware/update-receiver/update_receiver.cpp, UpdateReceiver::slotStart: a value outside
 * Slot. */
#define FAULT_AT_UPDATE_RECEIVER_SLOT_START     0x26U
/* firmware/update-receiver/update_receiver.cpp, UpdateReceiver::refusalFor: an image check
 * result outside image_result_t. */
#define FAULT_AT_UPDATE_RECEIVER_REFUSAL_FOR    0x27U
/* firmware/update-receiver/update_receiver.cpp, UpdateReceiver::valuesFit: an upload
 * identifier outside UploadId. */
#define FAULT_AT_UPDATE_RECEIVER_VALUES_FIT     0x28U
/* firmware/update-receiver/update_receiver.cpp, UpdateReceiver::run: an upload identifier
 * outside UploadId. */
#define FAULT_AT_UPDATE_RECEIVER_RUN            0x29U
/* firmware/command-checker/arm_state.cpp, ArmState::checkIntact: a word of the arm state that
 * differs from its complement, or a state word that is none of the three code words. */
#define FAULT_AT_ARM_STATE_CHECK_INTACT         0x2AU
/* firmware/mode-manager/test_image.cpp, TestImage::accept: an upload identifier outside
 * UploadId. */
#define FAULT_AT_TEST_IMAGE_ACCEPT              0x2BU
/* firmware/mode-manager/test_image.cpp, TestImage::run: an upload identifier outside UploadId. */
#define FAULT_AT_TEST_IMAGE_RUN                 0x2CU
/* firmware/mode-manager/mode_manager.cpp, copyCode: a value outside Slot. */
#define FAULT_AT_COPY_CODE                      0x2DU
/* firmware/mode-manager/mode_manager.cpp, ModeManager::poll: the mode FALLBACK, which this
 * build never sets, or a value outside HealthMode. */
#define FAULT_AT_MODE_MANAGER_POLL              0x2EU
/* firmware/mode-manager/mode_manager.cpp, ModeManager::runDangerous: a command identifier that
 * is neither SWITCH nor CONFIRM. */
#define FAULT_AT_MODE_MANAGER_RUN_DANGEROUS     0x2FU

#endif /* FAULT_CODES_H */
