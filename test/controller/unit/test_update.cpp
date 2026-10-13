/*
 * test_update.cpp — host unit tests of the update receiver
 * (src/controller/firmware/update-receiver/update_receiver.cpp) against Interfaces Sec. 4.2
 * (answers, refusal codes), Sec. 5.1 (UPLOAD commands) and REQ-007, with the chip replaced by
 * fake_port.cpp: a RAM copy of the flash in which every region holds its own byte pattern, so a
 * stray erase or write shows.
 *
 * The last group sends the build's real signed images through the receiver into the fake flash;
 * UPLOAD END then runs the bootloader's own checks on them (common/image_check.c, Monocypher).
 *
 * Built with the Mac's clang++, the undefined-behaviour sanitizer and libc++'s debug hardening
 * (Monocypher without them). Run: make -C test/controller unit. Prints PASS or FAIL per test;
 * exit status 1 on a failure.
 * Usage: test_update <dir with firmware-A.img, firmware-B.img> <dir with firmware-B-wrong-key-a.img>
 */
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "boot_slot.hpp"
#include "fake_port.hpp"
#include "frame.hpp"
#include "image_check.h"
#include "image_format.h"
#include "layout.h"
#include "update_receiver.hpp"

namespace {

/* Payload layout (Interfaces Sec. 5.1), written here byte by byte, apart from the parser. */
constexpr uint32_t kChunkHead = 7U;
constexpr uint32_t kPayloadRoom = kChunkHead + kUploadChunkMax + 8U;
/* A length that ends in a partial flash word, for the sequencing tests. */
constexpr uint32_t kTestLength = 1001U;
/* Same code byte as the bench's damaged image (test/controller/Makefile, FLIP_OFFSET). */
constexpr uint32_t kFlipOffset = 0x600U;
constexpr uint8_t kErased = 0xFFU;
constexpr uint32_t kPagesPerSlot = kSlotBytes / kFlashPageBytes;
/* Address sweep for the barrier test: the whole flash and 8 KB either side. */
constexpr uint32_t kSweepMargin = 0x2000U;

using SlotImage = std::array<uint8_t, kSlotBytes>;
using TestData = std::array<uint8_t, kTestLength>;

uint32_t failures;
uint32_t tests;

/**
 * @brief Counts one test result and prints a PASS or FAIL line for it.
 *
 * Increments the test counter, and the failure counter when ok is false.
 *
 * @param[in] ok    true if the check passed
 * @param[in] name  text printed after PASS or FAIL
 */
void report(bool ok, const char *name)
{
    tests++;
    if (!ok) {
        failures++;
    }
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
}

/* ---- Payloads ------------------------------------------------------------------------ */

/**
 * @brief Writes a 16-bit value little-endian.
 *
 * @param[out] p  destination, at least 2 bytes
 * @param[in]  v  value to store
 */
void putU16(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

/**
 * @brief Writes a 32-bit value little-endian.
 *
 * @param[out] p  destination, at least 4 bytes
 * @param[in]  v  value to store
 */
void putU32(uint8_t *p, uint32_t v)
{
    for (uint32_t i = 0U; i < 4U; i++) {
        p[i] = static_cast<uint8_t>(v >> (8U * i));
    }
}

/**
 * @brief Returns the identifier byte of an upload command.
 *
 * The identifier byte of an upload command.
 *
 * @param[in] id  upload command
 * @return the command's identifier byte
 */
constexpr uint8_t idByte(UploadId id)
{
    return static_cast<uint8_t>(id);
}

/* What main.cpp answers: REFUSED with the code from accept(), or RECEIVED and then the
 * result of run() (0x00 = DONE). */
struct Answer {
    bool received;
    AnswerCode code;
};

/**
 * @brief Offers one payload to the receiver and returns what main.cpp would answer.
 *
 * Calls accept(); if that gives kUpdateOk the answer is RECEIVED and run() then gives the
 * result code, otherwise the answer is REFUSED with the code from accept().
 *
 * @param[in,out] rx       receiver under test
 * @param[in]     payload  command payload bytes
 * @param[in]     length   payload length in bytes
 * @return the answer: received is true if RECEIVED came first; code is the final code
 */
Answer send(UpdateReceiver &rx, const uint8_t *payload, uint16_t length)
{
    UpdateCommand cmd{};
    Answer a{false, rx.accept(payload, length, cmd)};

    if (a.code == kUpdateOk) {
        a.received = true;
        a.code = rx.run(cmd);
    }
    return a;
}

/**
 * @brief Sends an UPLOAD BEGIN command.
 *
 * Payload: identifier, image number (2 bytes), image length (4 bytes).
 *
 * @param[in,out] rx      receiver under test
 * @param[in]     number  image number
 * @param[in]     length  announced image length in bytes
 * @return the receiver's answer
 */
Answer begin(UpdateReceiver &rx, uint16_t number, uint32_t length)
{
    std::array<uint8_t, kChunkHead> p{};

    p[0] = idByte(UploadId::kBegin);
    putU16(&p[1], number);
    putU32(&p[3], length);
    return send(rx, p.data(), kChunkHead);
}

/**
 * @brief Sends an UPLOAD CHUNK command carrying n data bytes.
 *
 * Payload: identifier, image number (2 bytes), offset (4 bytes), then the data.
 *
 * @param[in,out] rx      receiver under test
 * @param[in]     number  image number
 * @param[in]     offset  offset of the first data byte in the image
 * @param[in]     data    data bytes to send
 * @param[in]     n       number of data bytes, at most kUploadChunkMax + 8
 * @return the receiver's answer
 */
Answer chunk(UpdateReceiver &rx, uint16_t number, uint32_t offset, const uint8_t *data, uint32_t n)
{
    std::array<uint8_t, kPayloadRoom> p{};

    p[0] = idByte(UploadId::kChunk);
    putU16(&p[1], number);
    putU32(&p[3], offset);
    std::memcpy(&p[kChunkHead], data, n);
    return send(rx, p.data(), static_cast<uint16_t>(kChunkHead + n));
}

/**
 * @brief Sends a 3-byte upload command (END or CANCEL): identifier and image number.
 *
 * @param[in,out] rx      receiver under test
 * @param[in]     id      upload command to send
 * @param[in]     number  image number
 * @return the receiver's answer
 */
Answer shortCommand(UpdateReceiver &rx, UploadId id, uint16_t number)
{
    std::array<uint8_t, 3> p{};

    p[0] = idByte(id);
    putU16(&p[1], number);
    return send(rx, p.data(), 3U);
}

/**
 * @brief Tests whether an answer is RECEIVED followed by DONE.
 *
 * @param[in] a  answer to test
 * @return true if RECEIVED and then code 0x00; false otherwise
 */
bool done(Answer a)
{
    return a.received && (a.code == kUpdateOk);
}

/**
 * @brief Tests that a command was refused on arrival with the given code.
 *
 * Refused on arrival: no RECEIVED, only REFUSED with this code.
 *
 * @param[in] a     answer to test
 * @param[in] code  expected refusal code
 * @return true if no RECEIVED was sent and the code matches; false otherwise
 */
bool refusedOnly(Answer a, AnswerCode code)
{
    return !a.received && (a.code == code);
}

/**
 * @brief Tests that a command was answered RECEIVED and then refused with the given code.
 *
 * RECEIVED, then REFUSED with this code.
 *
 * @param[in] a     answer to test
 * @param[in] code  expected refusal code
 * @return true if RECEIVED was sent and the code matches; false otherwise
 */
bool receivedThenRefused(Answer a, AnswerCode code)
{
    return a.received && (a.code == code);
}

/**
 * @brief Sends a whole image as BEGIN, CHUNKs and END, stopping at the first failed answer.
 *
 * The whole image in 256-byte chunks, stop-and-wait as the onboard computer sends it. The END
 * answer, or the first answer that was not DONE.
 *
 * @param[in,out] rx      receiver under test
 * @param[in]     image   image bytes
 * @param[in]     size    image length in bytes
 * @param[in]     number  image number
 * @return the END answer, or the first answer that was not DONE
 */
Answer upload(UpdateReceiver &rx, const uint8_t *image, uint32_t size, uint16_t number)
{
    Answer a = begin(rx, number, size);

    for (uint32_t offset = 0U; done(a) && (offset < size); offset += kUploadChunkMax) {
        uint32_t n = ((size - offset) < kUploadChunkMax) ? (size - offset) : kUploadChunkMax;
        a = chunk(rx, number, offset, &image[offset], n);
    }
    return done(a) ? shortCommand(rx, UploadId::kEnd, number) : a;
}

/**
 * @brief Tests that the fake flash changed nowhere except in the slot that was written.
 *
 * Every byte outside `written_slot` still holds its region's pattern (fake_port.cpp). Also
 * requires no access outside the flash and no bad read.
 *
 * @param[in] written_slot  slot allowed to differ; kNone if no slot may differ
 * @return true if nothing else changed; false otherwise
 */
bool othersUntouched(Slot written_slot)
{
    uint32_t changed = fakeCountNot(BOOTLOADER_START, BOOTLOADER_BYTES, 0xB0U) +
                       fakeCountNot(LAST_RESORT_START, LAST_RESORT_BYTES, 0x1EU) +
                       fakeCountNot(BOOT_STATE_START, 2U * BOOT_STATE_COPY_BYTES, 0x5EU);
    if (written_slot != Slot::kA) {
        changed += fakeCountNot(kSlotAStart, kSlotBytes, 0xAAU);
    }
    if (written_slot != Slot::kB) {
        changed += fakeCountNot(kSlotBStart, kSlotBytes, 0xBBU);
    }
    return (changed == 0U) && (fake.outside == 0U) && (fake.bad_reads == 0U);
}

/**
 * @brief Fills the test data with a byte pattern whose first word is not erased.
 *
 * Byte i is (i * 7 + 1) modulo 256.
 *
 * @param[out] data  buffer of kTestLength bytes to fill
 */
void fillPattern(TestData &data)
{
    for (uint32_t i = 0U; i < kTestLength; i++) {
        data[i] = static_cast<uint8_t>((i * 7U) + 1U);   /* first word not 0xFFFFFFFF: not "empty" */
    }
}

/**
 * @brief Resets the fake flash and initialises the receiver as running slot A.
 *
 * Running slot A, confirmed A, no trial: the upload target is slot B.
 *
 * @param[in,out] rx  receiver to initialise
 */
void startRunningA(UpdateReceiver &rx)
{
    fakeReset();
    rx.init(Slot::kA);
}

/**
 * @brief Returns a pointer into the fake flash for an absolute flash address.
 *
 * @param[in] address  absolute address, at least kFlashStart and inside the fake flash
 * @return pointer to the byte at that address
 */
const uint8_t *flashAt(uint32_t address)
{
    return &fake.flash[address - kFlashStart];
}

/* ---- Slot choice ----------------------------------------------------------------------- */

/**
 * @brief Checks chooseTarget() and the trial refusal of every upload command.
 *
 * Passes if the target is the slot not running and not confirmed, trial gives 0x07, no free
 * slot or unknown state gives 0x06, and BEGIN, CHUNK, END and CANCEL during a trial are
 * refused 0x07 with the flash untouched.
 */
void testSlotChoice()
{
    boot_state_t s{};
    Slot t = toSlot(0xEEU);

    s.confirmed = BOOT_SLOT_A;
    report((UpdateReceiver::chooseTarget(true, s, Slot::kA, t) == kUpdateOk) && (t == Slot::kB),
           "slot choice: confirmed A, running A -> target B");
    s.confirmed = BOOT_SLOT_B;
    report((UpdateReceiver::chooseTarget(true, s, Slot::kB, t) == kUpdateOk) && (t == Slot::kA),
           "slot choice: confirmed B, running B -> target A");
    s.confirmed = BOOT_SLOT_A;
    s.trial = BOOT_SLOT_B;
    bool trial_other = (UpdateReceiver::chooseTarget(true, s, Slot::kA, t) == AnswerCode::kTrial) &&
                       (t == Slot::kNone);
    bool trial_running = (UpdateReceiver::chooseTarget(true, s, Slot::kB, t) == AnswerCode::kTrial) &&
                         (t == Slot::kNone);
    report(trial_other && trial_running,
           "slot choice: trial set -> refused 0x07, running the trial or the confirmed slot");
    s.trial = BOOT_SLOT_NONE;
    s.confirmed = BOOT_SLOT_B;
    report((UpdateReceiver::chooseTarget(true, s, Slot::kA, t) == AnswerCode::kImpossible) &&
               (t == Slot::kNone),
           "slot choice: running A, confirmed B, no trial -> no slot is free, refused 0x06");
    report((UpdateReceiver::chooseTarget(false, s, Slot::kA, t) == AnswerCode::kImpossible) &&
               (UpdateReceiver::chooseTarget(true, s, Slot::kNone, t) == AnswerCode::kImpossible),
           "slot choice: no valid boot state, or running slot not known -> refused 0x06");

    UpdateReceiver rx;
    startRunningA(rx);
    fake.state.trial = BOOT_SLOT_B;
    uint8_t one = 0x11U;
    const std::array<Answer, 4> a = {
        begin(rx, 1U, kTestLength),
        shortCommand(rx, UploadId::kEnd, 1U),
        shortCommand(rx, UploadId::kCancel, 1U),
        chunk(rx, 1U, 0U, &one, 1U),
    };
    bool all_07 = true;
    for (const Answer &answer : a) {
        all_07 = all_07 && refusedOnly(answer, AnswerCode::kTrial);
    }
    report(all_07 && (fake.erases == 0U) && (fake.programs == 0U) && othersUntouched(Slot::kNone),
           "during a trial BEGIN, CHUNK, END, CANCEL: REFUSED 0x07 only, flash untouched");
}

/**
 * @brief Checks slotOf() on slot edges and addresses outside both slots.
 *
 * Passes if each address maps to slot A, slot B or none as expected.
 */
void testSlotOf()
{
    struct Case {
        uint32_t address;
        Slot slot;
    };
    const Case cases[] = {
        {kFlashStart, Slot::kNone},
        {kSlotAStart - 1U, Slot::kNone},
        {kSlotAStart, Slot::kA},
        {SLOT_A_CODE_START, Slot::kA},
        {kSlotAStart + kSlotBytes - 1U, Slot::kA},
        {kSlotBStart, Slot::kB},
        {SLOT_B_CODE_START, Slot::kB},
        {kSlotBStart + kSlotBytes - 1U, Slot::kB},
        {LAST_RESORT_CODE_START, Slot::kNone},
        {0U, Slot::kNone},
        {0xFFFFFFFFU, Slot::kNone},
    };
    bool ok = true;

    for (const Case &c : cases) {
        ok = ok && (UpdateReceiver::slotOf(c.address) == c.slot);
    }
    report(ok, "running slot from the vector table address: A, B, or none (edges included)");
}

/* ---- The address barrier ------------------------------------------------------------- */

/**
 * @brief Tests whether two address ranges share at least one byte.
 *
 * @param[in] a      first address of range 1
 * @param[in] n      length of range 1 in bytes
 * @param[in] start  first address of range 2
 * @param[in] bytes  length of range 2 in bytes
 * @return true if the ranges overlap; false otherwise
 */
bool overlaps(uint64_t a, uint64_t n, uint64_t start, uint64_t bytes)
{
    return (a < (start + bytes)) && (start < (a + n));
}

/**
 * @brief Computes the answer writeAllowed() must give, independently of its code.
 *
 * What writeAllowed() must answer, worked out in 64 bits so nothing wraps.
 * Allowed only for a real target slot other than the running one, with n above 0 and the
 * range inside the target slot.
 *
 * @param[in] target   slot being written
 * @param[in] running  slot that is executing
 * @param[in] a        first address of the write
 * @param[in] n        length of the write in bytes
 * @return true if the write must be allowed; false otherwise
 */
bool expectedAllowed(Slot target, Slot running, uint64_t a, uint64_t n)
{
    uint64_t start = (target == Slot::kA) ? kSlotAStart : kSlotBStart;
    bool real = (target == Slot::kA) || (target == Slot::kB);

    return real && (target != running) && (n > 0U) && (a >= start) &&
           ((a + n) <= (start + kSlotBytes));
}

/**
 * @brief Tests whether a range touches the bootloader, last-resort, boot state or running slot.
 *
 * @param[in] running  slot that is executing; kNone if unknown
 * @param[in] a        first address of the range
 * @param[in] n        length of the range in bytes
 * @return true if the range overlaps any protected region; false otherwise
 */
bool touchesProtected(Slot running, uint64_t a, uint64_t n)
{
    uint64_t run = (running == Slot::kA) ? kSlotAStart : kSlotBStart;

    return overlaps(a, n, BOOTLOADER_START, BOOTLOADER_BYTES) ||
           overlaps(a, n, LAST_RESORT_START, LAST_RESORT_BYTES) ||
           overlaps(a, n, BOOT_STATE_START, 2U * static_cast<uint64_t>(BOOT_STATE_COPY_BYTES)) ||
           ((running != Slot::kNone) && overlaps(a, n, run, kSlotBytes));
}

/**
 * @brief Sweeps writeAllowed() over targets, running slots, sizes and addresses.
 *
 * Passes if no answer differs from expectedAllowed() and no allowed range touches the
 * bootloader, running slot, last-resort or boot state. Slot codes 3 and 0xFF are no slot: a
 * damaged value must be refused like kNone.
 */
void testAddressBarrier()
{
    /* Slot codes 3 and 0xFF are no slot: a damaged value must be refused like kNone. */
    const Slot targets[] = {Slot::kNone, Slot::kA, Slot::kB, toSlot(3U), toSlot(0xFFU)};
    const Slot runnings[] = {Slot::kNone, Slot::kA, Slot::kB};
    const uint32_t sizes[] = {0U, 1U, 8U, 256U, kFlashPageBytes, kSlotBytes, kSlotBytes + 1U};
    const uint32_t extremes[] = {0U, 8U, 0x7FFFFFFFU, 0xFFFFF800U, 0xFFFFFFF8U, 0xFFFFFFFFU};
    constexpr uint64_t kExtremes = sizeof extremes / sizeof extremes[0];
    const uint64_t first = static_cast<uint64_t>(kFlashStart) - kSweepMargin;
    const uint64_t last = static_cast<uint64_t>(kFlashStart) + kFakeFlashBytes + kSweepMargin;
    uint32_t cases = 0U;
    uint32_t wrong = 0U;
    uint32_t protected_hits = 0U;

    for (Slot target : targets) {
        for (Slot running : runnings) {
            for (uint32_t size : sizes) {
                for (uint64_t a = first; a < last + kExtremes; a++) {
                    uint64_t address = (a < last) ? a : extremes[a - last];
                    bool got = UpdateReceiver::writeAllowed(target, running,
                                                            static_cast<uint32_t>(address), size);
                    cases++;
                    wrong += (got != expectedAllowed(target, running, address, size)) ? 1U : 0U;
                    protected_hits += (got && touchesProtected(running, address, size)) ? 1U : 0U;
                }
            }
        }
    }
    char line[200];
    std::snprintf(line, sizeof line,
                  "address barrier: %u cases (every byte address of the flash +-8 KB), %u wrong; "
                  "%u allowed ranges touch bootloader/running slot/last-resort/boot state",
                  cases, wrong, protected_hits);
    report((wrong == 0U) && (protected_hits == 0U), line);
}

/* ---- Answer codes -------------------------------------------------------------------- */

/**
 * @brief Checks the wire values of the refusal codes and refusalFor() for all 11 image results.
 *
 * Passes if every image check result maps to the expected answer code.
 */
void testResultCodes()
{
    struct Case {
        image_result_t result;
        AnswerCode code;
    };
    const Case cases[] = {
        {IMAGE_OK, kUpdateOk},
        {IMAGE_EMPTY, AnswerCode::kDamaged},
        {IMAGE_BAD_MAGIC, AnswerCode::kDamaged},
        {IMAGE_BAD_FORMAT, AnswerCode::kDamaged},
        {IMAGE_BAD_KIND, AnswerCode::kDamaged},
        {IMAGE_BAD_TARGET, AnswerCode::kOtherController},
        {IMAGE_BAD_LENGTH, AnswerCode::kDamaged},
        {IMAGE_BAD_START, AnswerCode::kOtherSlot},
        {IMAGE_BAD_BODY_CHECK, AnswerCode::kDamaged},
        {IMAGE_BAD_SIGNATURE_A, AnswerCode::kNotOperator},
        {IMAGE_BAD_SIGNATURE_B, AnswerCode::kNotOperator},
    };
    /* The codes on the wire (Interfaces Sec. 4.2 table), checked byte by byte. */
    bool ok = (static_cast<uint8_t>(AnswerCode::kDamaged) == 0x08U) &&
              (static_cast<uint8_t>(AnswerCode::kNotOperator) == 0x09U) &&
              (static_cast<uint8_t>(AnswerCode::kOtherController) == 0x0BU) &&
              (static_cast<uint8_t>(AnswerCode::kOtherSlot) == 0x0EU) &&
              (static_cast<uint8_t>(AnswerCode::kTrial) == 0x07U) &&
              (static_cast<uint8_t>(AnswerCode::kImpossible) == 0x06U);

    for (const Case &c : cases) {
        ok = ok && (UpdateReceiver::refusalFor(c.result) == c.code);
    }
    report(ok, "image check result -> code: passed 0x00, body/structure 0x08, signature 0x09, "
               "target 0x0B, start address 0x0E (all 11 results)");
}

/* Values that are no image_result_t enumerator. The enum's enumerators are 0 to 10; a C++ enum
 * may hold any value up to 15 (the bits its enumerators need), and the undefined-behaviour
 * sanitizer stops the run on a load of a value beyond that, so 11 and 15 are the outside
 * values a test can hand over. */
constexpr int kOutsideA = 11;
constexpr int kOutsideB = 15;

/**
 * @brief Checks refusalFor() one image_result_t value at a time, and a value outside the enum.
 *
 * One report per enumerator, so a failure names the result. The mapping is Interfaces Sec. 4.2:
 * signatures 0x09, target 0x0B, start address 0x0E, damage-type results 0x08. IMAGE_BAD_KIND has
 * no code of its own in Sec. 4.2; 0x08 is the nearest refusal and is what the receiver gave
 * before (open gap, reported to the owner). A value outside the enum refuses the image (0x08).
 */
void testResultCodeEach()
{
    struct Case {
        image_result_t result;
        AnswerCode code;
        const char *name;
    };
    const Case cases[] = {
        {IMAGE_OK, kUpdateOk, "IMAGE_OK -> DONE code 0x00"},
        {IMAGE_EMPTY, AnswerCode::kDamaged, "IMAGE_EMPTY -> 0x08"},
        {IMAGE_BAD_MAGIC, AnswerCode::kDamaged, "IMAGE_BAD_MAGIC -> 0x08"},
        {IMAGE_BAD_FORMAT, AnswerCode::kDamaged, "IMAGE_BAD_FORMAT -> 0x08"},
        {IMAGE_BAD_KIND, AnswerCode::kDamaged, "IMAGE_BAD_KIND -> 0x08 (no code for a wrong kind)"},
        {IMAGE_BAD_TARGET, AnswerCode::kOtherController, "IMAGE_BAD_TARGET -> 0x0B"},
        {IMAGE_BAD_LENGTH, AnswerCode::kDamaged, "IMAGE_BAD_LENGTH -> 0x08"},
        {IMAGE_BAD_START, AnswerCode::kOtherSlot, "IMAGE_BAD_START -> 0x0E"},
        {IMAGE_BAD_BODY_CHECK, AnswerCode::kDamaged, "IMAGE_BAD_BODY_CHECK -> 0x08"},
        {IMAGE_BAD_SIGNATURE_A, AnswerCode::kNotOperator, "IMAGE_BAD_SIGNATURE_A -> 0x09"},
        {IMAGE_BAD_SIGNATURE_B, AnswerCode::kNotOperator, "IMAGE_BAD_SIGNATURE_B -> 0x09"},
    };
    char line[96];

    for (const Case &c : cases) {
        std::snprintf(line, sizeof line, "refusalFor: %s", c.name);
        report(UpdateReceiver::refusalFor(c.result) == c.code, line);
    }
    report((UpdateReceiver::refusalFor(static_cast<image_result_t>(kOutsideA)) == AnswerCode::kDamaged) &&
               (UpdateReceiver::refusalFor(static_cast<image_result_t>(kOutsideB)) == AnswerCode::kDamaged),
           "refusalFor: a value outside image_result_t (11, 15) refuses the image, 0x08");
}

/**
 * @brief Checks image_result_text() for every image_result_t value and a value outside the enum.
 *
 * The text is for the bootloader's log (common/image_check.c). A value outside the enum gives
 * "corrupted result value", never the text of a real result.
 */
void testResultText()
{
    struct Case {
        image_result_t result;
        const char *text;
    };
    const Case cases[] = {
        {IMAGE_OK, "passed"},
        {IMAGE_EMPTY, "empty"},
        {IMAGE_BAD_MAGIC, "proprietary string"},
        {IMAGE_BAD_FORMAT, "format"},
        {IMAGE_BAD_KIND, "kind"},
        {IMAGE_BAD_TARGET, "target"},
        {IMAGE_BAD_LENGTH, "length"},
        {IMAGE_BAD_START, "start address outside the region"},
        {IMAGE_BAD_BODY_CHECK, "body check value"},
        {IMAGE_BAD_SIGNATURE_A, "signature A"},
        {IMAGE_BAD_SIGNATURE_B, "signature B"},
    };
    bool ok = true;

    for (const Case &c : cases) {
        ok = ok && (std::strcmp(image_result_text(c.result), c.text) == 0);
    }
    report(ok, "image_result_text: all 11 results have their own text");
    report((std::strcmp(image_result_text(static_cast<image_result_t>(kOutsideA)), "corrupted result value") == 0) &&
               (std::strcmp(image_result_text(static_cast<image_result_t>(kOutsideB)), "corrupted result value") == 0),
           "image_result_text: a value outside image_result_t (11, 15) gives \"unknown\"");
}

/**
 * @brief Checks otherSlot() (boot_slot.hpp) on A, B, none and values outside the enum.
 *
 * A gives B and B gives A. None, and any code that names no slot, gives none: before, they gave
 * slot A, a real slot, so a damaged code could be taken for the other slot of a pair.
 */
void testOtherSlot()
{
    report((otherSlot(Slot::kA) == Slot::kB) && (otherSlot(Slot::kB) == Slot::kA),
           "otherSlot: A -> B, B -> A");
    report((otherSlot(Slot::kNone) == Slot::kNone) && (otherSlot(toSlot(3U)) == Slot::kNone) &&
               (otherSlot(toSlot(0xEEU)) == Slot::kNone) && (otherSlot(toSlot(0xFFU)) == Slot::kNone),
           "otherSlot: none, 3, 0xEE and 0xFF -> none (not slot A)");
}

/**
 * @brief Checks that run() refuses an identifier outside UploadId and touches nothing.
 *
 * accept() cannot produce such a command (parse takes only 0x01 to 0x04), so this is the second
 * barrier, for a corrupted UpdateCommand. Passes if run() answers 0x06 and no page is erased
 * and nothing is programmed.
 */
void testRunOutsideEnum()
{
    UpdateReceiver rx;
    UpdateCommand cmd{};

    startRunningA(rx);
    cmd.id = static_cast<UploadId>(0x7FU);
    cmd.target = Slot::kB;
    report((rx.run(cmd) == AnswerCode::kImpossible) && (fake.erases == 0U) &&
               (fake.programs == 0U),
           "run: an identifier outside UploadId (0x7F) -> 0x06, flash untouched");
}

/* ---- Payload sizes and length bounds ------------------------------------------------- */

/**
 * @brief Checks that wrong payload sizes are refused 0x06 and isUpload() accepts 0x01 to 0x04.
 *
 * Passes if every bad size gets REFUSED 0x06 only with no erase, and only identifiers 0x01 to
 * 0x04 count as upload commands.
 */
void testPayloadSizes()
{
    UpdateReceiver rx;
    std::array<uint8_t, kPayloadRoom + 2U> p{};
    struct Bad {
        uint8_t id;
        uint16_t length;
    };
    const Bad bad[] = {
        {idByte(UploadId::kBegin), 6U},
        {idByte(UploadId::kBegin), 8U},
        {idByte(UploadId::kChunk), kChunkHead},
        {idByte(UploadId::kChunk), kChunkHead + kUploadChunkMax + 1U},
        {idByte(UploadId::kEnd), 2U},
        {idByte(UploadId::kEnd), 4U},
        {idByte(UploadId::kCancel), 2U},
        {idByte(UploadId::kCancel), 4U},
        {0x05U, 3U},
    };
    bool ok = true;

    startRunningA(rx);
    for (const Bad &b : bad) {
        p[0] = b.id;
        ok = ok && refusedOnly(send(rx, p.data(), b.length), AnswerCode::kImpossible);
    }
    ok = ok && refusedOnly(send(rx, p.data(), 0U), AnswerCode::kImpossible);
    report(ok && (fake.erases == 0U),
           "wrong payload sizes (BEGIN 6/8, CHUNK 0/257 data bytes, END/CANCEL 2/4, empty, "
           "id 0x05): REFUSED 0x06 only");

    struct IdCase {
        uint8_t id;
        bool is_upload;
    };
    const IdCase ids[] = {
        {0x00U, false}, {0x01U, true}, {0x04U, true}, {0x05U, false}, {0x25U, false},
    };
    ok = !UpdateReceiver::isUpload(p.data(), 0U);
    for (const IdCase &c : ids) {
        p[0] = c.id;
        ok = ok && (UpdateReceiver::isUpload(p.data(), 1U) == c.is_upload);
    }
    report(ok, "upload commands are identifiers 0x01 to 0x04 only (Interfaces Sec. 5.1)");
}

/**
 * @brief Checks the BEGIN length limits.
 *
 * Passes if lengths below the smallest image or above the slot size are refused 0x06 with
 * nothing erased, and the two valid extremes are DONE after erasing the slot page by page.
 */
void testLengthBounds()
{
    UpdateReceiver rx;
    constexpr uint32_t kSmallest = IMAGE_HEADER_BYTES + IMAGE_SIGNATURES_BYTES;

    startRunningA(rx);
    bool low = refusedOnly(begin(rx, 1U, 0U), AnswerCode::kImpossible) &&
               refusedOnly(begin(rx, 1U, kSmallest - 1U), AnswerCode::kImpossible);
    bool high = refusedOnly(begin(rx, 1U, kSlotBytes + 1U), AnswerCode::kImpossible) &&
                refusedOnly(begin(rx, 1U, 0xFFFFFFFFU), AnswerCode::kImpossible);
    report(low && high && (fake.erases == 0U),
           "BEGIN length 0, 191, slot size + 1, 2^32 - 1: REFUSED 0x06 only, nothing erased");

    bool fits = done(begin(rx, 1U, kSmallest));
    uint32_t erased_once = fake.erases;
    fits = fits && done(begin(rx, 2U, kSlotBytes));
    report(fits && (erased_once == kPagesPerSlot) && (fake.erases == 2U * kPagesPerSlot) &&
               (fakeCountNot(kSlotBStart, kSlotBytes, kErased) == 0U) &&
               (fake.reloads >= fake.erases) && othersUntouched(Slot::kB),
           "BEGIN length 192 and 40960: DONE after all 20 pages of slot B are erased, watchdog "
           "reloaded after each page, nothing else touched");
}

/* ---- Offset sequencing --------------------------------------------------------------- */

/**
 * @brief Checks CHUNK, END and CANCEL sequencing through one transfer.
 *
 * Passes if commands out of order, at a wrong offset or number, or of a wrong size are
 * refused 0x06, the data lands in slot B in order, END on non-image bytes gives RECEIVED then
 * 0x08, and CANCEL erases the first page of slot B.
 */
void testOffsets()
{
    UpdateReceiver rx;
    static TestData data;
    uint8_t one = 0x11U;

    fillPattern(data);
    startRunningA(rx);
    report(refusedOnly(chunk(rx, 1U, 0U, data.data(), kUploadChunkMax), AnswerCode::kImpossible) &&
               refusedOnly(shortCommand(rx, UploadId::kEnd, 1U), AnswerCode::kImpossible) &&
               refusedOnly(shortCommand(rx, UploadId::kCancel, 1U), AnswerCode::kImpossible),
           "CHUNK, END, CANCEL before any BEGIN: REFUSED 0x06 only");

    bool ok = done(begin(rx, 1U, kTestLength));
    ok = ok && refusedOnly(chunk(rx, 1U, 8U, data.data(), kUploadChunkMax), AnswerCode::kImpossible);
    ok = ok && done(chunk(rx, 1U, 0U, data.data(), kUploadChunkMax));
    report(ok, "first CHUNK must start at offset 0; offset 0 then DONE");

    report(refusedOnly(chunk(rx, 1U, 0U, data.data(), kUploadChunkMax), AnswerCode::kImpossible) &&
               refusedOnly(chunk(rx, 1U, 512U, &data[512], kUploadChunkMax), AnswerCode::kImpossible),
           "CHUNK repeating offset 0, or skipping to 512 (expected 256): REFUSED 0x06 only");
    report(refusedOnly(chunk(rx, 1U, 256U, &data[256], 100U), AnswerCode::kImpossible),
           "CHUNK of 100 bytes that is not the last: REFUSED 0x06 (whole flash words only)");
    report(refusedOnly(chunk(rx, 2U, 256U, &data[256], kUploadChunkMax), AnswerCode::kImpossible),
           "CHUNK with another image number: REFUSED 0x06 only");
    report(refusedOnly(shortCommand(rx, UploadId::kEnd, 1U), AnswerCode::kImpossible),
           "END before the last byte: REFUSED 0x06 only");

    ok = done(chunk(rx, 1U, 256U, &data[256], kUploadChunkMax)) &&
         done(chunk(rx, 1U, 512U, &data[512], kUploadChunkMax));
    ok = ok && refusedOnly(chunk(rx, 1U, 768U, &data[768], 240U), AnswerCode::kImpossible);
    ok = ok && done(chunk(rx, 1U, 768U, &data[768], kTestLength - 768U));
    ok = ok && refusedOnly(chunk(rx, 1U, kTestLength, &one, 1U), AnswerCode::kImpossible);
    report(ok, "CHUNK past the announced length refused 0x06; last partial chunk (233 bytes) "
               "DONE; a CHUNK after the last byte refused 0x06");

    const uint8_t *slot = flashAt(kSlotBStart);
    report((std::memcmp(slot, data.data(), kTestLength) == 0) &&
               (fakeCountNot(kSlotBStart + kTestLength, kSlotBytes - kTestLength, kErased) == 0U) &&
               othersUntouched(Slot::kB),
           "slot B holds the 1001 bytes in order, the rest of the slot erased (last flash word "
           "padded with 0xFF), nothing else touched");

    report(receivedThenRefused(shortCommand(rx, UploadId::kEnd, 1U), AnswerCode::kDamaged) &&
               !rx.imagePassed(),
           "END on bytes that are not an image: RECEIVED, then REFUSED 0x08");
    report(refusedOnly(chunk(rx, 1U, kTestLength, &one, 1U), AnswerCode::kImpossible) &&
               refusedOnly(shortCommand(rx, UploadId::kEnd, 1U), AnswerCode::kImpossible),
           "after END: CHUNK and END refused 0x06 (transfer closed)");

    ok = refusedOnly(shortCommand(rx, UploadId::kCancel, 9U), AnswerCode::kImpossible);
    ok = ok && done(shortCommand(rx, UploadId::kCancel, 1U));
    ok = ok && (fakeCountNot(kSlotBStart, kFlashPageBytes, kErased) == 0U);
    ok = ok && refusedOnly(shortCommand(rx, UploadId::kCancel, 1U), AnswerCode::kImpossible);
    uint32_t signed_bytes = 0U;
    ok = ok && (image_check_contents(slot, kSlotBStart, kSlotBytes, IMAGE_KIND_MAIN,
                                     &signed_bytes) == IMAGE_EMPTY);
    report(ok && othersUntouched(Slot::kB),
           "CANCEL with another number refused 0x06; CANCEL DONE erases slot B's first page "
           "(bootloader check: empty); a second CANCEL refused 0x06");
}

/**
 * @brief Checks that BEGIN during an open transfer starts it over.
 *
 * Passes if the slot is erased again, the old image number is refused and the new one starts
 * at offset 0.
 */
void testBeginRestarts()
{
    UpdateReceiver rx;
    static TestData data;

    fillPattern(data);
    startRunningA(rx);
    bool ok = done(begin(rx, 1U, kTestLength)) &&
              done(chunk(rx, 1U, 0U, data.data(), kUploadChunkMax));
    ok = ok && done(begin(rx, 2U, kTestLength));
    ok = ok && (fakeCountNot(kSlotBStart, kSlotBytes, kErased) == 0U);
    ok = ok && refusedOnly(chunk(rx, 1U, 256U, &data[256], kUploadChunkMax), AnswerCode::kImpossible);
    ok = ok && done(chunk(rx, 2U, 0U, data.data(), kUploadChunkMax));
    report(ok, "BEGIN during an open transfer restarts it: slot erased again, old image number "
               "refused, new one starts at offset 0");
}

/**
 * @brief Checks the receiver notices a boot state change after BEGIN.
 *
 * Passes if a trial set after BEGIN gives CHUNK REFUSED 0x07 with nothing written and the
 * transfer continues once the trial is cleared, and a changed confirmed slot gives 0x06.
 */
void testTrialMidTransfer()
{
    UpdateReceiver rx;
    static TestData data;

    fillPattern(data);
    startRunningA(rx);
    bool ok = done(begin(rx, 1U, kTestLength));
    uint32_t programs = fake.programs;
    fake.state.trial = BOOT_SLOT_B;   /* e.g. SWITCH ran (step 2d) */
    ok = ok && refusedOnly(chunk(rx, 1U, 0U, data.data(), kUploadChunkMax), AnswerCode::kTrial);
    ok = ok && (fake.programs == programs);
    fake.state.trial = BOOT_SLOT_NONE;
    ok = ok && done(chunk(rx, 1U, 0U, data.data(), kUploadChunkMax));
    report(ok, "trial set after BEGIN: next CHUNK REFUSED 0x07, nothing written; trial cleared: "
               "the transfer continues");

    startRunningA(rx);
    ok = done(begin(rx, 1U, kTestLength));
    fake.state.confirmed = BOOT_SLOT_B;   /* boot state changed under the transfer */
    ok = ok && refusedOnly(chunk(rx, 1U, 0U, data.data(), kUploadChunkMax), AnswerCode::kImpossible);
    report(ok && (fake.programs == 0U), "confirmed slot changed after BEGIN: CHUNK REFUSED 0x06, "
                                        "nothing written");
}

/**
 * @brief Checks the receiver's answers to erase, program and ECC read faults.
 *
 * Passes if each fault gives RECEIVED then REFUSED 0x08 (the ECC case REFUSED 0x08), the
 * transfer is closed afterwards, and CANCEL can be repeated.
 */
void testFlashFaults()
{
    UpdateReceiver rx;
    static TestData data;

    fillPattern(data);
    startRunningA(rx);
    fake.fail_erase = true;
    bool ok = receivedThenRefused(begin(rx, 1U, kTestLength), AnswerCode::kDamaged);
    ok = ok && refusedOnly(chunk(rx, 1U, 0U, data.data(), kUploadChunkMax), AnswerCode::kImpossible);
    ok = ok && receivedThenRefused(shortCommand(rx, UploadId::kCancel, 1U), AnswerCode::kDamaged);
    fake.fail_erase = false;
    ok = ok && done(shortCommand(rx, UploadId::kCancel, 1U));
    report(ok, "erase fault: BEGIN RECEIVED then REFUSED 0x08; transfer closed; CANCEL repeatable");

    startRunningA(rx);
    ok = done(begin(rx, 1U, kTestLength));
    fake.fail_program = true;
    ok = ok && receivedThenRefused(chunk(rx, 1U, 0U, data.data(), kUploadChunkMax),
                                   AnswerCode::kDamaged);
    fake.fail_program = false;
    ok = ok && refusedOnly(chunk(rx, 1U, 0U, data.data(), kUploadChunkMax), AnswerCode::kImpossible);
    report(ok, "write fault: CHUNK RECEIVED then REFUSED 0x08; transfer closed (BEGIN again)");

    startRunningA(rx);
    ok = done(begin(rx, 1U, kTestLength));
    fake.ecc_error = true;
    ok = ok && receivedThenRefused(chunk(rx, 1U, 0U, data.data(), kUploadChunkMax),
                                   AnswerCode::kDamaged);
    report(ok, "read-back hits a damaged flash word (ECC): CHUNK REFUSED 0x08");
}

/* ---- Real images ---------------------------------------------------------------------- */

/**
 * @brief Reads an image file into a buffer.
 *
 * Prints a message if nothing could be read.
 *
 * @param[in]  dir   directory holding the file
 * @param[in]  name  file name
 * @param[out] buf   buffer receiving the bytes, at most one slot
 * @return number of bytes read; 0 if the file could not be read
 */
uint32_t load(const char *dir, const char *name, SlotImage &buf)
{
    char path[1024];
    size_t n = 0U;

    std::snprintf(path, sizeof path, "%s/%s", dir, name);
    std::FILE *f = std::fopen(path, "rb");
    if (f != nullptr) {
        n = std::fread(buf.data(), 1U, buf.size(), f);
        (void)std::fclose(f);
    }
    if (n == 0U) {
        std::printf("cannot read %s\n", path);
    }
    return static_cast<uint32_t>(n);
}

/**
 * @brief Sends the build's real signed images through the receiver into the fake flash.
 *
 * Passes if a good image is DONE and equals the slot, and a damaged body, an unknown key, a
 * wrong controller identity, a wrong start address and a flash read error during END are
 * refused with 0x08, 0x09, 0x0B, 0x0E and 0x08, with the rest of the flash untouched.
 *
 * @param[in] src_images   directory with firmware-A.img and firmware-B.img
 * @param[in] test_images  directory with firmware-B-wrong-key-a.img
 */
void testImages(const char *src_images, const char *test_images)
{
    static SlotImage a;
    static SlotImage b;
    static SlotImage bad_key;
    static SlotImage changed;
    uint32_t a_size = load(src_images, "firmware-A.img", a);
    uint32_t b_size = load(src_images, "firmware-B.img", b);
    uint32_t key_size = load(test_images, "firmware-B-wrong-key-a.img", bad_key);
    UpdateReceiver rx;

    if ((a_size == 0U) || (b_size == 0U) || (key_size == 0U)) {
        report(false, "real images: could not read the image files");
        return;
    }

    startRunningA(rx);
    Answer end = upload(rx, b.data(), b_size, 1U);
    report(done(end) && rx.imagePassed() &&
               (std::memcmp(flashAt(kSlotBStart), b.data(), b_size) == 0) &&
               othersUntouched(Slot::kB),
           "firmware-B.img into slot B (running A): END DONE 0x00, slot equals the image, "
           "nothing else touched");

    changed = b;
    changed[kFlipOffset] = static_cast<uint8_t>(changed[kFlipOffset] ^ 0xFFU);
    report(receivedThenRefused(upload(rx, changed.data(), b_size, 2U), AnswerCode::kDamaged),
           "firmware-B.img with byte 0x600 inverted: END RECEIVED, then REFUSED 0x08");

    report(receivedThenRefused(upload(rx, bad_key.data(), key_size, 3U), AnswerCode::kNotOperator),
           "firmware-B signed with an unknown key A: END REFUSED 0x09");

    changed = b;
    changed[IMAGE_OFFSET_TARGET] = 0x32U;   /* controller identity 0x0432, not 0x0431 */
    report(receivedThenRefused(upload(rx, changed.data(), b_size, 4U), AnswerCode::kOtherController),
           "firmware-B with another controller identity in the header: END REFUSED 0x0B");

    report(receivedThenRefused(upload(rx, a.data(), a_size, 5U), AnswerCode::kOtherSlot),
           "firmware-A.img (linked for slot A) into slot B: END REFUSED 0x0E");

    report(othersUntouched(Slot::kB),
           "after these five uploads: bootloader, slot A, last-resort and boot state untouched");

    startRunningA(rx);
    Answer a_ok = begin(rx, 8U, b_size);
    for (uint32_t off = 0U; done(a_ok) && (off < b_size); off += kUploadChunkMax) {
        uint32_t n = ((b_size - off) < kUploadChunkMax) ? (b_size - off) : kUploadChunkMax;
        a_ok = chunk(rx, 8U, off, &b[off], n);
    }
    fake.ecc_error = true;   /* a damaged word read during END's checks */
    report(done(a_ok) &&
               receivedThenRefused(shortCommand(rx, UploadId::kEnd, 8U), AnswerCode::kDamaged),
           "good image, but a flash read error during END's checks: REFUSED 0x08");
    fake.ecc_error = false;

    fakeReset();
    fake.state.confirmed = BOOT_SLOT_B;
    fake.allowed_slot = Slot::kA;
    rx.init(Slot::kB);
    end = upload(rx, a.data(), a_size, 9U);
    report(done(end) && (std::memcmp(flashAt(kSlotAStart), a.data(), a_size) == 0) &&
               othersUntouched(Slot::kA),
           "running B, confirmed B: firmware-A.img into slot A, END DONE 0x00, slot B untouched");
}

}  // namespace

/**
 * @brief Runs all update receiver unit tests.
 *
 * @param[in] argc  argument count; must be 3
 * @param[in] argv  program name, directory with the firmware images, directory with the
 *                  wrong-key image
 * @return 0 if all tests passed; 1 if a test failed; 2 on a usage error
 */
int main(int argc, char **argv)
{
    if (argc != 3) {
        std::printf("usage: %s <src image dir> <test image dir>\n", argv[0]);
        return 2;
    }
    testSlotChoice();
    testSlotOf();
    testAddressBarrier();
    testResultCodes();
    testResultCodeEach();
    testResultText();
    testOtherSlot();
    testRunOutsideEnum();
    testPayloadSizes();
    testLengthBounds();
    testOffsets();
    testBeginRestarts();
    testTrialMidTransfer();
    testFlashFaults();
    testImages(argv[1], argv[2]);
    if (failures == 0U) {
        std::printf("update receiver unit tests: all %u passed\n", tests);
        return 0;
    }
    std::printf("update receiver unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
