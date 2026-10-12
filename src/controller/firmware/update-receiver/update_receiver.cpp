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
 */
#include "update_receiver.hpp"

#include <array>
#include <cstring>

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

uint16_t readU16(const uint8_t *bytes)
{
    return static_cast<uint16_t>(static_cast<uint32_t>(bytes[0]) |
                                 (static_cast<uint32_t>(bytes[1]) << 8));
}

/* ---- Checks on arrival that need no state ------------------------------------------- */

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
    default:
        return false;
    }
}

/* The payload's fields; false if its size does not fit the command. */
bool parse(const uint8_t *payload, uint16_t length, UpdateCommand &cmd)
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

/* The bootloader's own checks, in its order (bootloader/main.c, image_passes): contents, then
 * both signatures, then any flash read error seen on the way. */
AnswerCode checkSlot(Slot slot)
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

}  // namespace

/* ---- The rules ---------------------------------------------------------------------- */

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

uint32_t UpdateReceiver::slotStart(Slot slot)
{
    if (slot == Slot::kA) {
        return kSlotAStart;
    }
    if (slot == Slot::kB) {
        return kSlotBStart;
    }
    return 0U;
}

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
    target = (running == Slot::kA) ? Slot::kB : Slot::kA;
    return kUpdateOk;
}

bool UpdateReceiver::writeAllowed(Slot target, Slot running, uint32_t address, uint32_t bytes)
{
    uint32_t start = slotStart(target);

    if ((start == 0U) || (target == running) || (bytes == 0U) || (bytes > kSlotBytes)) {
        return false;
    }
    /* As offsets from the slot start, so no sum can wrap past 2^32. */
    return (address >= start) && ((address - start) <= (kSlotBytes - bytes));
}

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
    default:
        /* Body check value, and every other fault of the image's structure: empty, proprietary
         * string, format, kind, length. */
        return AnswerCode::kDamaged;
    }
}

/* ---- Checks on arrival -------------------------------------------------------------- */

bool UpdateReceiver::isUpload(const uint8_t *payload, uint16_t length)
{
    return (length >= kIdBytes) && (payload[0] >= static_cast<uint8_t>(UploadId::kBegin)) &&
           (payload[0] <= static_cast<uint8_t>(UploadId::kCancel));
}

/* The command names the transfer that BEGIN opened, into the same slot. */
bool UpdateReceiver::sameTransfer(const UpdateCommand &cmd) const
{
    return (phase_ != UpdatePhase::kIdle) && (cmd.image_number == image_number_) &&
           (cmd.target == target_);
}

/* A chunk starts at the next byte not yet written and stays inside the announced length.
 * Every chunk but the last is a whole number of flash words, so the next chunk starts on an
 * unwritten flash word: a written one cannot be written again [RM0440 Rev 9, Sec. 5.3.7,
 * p. 196]. */
bool UpdateReceiver::chunkFits(const UpdateCommand &cmd) const
{
    uint32_t room = length_ - written_;   /* written_ never exceeds length_ */
    bool last = (cmd.data_bytes == room);

    return (cmd.value == written_) && (cmd.data_bytes <= room) &&
           (last || ((cmd.data_bytes % kFlashWordBytes) == 0U));
}

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
    default:
        return false;
    }
}

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

bool UpdateReceiver::erasePage(uint32_t page) const
{
    return writeAllowed(target_, running_, page, kFlashPageBytes) && UpdatePort::erasePage(page);
}

/* A BEGIN while a transfer is open restarts it. The onboard computer is the only sender and
 * runs one transfer at a time, so a new BEGIN means it started over (after its own restart, or
 * for a new upload from the ground). Refusing it would block the slot until a CANCEL the
 * sender may not know it owes. Erasing the whole slot removes every byte of the old transfer. */
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

/* Program the chunk at `address`, then read it back through the ECC check. Byte i of the data
 * lands at address + i: the chip stores words little-endian [RM0440 Rev 9, Sec. 2.2.1, p. 82]. */
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

AnswerCode UpdateReceiver::runChunk(const UpdateCommand &cmd)
{
    if (!writeChunk(slotStart(target_) + cmd.value, cmd)) {
        phase_ = UpdatePhase::kClosed;   /* a half-written flash word cannot be written again */
        return AnswerCode::kDamaged;
    }
    written_ += cmd.data_bytes;
    return kUpdateOk;
}

AnswerCode UpdateReceiver::runEnd()
{
    AnswerCode result = checkSlot(target_);

    phase_ = UpdatePhase::kClosed;
    image_passed_ = (result == kUpdateOk);
    return result;
}

/* The header is in the slot's first page. Erased, the slot reads as empty to the bootloader
 * (common/image_check.c, IMAGE_EMPTY), which never starts it. On a failed erase the transfer
 * stays as it was, so CANCEL can be sent again. */
AnswerCode UpdateReceiver::runCancel()
{
    if (!erasePage(slotStart(target_))) {
        return AnswerCode::kDamaged;
    }
    phase_ = UpdatePhase::kIdle;
    image_passed_ = false;
    return kUpdateOk;
}

void UpdateReceiver::init(Slot running_slot)
{
    *this = UpdateReceiver{};   /* every member back to its initial value (C: memset to 0) */
    running_ = running_slot;
}

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
    default:
        return AnswerCode::kImpossible;   /* not reached: accept() takes only these four */
    }
}
