/*
 * update_receiver.cpp — see update_receiver.hpp. No register access: the chip is reached
 * through update_port.hpp, so the same file runs in the host unit tests (test/controller/unit).
 *
 * The checks, in order:
 *   1. the payload's size fits the command (0x06);
 *   2. the slot: neither confirmed nor running (chooseTarget); none during a trial (0x07).
 *      Chosen again for every command from the boot state as it is now, so a trial set after
 *      BEGIN stops the next CHUNK instead of writing into the trial slot;
 *   3. the command's values (0x06): BEGIN's length fits a slot; a CHUNK continues the transfer
 *      at exactly the next byte; END comes after the last byte;
 *   4. before every erase and write, the address range again (writeAllowed).
 *
 * Flash faults: Interfaces Sec. 4.2 has no code for "the flash did not take the write"; 0x08
 * "image damaged" is the nearest, since the copy in the slot can no longer be trusted. The
 * transfer is then closed; CANCEL or a new BEGIN starts over.
 *
 * Switches over an enum held in memory have no default label. No default: the compiler checks
 * every case (-Wswitch, -Werror); only a corrupted value reaches the line after the switch, which
 * calls the fault reaction (firmware/fault.hpp). A deliberate deviation from MISRA C:2012 Rule
 * 16.4, whose purpose, handling a value outside the cases, the fault reaction meets. The one
 * switch over a byte received from the link (sizeFits) keeps a refusing default: that byte is
 * input, refused 0x06, not a fault.
 */
#include "update_receiver.hpp"

#include <array>
#include <cstring>

#include "fault.hpp"
#include "image_format.h"
#include "layout.h"
#include "update_port.hpp"

namespace {

/* Payload layout (Interfaces Sec. 5.1): identifier (1), image number (2), then for BEGIN the
 * image length (4) and for CHUNK the offset (4) and the data. */
constexpr uint32_t kIdBytes = 1U;
constexpr uint32_t kNumberBytes = 2U;
constexpr uint32_t kValueBytes = 4U;
constexpr uint32_t kShortPayload = kIdBytes + kNumberBytes;       /* END, CANCEL: 3 bytes */
constexpr uint32_t kBeginPayload = kShortPayload + kValueBytes;   /* 7 bytes */
constexpr uint32_t kChunkHead = kBeginPayload;                    /* CHUNK data starts here */
/* The chip programs one 64-bit flash word at a time [RM0440 Rev 9, Sec. 5.3.7, p. 196]. */
constexpr uint32_t kFlashWordBytes = 8U;
constexpr uint32_t kChunkWords = kUploadChunkMax / sizeof(uint32_t);
/* A last flash word only partly filled is completed with the erased value 0xFF, which leaves
 * those bytes erased [RM0440 Rev 9, Sec. 5.3.7, p. 197]. */
constexpr uint8_t kErasedByte = 0xFFU;
/* layout.h's values have no U suffix (the linker script reads the same file), so they are
 * ints; typed once here instead of a cast at every use. */
constexpr uint32_t kSlotAStart = SLOT_A_START;
constexpr uint32_t kSlotBStart = SLOT_B_START;
constexpr uint32_t kSlotBytes = SLOT_BYTES;
constexpr uint32_t kFlashPageBytes = FLASH_PAGE_BYTES;
/* The smallest image: header and two signatures (Interfaces Sec. 5.5, Figure 5). */
constexpr uint32_t kImageMinBytes = IMAGE_HEADER_BYTES + IMAGE_SIGNATURES_BYTES;

/**
 * @brief Reads a little-endian 16-bit value from two bytes.
 *
 * @param[in] bytes  pointer to the low byte; the high byte follows
 * @return the value, byte 0 low and byte 1 high
 */
uint16_t readU16(const uint8_t *bytes)
{
    return static_cast<uint16_t>(static_cast<uint32_t>(bytes[0]) |
                                 (static_cast<uint32_t>(bytes[1]) << 8));
}

/* ---- Checks on arrival that need no state ------------------------------------------- */

/**
 * @brief Checks that a payload's length fits the command its identifier names.
 *
 * BEGIN is exactly 7 bytes, CHUNK is 7 bytes plus 1 to 256 data bytes, END and CANCEL are
 * exactly 3 bytes. Any other identifier does not fit.
 *
 * All four UploadId values have their own case (-Wswitch-enum, common/build.mk). The default is
 * for a byte that is none of them: `id` arrives from the wire as a plain byte, so a value outside
 * the enum is the normal case here, not a corruption. It answers false: refused 0x06 by the
 * caller. This is input from the link, so the owner's fault rule (firmware/fault.hpp) does not
 * apply here.
 *
 * @param[in] id      command identifier, first payload byte
 * @param[in] length  payload bytes
 * @return true if the length fits the command, false otherwise
 */
bool sizeFits(uint8_t id, uint32_t length)
{
    switch (static_cast<UploadId>(id)) {
    case UploadId::kBegin:
        return length == kBeginPayload;
    case UploadId::kChunk:
        return (length > kChunkHead) && (length <= (kChunkHead + kUploadChunkMax));
    case UploadId::kEnd:
    case UploadId::kCancel:
        return length == kShortPayload;
    default:   /* input from the link, not memory: refused, not a fault */
        return false;
    }
}

}  // namespace

/* ---- The rules ---------------------------------------------------------------------- */

/**
 * @brief Runs the bootloader's checks on the image in a slot and returns the answer code.
 *
 * The bootloader's own checks, in its order (bootloader/main.c, image_passes): contents, then
 * both signatures, then any flash read error seen on the way. A static member since decision F(a)
 * (owner, 13 Oct 2026; before, private to this file): SWITCH checks the other slot with these
 * same checks before it writes the boot state (mode-manager/mode_manager.cpp, runSwitch), so
 * UPLOAD END and SWITCH cannot disagree on an image.
 *
 * @param[in] slot  slot holding the image (Slot::kA or Slot::kB)
 * @return kUpdateOk if the image passes, otherwise the refusal code for the first fault;
 *         AnswerCode::kDamaged if a flash read error was seen
 */
AnswerCode UpdateReceiver::checkSlot(Slot slot)
{
    uint32_t start = UpdateReceiver::slotStart(slot);
    const uint8_t *image = UpdatePort::flash(start);
    uint32_t signed_bytes = 0U;

    UpdatePort::eccClear();
    image_result_t result = image_check_contents(image, start, kSlotBytes, IMAGE_KIND_MAIN,
                                                 &signed_bytes);
    if (result == IMAGE_OK) {
        result = image_check_signatures(image, signed_bytes);
    }
    if (UpdatePort::eccErrorSeen()) {
        return AnswerCode::kDamaged;
    }
    return UpdateReceiver::refusalFor(result);
}

/**
 * @brief Finds the slot that contains a chip address.
 *
 * @param[in] address  chip address
 * @return Slot::kA or Slot::kB if the address lies in that slot, otherwise Slot::kNone
 */
Slot UpdateReceiver::slotOf(uint32_t address)
{
    /* Unsigned: an address below the slot wraps to a large number and fails the test. */
    if ((address - kSlotAStart) < kSlotBytes) {
        return Slot::kA;
    }
    if ((address - kSlotBStart) < kSlotBytes) {
        return Slot::kB;
    }
    return Slot::kNone;
}

/**
 * @brief Gives the first byte of a slot.
 *
 * Slot::kNone has no first byte: 0, which every caller treats as "no slot" (ModeManager::selfTest
 * fails, writeAllowed refuses, main.cpp reports no header). A value outside the Slot enum is a
 * corrupted variable: the fault reaction (see the file comment). Before, it also gave 0.
 *
 * @param[in] slot  Slot::kNone, Slot::kA or Slot::kB
 * @return chip address of the slot's first byte; 0 for Slot::kNone
 * @note Does not return for a value outside Slot.
 */
uint32_t UpdateReceiver::slotStart(Slot slot)
{
    switch (slot) {
    case Slot::kA:
        return kSlotAStart;
    case Slot::kB:
        return kSlotBStart;
    case Slot::kNone:
        return 0U;
    }
    faultImpossibleValue("slotStart");   /* only a corrupted value reaches here */
}

/**
 * @brief Chooses the slot an upload may write: not confirmed, not running.
 *
 * Refuses without a valid boot state (the confirmed slot is not known) and during a trial.
 * Otherwise the target is the slot the firmware does not run from.
 *
 * @param[in]  state_valid  true if the boot state was read
 * @param[in]  state        the boot state
 * @param[in]  running      slot this firmware runs from
 * @param[out] target       the chosen slot, or Slot::kNone on a refusal
 * @return kUpdateOk with target set, otherwise the refusal code (kImpossible or kTrial)
 */
AnswerCode UpdateReceiver::chooseTarget(bool state_valid, const boot_state_t &state, Slot running,
                                        Slot &target)
{
    target = Slot::kNone;
    /* Without a boot state the confirmed slot is not known, so no slot is safe to write. */
    if (!state_valid) {
        return AnswerCode::kImpossible;
    }
    /* The only slot that may be written is the trial's own (REQ-050, Interfaces Sec. 5.1). */
    if (toSlot(state.trial) != Slot::kNone) {
        return AnswerCode::kTrial;
    }
    /* Running a slot that is not confirmed, with no trial: the other slot is the confirmed
     * one, so no slot is both not confirmed and not running (REQ-007). */
    if (((running != Slot::kA) && (running != Slot::kB)) || (toSlot(state.confirmed) != running)) {
        return AnswerCode::kImpossible;
    }
    target = otherSlot(running);   /* running is A or B here: checked just above */
    return kUpdateOk;
}

/**
 * @brief Checks that a flash range may be erased or written (the second barrier).
 *
 * Asked before every erase and write. Offsets are taken from the slot start, so no sum wraps
 * past 2^32.
 *
 * @param[in] target   slot to be written
 * @param[in] running  slot this firmware runs from
 * @param[in] address  chip address of the first byte
 * @param[in] bytes    length of the range in bytes
 * @return true only if the range lies inside slot target, target is slot A or B and is not
 *         running, and bytes is 1 to the slot size; false otherwise
 */
bool UpdateReceiver::writeAllowed(Slot target, Slot running, uint32_t address, uint32_t bytes)
{
    uint32_t start = slotStart(target);

    if ((start == 0U) || (target == running) || (bytes == 0U) || (bytes > kSlotBytes)) {
        return false;
    }
    /* As offsets from the slot start, so no sum can wrap past 2^32. */
    return (address >= start) && ((address - start) <= (kSlotBytes - bytes));
}

/**
 * @brief Maps a result of the image checks to an answer code (Interfaces Sec. 4.2).
 *
 * Every image_result_t value has its own case, so -Wswitch-enum (common/build.mk) stops the
 * build when a check result is added and not mapped here. Before, a `default` lumped the damage
 * results together and also took any result added later. The mapping:
 *   IMAGE_OK                                  -> kUpdateOk
 *   IMAGE_BAD_SIGNATURE_A, _B                 -> kNotOperator (0x09)
 *   IMAGE_BAD_TARGET                          -> kOtherController (0x0B)
 *   IMAGE_BAD_START                           -> kOtherSlot (0x0E)
 *   IMAGE_EMPTY, _BAD_MAGIC, _BAD_FORMAT, _BAD_LENGTH, _BAD_BODY_CHECK -> kDamaged (0x08)
 *   IMAGE_BAD_KIND                            -> kWrongKind (0x10)
 * IMAGE_BAD_KIND (a bootloader or last-resort image sent for a main slot) answers 0x10 "image of
 * the wrong kind for its place", the code the Interfaces document added for it (Sec. 4.2, accepted
 * by the owner 13 Oct 2026, decision M); before, it answered the nearest code, 0x08.
 * A value outside the enum is a corrupted variable: the fault reaction (see the file comment);
 * before, a default refused the image with 0x08.
 *
 * @param[in] result  result of image_check_contents or image_check_signatures
 * @return the answer code of the table above
 * @note Does not return for a value outside image_result_t.
 */
AnswerCode UpdateReceiver::refusalFor(image_result_t result)
{
    switch (result) {
    case IMAGE_OK:
        return kUpdateOk;
    case IMAGE_BAD_SIGNATURE_A:
    case IMAGE_BAD_SIGNATURE_B:
        return AnswerCode::kNotOperator;
    case IMAGE_BAD_TARGET:
        return AnswerCode::kOtherController;
    case IMAGE_BAD_START:
        return AnswerCode::kOtherSlot;   /* reset handler outside this slot: linked for the other */
    case IMAGE_EMPTY:
    case IMAGE_BAD_MAGIC:
    case IMAGE_BAD_FORMAT:
    case IMAGE_BAD_LENGTH:
    case IMAGE_BAD_BODY_CHECK:
        return AnswerCode::kDamaged;
    case IMAGE_BAD_KIND:
        return AnswerCode::kWrongKind;
    }
    faultImpossibleValue("refusalFor");   /* only a corrupted value reaches here */
}

/* ---- Checks on arrival -------------------------------------------------------------- */

/**
 * @brief Tells whether a payload starts with an upload command identifier.
 *
 * @param[in] payload  command payload, identifier first
 * @param[in] length   payload bytes
 * @return true if the identifier is one of the four upload commands (0x01 to 0x04), false
 *         otherwise or if the payload is empty
 */
bool UpdateReceiver::isUpload(const uint8_t *payload, uint16_t length)
{
    return (length >= kIdBytes) && (payload[0] >= static_cast<uint8_t>(UploadId::kBegin)) &&
           (payload[0] <= static_cast<uint8_t>(UploadId::kCancel));
}

/**
 * @brief Reads the fields of an upload payload (Interfaces Sec. 5.1, little-endian, Sec. 4.0).
 *
 * A static member since step 2d (before: a function private to this file): main.cpp reads the
 * image number with it to send the test image, number 0xFFFF, to the mode manager
 * (mode-manager/test_image.hpp) and never into this receiver. One reader keeps one payload
 * layout for both. Its body is unchanged.
 *
 * @param[in]     payload  command payload, identifier first
 * @param[in]     length   payload bytes
 * @param[out]    cmd      the fields; every field 0 and target Slot::kNone if the size is wrong
 * @return true if the payload's size fits its command, false otherwise
 */
bool UpdateReceiver::parse(const uint8_t *payload, uint16_t length, UpdateCommand &cmd)
{
    cmd = UpdateCommand{};   /* every field 0, target Slot::kNone (C: memset to 0) */
    if ((length < kIdBytes) || !sizeFits(payload[0], length)) {
        return false;
    }
    cmd.id = static_cast<UploadId>(payload[0]);
    cmd.image_number = readU16(&payload[kIdBytes]);
    if (length >= kBeginPayload) {
        cmd.value = image_read_u32(&payload[kShortPayload]);
    }
    if (cmd.id == UploadId::kChunk) {
        cmd.data = &payload[kChunkHead];
        cmd.data_bytes = static_cast<uint32_t>(length) - kChunkHead;
    }
    return true;
}

/**
 * @brief Checks that a command names the transfer that BEGIN opened.
 *
 * The command names the transfer that BEGIN opened, into the same slot.
 *
 * @param[in] cmd  the command
 * @return true if a transfer is open and cmd has its image number and target slot, false
 *         otherwise
 */
bool UpdateReceiver::sameTransfer(const UpdateCommand &cmd) const
{
    return (phase_ != UpdatePhase::kIdle) && (cmd.image_number == image_number_) &&
           (cmd.target == target_);
}

/**
 * @brief Checks a CHUNK's offset and size against the transfer.
 *
 * A chunk starts at the next byte not yet written and stays inside the announced length.
 * Every chunk but the last is a whole number of flash words, so the next chunk starts on an
 * unwritten flash word: a written one cannot be written again [RM0440 Rev 9, Sec. 5.3.7,
 * p. 196].
 *
 * @param[in] cmd  the CHUNK command
 * @return true if the chunk fits, false otherwise
 */
bool UpdateReceiver::chunkFits(const UpdateCommand &cmd) const
{
    uint32_t room = length_ - written_;   /* written_ never exceeds length_ */
    bool last = (cmd.data_bytes == room);

    return (cmd.value == written_) && (cmd.data_bytes <= room) &&
           (last || ((cmd.data_bytes % kFlashWordBytes) == 0U));
}

/**
 * @brief Checks a command's values against the transfer state.
 *
 * BEGIN: the length is from the smallest image to the slot size, in any phase. CHUNK: phase
 * receiving, same transfer, chunk fits. END: phase receiving, same transfer, all bytes
 * written. CANCEL: same transfer.
 *
 * All four UploadId values have their own case (-Wswitch-enum). cmd.id was set by parse() only
 * after sizeFits accepted the byte, so a value outside the enum is a corrupted variable: the fault
 * reaction (see the file comment). Before, a default answered false (refused 0x06).
 *
 * @param[in] cmd  the command
 * @return true if the values fit, false otherwise
 * @note Does not return for an identifier outside UploadId.
 */
bool UpdateReceiver::valuesFit(const UpdateCommand &cmd) const
{
    switch (cmd.id) {
    case UploadId::kBegin:
        /* Accepted whatever the phase: a BEGIN restarts an open transfer (runBegin). */
        return (cmd.value >= kImageMinBytes) && (cmd.value <= kSlotBytes);
    case UploadId::kChunk:
        return (phase_ == UpdatePhase::kReceiving) && sameTransfer(cmd) && chunkFits(cmd);
    case UploadId::kEnd:
        return (phase_ == UpdatePhase::kReceiving) && sameTransfer(cmd) && (written_ == length_);
    case UploadId::kCancel:
        return sameTransfer(cmd);
    }
    faultImpossibleValue("UpdateReceiver::valuesFit");   /* only a corrupted value reaches here */
}

/**
 * @brief Does the checks on arrival of an upload command.
 *
 * Parses the payload, chooses the target slot from the boot state as it is now, then checks
 * the command's values. Changes no transfer state.
 *
 * @param[in]  payload  command payload, identifier first
 * @param[in]  length   payload bytes
 * @param[out] cmd      the command, filled in by parse and the slot choice
 * @return kUpdateOk, or the refusal code (kImpossible, kTrial)
 */
AnswerCode UpdateReceiver::accept(const uint8_t *payload, uint16_t length, UpdateCommand &cmd) const
{
    boot_state_t state{};

    if (!parse(payload, length, cmd)) {
        return AnswerCode::kImpossible;
    }
    bool state_valid = UpdatePort::bootState(state);
    AnswerCode refusal = chooseTarget(state_valid, state, running_, cmd.target);
    if (refusal != kUpdateOk) {
        return refusal;
    }
    return valuesFit(cmd) ? kUpdateOk : AnswerCode::kImpossible;
}

/* ---- Running a command -------------------------------------------------------------- */

/**
 * @brief Erases one flash page of the target slot after the write-range check.
 *
 * @param[in] page  chip address of the page's first byte
 * @return true if the page is inside the target slot and the chip erased it, false otherwise
 */
bool UpdateReceiver::erasePage(uint32_t page) const
{
    return writeAllowed(target_, running_, page, kFlashPageBytes) && UpdatePort::erasePage(page);
}

/**
 * @brief Runs UPLOAD BEGIN: records the transfer and erases the whole target slot.
 *
 * A BEGIN while a transfer is open restarts it. The onboard computer is the only sender and
 * runs one transfer at a time, so a new BEGIN means it started over (after its own restart, or
 * for a new upload from the ground). Refusing it would block the slot until a CANCEL the
 * sender may not know it owes. Erasing the whole slot removes every byte of the old transfer.
 *
 * The phase is closed until the whole slot is erased, then receiving. The watchdog is reloaded
 * after each page.
 *
 * @param[in] cmd  the BEGIN command; cmd.value is the image length
 * @return kUpdateOk, or AnswerCode::kDamaged if a page erase failed
 */
AnswerCode UpdateReceiver::runBegin(const UpdateCommand &cmd)
{
    uint32_t start = slotStart(cmd.target);

    image_number_ = cmd.image_number;
    target_ = cmd.target;
    length_ = cmd.value;
    written_ = 0U;
    image_passed_ = false;
    phase_ = UpdatePhase::kClosed;   /* until the whole slot is erased */
    for (uint32_t page = start; page < (start + kSlotBytes); page += kFlashPageBytes) {
        /* One page erase stalls the core up to 24.47 ms [DS12589 Rev 6, Table 46, p. 107]; the
         * 20 pages up to 489 ms. Reloaded after each page, so the watchdog never sees the
         * whole erase as one pause. */
        bool erased = erasePage(page);
        UpdatePort::watchdogReload();
        if (!erased) {
            return AnswerCode::kDamaged;
        }
    }
    phase_ = UpdatePhase::kReceiving;
    return kUpdateOk;
}

/**
 * @brief Programs a chunk into flash and verifies it by reading it back.
 *
 * Program the chunk at `address`, then read it back through the ECC check. Byte i of the data
 * lands at address + i: the chip stores words little-endian [RM0440 Rev 9, Sec. 2.2.1, p. 82].
 * A last flash word only partly filled is completed with 0xFF.
 *
 * @param[in] address  chip address of the first byte
 * @param[in] cmd      the CHUNK command; its data and data_bytes are written
 * @return true if the write was allowed, the chip programmed it, the read-back matches and no
 *         ECC error was seen; false otherwise
 */
bool UpdateReceiver::writeChunk(uint32_t address, const UpdateCommand &cmd) const
{
    std::array<uint32_t, kChunkWords> words;
    uint32_t pairs = (cmd.data_bytes + kFlashWordBytes - 1U) / kFlashWordBytes;

    std::memset(words.data(), kErasedByte, sizeof(words));
    std::memcpy(words.data(), cmd.data, cmd.data_bytes);
    if (!writeAllowed(target_, running_, address, pairs * kFlashWordBytes) ||
        !UpdatePort::program(address, words.data(), pairs)) {
        return false;
    }
    UpdatePort::eccClear();
    bool same = (std::memcmp(UpdatePort::flash(address), cmd.data, cmd.data_bytes) == 0);
    return same && !UpdatePort::eccErrorSeen();
}

/**
 * @brief Runs UPLOAD CHUNK: writes the data at the chunk's offset in the target slot.
 *
 * On a failed write the phase becomes closed, since a half-written flash word cannot be
 * written again. Otherwise the written count grows by the chunk's size.
 *
 * @param[in] cmd  the CHUNK command; cmd.value is the offset
 * @return kUpdateOk, or AnswerCode::kDamaged if the write failed
 */
AnswerCode UpdateReceiver::runChunk(const UpdateCommand &cmd)
{
    if (!writeChunk(slotStart(target_) + cmd.value, cmd)) {
        phase_ = UpdatePhase::kClosed;   /* a half-written flash word cannot be written again */
        return AnswerCode::kDamaged;
    }
    written_ += cmd.data_bytes;
    return kUpdateOk;
}

/**
 * @brief Runs UPLOAD END: checks the image in the target slot and closes the transfer.
 *
 * Sets the phase to closed and records in image_passed_ whether the checks passed.
 *
 * @return kUpdateOk if the image passes, otherwise the refusal code from checkSlot
 */
AnswerCode UpdateReceiver::runEnd()
{
    AnswerCode result = checkSlot(target_);

    phase_ = UpdatePhase::kClosed;
    image_passed_ = (result == kUpdateOk);
    return result;
}

/**
 * @brief Runs UPLOAD CANCEL: erases the target slot's first page and ends the transfer.
 *
 * The header is in the slot's first page. Erased, the slot reads as empty to the bootloader
 * (common/image_check.c, IMAGE_EMPTY), which never starts it. On a failed erase the transfer
 * stays as it was, so CANCEL can be sent again.
 *
 * @return kUpdateOk, or AnswerCode::kDamaged if the erase failed
 */
AnswerCode UpdateReceiver::runCancel()
{
    if (!erasePage(slotStart(target_))) {
        return AnswerCode::kDamaged;
    }
    phase_ = UpdatePhase::kIdle;
    image_passed_ = false;
    return kUpdateOk;
}

/**
 * @brief Forgets any transfer and records the slot the firmware runs from.
 *
 * @param[in] running_slot  slotOf(the firmware's vector table address)
 */
void UpdateReceiver::init(Slot running_slot)
{
    *this = UpdateReceiver{};   /* every member back to its initial value (C: memset to 0) */
    running_ = running_slot;
}

/**
 * @brief Runs a command that accept() accepted.
 *
 * All four UploadId values have their own case (-Wswitch-enum). accept() let only these four
 * through, so a value outside the enum is a corrupted variable: the fault reaction (see the file
 * comment), and nothing runs. Before, a default answered 0x06.
 *
 * @param[in] cmd  the accepted command
 * @return kUpdateOk, or the refusal code of the command's work (kDamaged or the END checks)
 * @note Does not return for an identifier outside UploadId.
 */
AnswerCode UpdateReceiver::run(const UpdateCommand &cmd)
{
    switch (cmd.id) {
    case UploadId::kBegin:
        return runBegin(cmd);
    case UploadId::kChunk:
        return runChunk(cmd);
    case UploadId::kEnd:
        return runEnd();
    case UploadId::kCancel:
        return runCancel();
    }
    faultImpossibleValue("UpdateReceiver::run");   /* only a corrupted value reaches here */
}
