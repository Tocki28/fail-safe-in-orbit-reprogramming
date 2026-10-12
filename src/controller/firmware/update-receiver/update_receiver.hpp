/*
 * update_receiver.hpp — the update receiver (Architecture Sec. 6.0): takes UPLOAD BEGIN, CHUNK,
 * END and CANCEL from the onboard computer and writes the image into the controller's slot
 * that is NOT confirmed, never the one it runs from (REQ-007, Interfaces Sec. 5.1).
 *
 * Commands (Interfaces Sec. 5.1, command table; multi-byte fields little-endian, Sec. 4.0):
 *   0x01 UPLOAD BEGIN   image number (2), image length (4)
 *   0x02 UPLOAD CHUNK   image number (2), offset (4), data (1 to 256)
 *   0x03 UPLOAD END     image number (2)
 *   0x04 UPLOAD CANCEL  image number (2)
 *
 * STOP-AND-WAIT: the sender sends the next frame only after the DONE (or REFUSED) of the
 * previous one. While the flash is erased or written, every read of the flash stalls
 * [RM0440 Rev 9, Sec. 5.3.5, p. 195]; the interrupt vectors and handlers are in flash, so the
 * receive interrupt cannot run and bytes that arrive meanwhile are lost (USART overrun). A
 * page erase stalls up to 24.47 ms [DS12589 Rev 6, Table 46, p. 107]; UPLOAD BEGIN erases 20.
 *
 * Each command is handled in two calls, matching its two answers (REQ-033, REQ-034):
 *   accept()  the checks on arrival; a refusal here is answered REFUSED only;
 *   run()     the work, after RECEIVED; its result is answered DONE 0x00 or REFUSED.
 *
 * C++: class UpdateReceiver (C: update_receiver_t and the update_ functions). The transfer's
 * state is private: only accept(), run() and init() change it (C: any code holding the struct
 * could). The rules that need no state are static member functions, called as
 * UpdateReceiver::slotOf() and so on, and unit-tested on their own as before.
 */
#ifndef UPDATE_RECEIVER_HPP
#define UPDATE_RECEIVER_HPP

#include <cstdint>

#include "boot_slot.hpp"
#include "boot_state.h"
#include "frame.hpp"
#include "image_check.h"

/* Upload command identifiers (Interfaces Sec. 5.1, command table). */
enum class UploadId : uint8_t {
    kBegin = 0x01U,
    kChunk = 0x02U,
    kEnd = 0x03U,
    kCancel = 0x04U,
};

/* Most data bytes in one UPLOAD CHUNK (Interfaces Sec. 5.1). */
constexpr uint32_t kUploadChunkMax = 256U;

/* Accepted, or done with success: the DONE code 0x00 (Interfaces Sec. 4.2). Any other result
 * is a refusal code of the Interfaces Sec. 4.2 table (frame.hpp, AnswerCode). */
constexpr AnswerCode kUpdateOk = AnswerCode::kSuccess;

enum class UpdatePhase : uint8_t {
    kIdle,        /* no transfer since start, or the last one was cancelled */
    kReceiving,   /* BEGIN done; chunks expected */
    kClosed,      /* END has run (imagePassed() says how), or a flash operation failed */
};

/* One upload command, read from the payload by accept(). */
struct UpdateCommand {
    UploadId id;
    uint16_t image_number;
    uint32_t value;           /* BEGIN: image length; CHUNK: offset; END, CANCEL: 0 */
    const uint8_t *data;      /* CHUNK: its data, inside the frame payload */
    uint32_t data_bytes;      /* CHUNK: 1 to 256 */
    Slot target;              /* the slot the command acts on */
};

class UpdateReceiver {
public:
    /* running_slot: slotOf(the firmware's vector table address). Forgets any transfer. */
    void init(Slot running_slot);

    /* True if the payload's identifier is one of the four upload commands. */
    [[nodiscard]] static bool isUpload(const uint8_t *payload, uint16_t length);

    /* Checks on arrival. kUpdateOk, with cmd filled in, or the refusal code. */
    AnswerCode accept(const uint8_t *payload, uint16_t length, UpdateCommand &cmd) const;

    /* Run a command that accept() accepted. kUpdateOk or the refusal code. */
    AnswerCode run(const UpdateCommand &cmd);

    /* The slot this firmware runs from, as given to init(). */
    Slot runningSlot() const
    {
        return running_;
    }

    /* After END (phase kClosed): its checks passed (for SWITCH, step 2d). */
    bool imagePassed() const
    {
        return image_passed_;
    }

    /* ---- The rules, as static functions (unit-tested on the host) ---------------------- */

    /* Slot::kA or kB if the address lies in that slot, otherwise Slot::kNone. */
    static Slot slotOf(uint32_t address);

    /* First byte of slot A or B; 0 for any other value. */
    static uint32_t slotStart(Slot slot);

    /* The slot an upload may write: not confirmed, not running. kUpdateOk and target set, or
     * the refusal code with target = Slot::kNone. */
    static AnswerCode chooseTarget(bool state_valid, const boot_state_t &state, Slot running,
                                   Slot &target);

    /* The second barrier, asked before every erase and write: true only if [address,
     * address + bytes) lies inside slot `target`, and `target` is slot A or B and not
     * `running`. */
    [[nodiscard]] static bool writeAllowed(Slot target, Slot running, uint32_t address,
                                           uint32_t bytes);

    /* The answer code for a result of the image checks (common/image_check.h). */
    static AnswerCode refusalFor(image_result_t result);

private:
    bool sameTransfer(const UpdateCommand &cmd) const;
    bool chunkFits(const UpdateCommand &cmd) const;
    bool valuesFit(const UpdateCommand &cmd) const;
    bool erasePage(uint32_t page) const;
    bool writeChunk(uint32_t address, const UpdateCommand &cmd) const;
    AnswerCode runBegin(const UpdateCommand &cmd);
    AnswerCode runChunk(const UpdateCommand &cmd);
    AnswerCode runEnd();
    AnswerCode runCancel();

    Slot running_ = Slot::kNone;          /* Slot::kA or kB: the slot this firmware runs from */
    UpdatePhase phase_ = UpdatePhase::kIdle;
    uint16_t image_number_ = 0U;          /* from the last UPLOAD BEGIN */
    Slot target_ = Slot::kNone;           /* the slot that transfer writes */
    uint32_t length_ = 0U;                /* image length from that UPLOAD BEGIN */
    uint32_t written_ = 0U;               /* bytes written so far, from the slot's first byte */
    bool image_passed_ = false;           /* kClosed only: END's checks passed (SWITCH, step 2d) */
};

#endif /* UPDATE_RECEIVER_HPP */
