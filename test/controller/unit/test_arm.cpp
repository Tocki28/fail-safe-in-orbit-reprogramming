/*
 * test_arm.cpp — host unit tests of the arm state
 * (src/controller/firmware/command-checker/arm_state.cpp) against Interfaces Sec. 5.1 (ARM
 * payload), Sec. 5.2 (arming), Sec. 4.2 (codes 0x01, 0x02), REQ-029, REQ-030 and REQ-137 (no
 * single bit error makes it armed).
 *
 * The REQ-137 test flips every bit of the stored object, one at a time, from each state the
 * object can be in, and asks the arming check whether it is armed.
 *
 * Run: make -C test/controller unit. Prints PASS or FAIL per test; exit status 1 on a failure.
 */
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "arm_state.hpp"
#include "frame.hpp"

namespace {

constexpr uint8_t kSwitch = 0x20U;
constexpr uint8_t kConfirm = 0x26U;
constexpr uint32_t kSeq = 7U;
constexpr uint32_t kArmedAt = 1000U;
constexpr uint32_t kBitsPerByte = 8U;

using Raw = std::array<uint8_t, sizeof(ArmState)>;

static_assert(std::is_trivially_copyable<ArmState>::value,
              "the bit-flip test copies the object's bytes");

uint32_t failures;
uint32_t tests;

/**
 * @brief Counts and prints one test result.
 *
 * @param[in]     ok    true if the test passed
 * @param[in]     name  what was tested
 */
void report(bool ok, const char *name)
{
    tests++;
    if (!ok) {
        failures++;
    }
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
}

/**
 * @brief An ARM payload, written byte by byte from the Interfaces Sec. 5.1 command table.
 *
 * @param[in]     command   identifier named
 * @param[in]     sequence  sequence number named, little-endian
 * @return the 6 bytes
 */
std::array<uint8_t, 6> armPayload(uint8_t command, uint32_t sequence)
{
    return {0x0DU, command, static_cast<uint8_t>(sequence), static_cast<uint8_t>(sequence >> 8),
            static_cast<uint8_t>(sequence >> 16), static_cast<uint8_t>(sequence >> 24)};
}

/**
 * @brief An arm state armed for SWITCH, sequence kSeq, at kArmedAt.
 *
 * @return the state
 */
ArmState armedForSwitch()
{
    ArmState arm;

    arm.arm(ArmRequest{kSwitch, kSeq}, kArmedAt);
    return arm;
}

/**
 * @brief Tests the ARM payload reader.
 */
void testParse()
{
    ArmRequest r{};
    std::array<uint8_t, 6> p = armPayload(kConfirm, 0x0A0B0C0DU);

    report(ArmState::isArm(p.data(), 6U) && ArmState::parse(p.data(), 6U, r) &&
               (r.command == kConfirm) && (r.sequence == 0x0A0B0C0DU),
           "ARM payload: identifier 0x0D, command 0x26, sequence 0x0A0B0C0D little-endian");

    std::array<uint8_t, 8> longer{};
    std::memcpy(longer.data(), p.data(), p.size());
    report(!ArmState::parse(p.data(), 5U, r) && !ArmState::parse(longer.data(), 7U, r) &&
               ArmState::isArm(p.data(), 5U),
           "ARM of 5 or 7 bytes: isArm true (refused as an ARM), parse false (REFUSED 0x06)");

    p[0] = 0x0EU;
    report(!ArmState::isArm(p.data(), 6U) && !ArmState::parse(p.data(), 6U, r) &&
               !ArmState::isArm(p.data(), 0U),
           "another identifier, or an empty payload, is not an ARM");

    report((static_cast<uint8_t>(AnswerCode::kNotArmed) == 0x01U) &&
               (static_cast<uint8_t>(AnswerCode::kArmExpired) == 0x02U) &&
               (static_cast<uint8_t>(AnswerCode::kMode) == 0x05U) &&
               (static_cast<uint8_t>(AnswerCode::kTestsNotPassed) == 0x0FU),
           "codes on the wire (Interfaces Sec. 4.2): 0x01 no valid ARM, 0x02 expired, 0x05 mode, "
           "0x0F trial tests");
}

/**
 * @brief Tests the arming check: no ARM, the named command and sequence number, expiry.
 */
void testCheck()
{
    ArmState fresh;
    report(fresh.check(kSwitch, kSeq, 0U) == AnswerCode::kNotArmed,
           "no ARM since start: SWITCH refused 0x01");

    ArmState arm = armedForSwitch();
    report(arm.check(kSwitch, kSeq, kArmedAt) == AnswerCode::kSuccess,
           "ARM naming SWITCH, sequence 7: SWITCH with sequence 7 passes");
    report((arm.check(kConfirm, kSeq, kArmedAt) == AnswerCode::kNotArmed) &&
               (arm.check(kSwitch, kSeq + 1U, kArmedAt) == AnswerCode::kNotArmed) &&
               (arm.check(kSwitch, kSeq - 1U, kArmedAt) == AnswerCode::kNotArmed),
           "ARM for SWITCH 7: CONFIRM 7, SWITCH 8 and SWITCH 6 refused 0x01");

    report((arm.check(kSwitch, kSeq, kArmedAt + kArmExpiryMs - 1U) == AnswerCode::kSuccess) &&
               (arm.check(kSwitch, kSeq, kArmedAt + kArmExpiryMs) == AnswerCode::kArmExpired),
           "expiry 30 s: 29 999 ms after the ARM passes, 30 000 ms refused 0x02");
    report(arm.check(kConfirm, kSeq, kArmedAt + kArmExpiryMs) == AnswerCode::kNotArmed,
           "expired ARM naming another command: 0x01 (command checked before expiry)");

    ArmState wrap;
    wrap.arm(ArmRequest{kSwitch, kSeq}, 0xFFFFF000U);
    report(wrap.check(kSwitch, kSeq, 0x00000F00U) == AnswerCode::kSuccess,
           "ARM 4 096 ms before the millisecond count wraps, checked 3 840 ms after: passes");

    arm.disarm();
    report(arm.check(kSwitch, kSeq, kArmedAt) == AnswerCode::kNotArmed,
           "after disarm (ARM used up): refused 0x01");

    arm = armedForSwitch();
    arm.arm(ArmRequest{kConfirm, 9U}, kArmedAt);
    report((arm.check(kSwitch, kSeq, kArmedAt) == AnswerCode::kNotArmed) &&
               (arm.check(kConfirm, 9U, kArmedAt) == AnswerCode::kSuccess),
           "a new ARM replaces the old one: old SWITCH 7 refused 0x01, new CONFIRM 9 passes");
}

/**
 * @brief Tests that the expiry mark keeps an old ARM expired after the millisecond count wraps.
 */
void testExpireWhenDue()
{
    ArmState arm = armedForSwitch();

    arm.expireWhenDue(kArmedAt + kArmExpiryMs - 1U);
    bool still = arm.check(kSwitch, kSeq, kArmedAt + kArmExpiryMs - 1U) == AnswerCode::kSuccess;
    arm.expireWhenDue(kArmedAt + kArmExpiryMs);
    /* 2^32 ms after the ARM, now - armed_ms is 0 again: without the mark it would pass. */
    bool wrapped = arm.check(kSwitch, kSeq, kArmedAt) == AnswerCode::kArmExpired;
    report(still && wrapped,
           "expireWhenDue: not before 30 s; after, the ARM stays expired (0x02) even when the "
           "millisecond count has wrapped to the ARM's own time");
}

/**
 * @brief Flips every bit of `start`, one at a time, and counts the copies the check finds armed.
 *
 * Each flipped copy is also passed through expireWhenDue, which must not turn it armed either.
 *
 * @param[in]     start  the state to damage
 * @return number of single-bit flips after which SWITCH 7 or CONFIRM 7 passes the check
 */
uint32_t armedAfterFlips(const ArmState &start)
{
    uint32_t armed = 0U;
    Raw raw{};

    for (uint32_t bit = 0U; bit < (sizeof(ArmState) * kBitsPerByte); bit++) {
        ArmState copy;
        std::memcpy(raw.data(), &start, sizeof(ArmState));
        raw[bit / kBitsPerByte] = static_cast<uint8_t>(raw[bit / kBitsPerByte] ^
                                                       (1U << (bit % kBitsPerByte)));
        std::memcpy(&copy, raw.data(), sizeof(ArmState));
        copy.expireWhenDue(kArmedAt + 1U);
        bool passes = (copy.check(kSwitch, kSeq, kArmedAt + 1U) == AnswerCode::kSuccess) ||
                      (copy.check(kConfirm, kSeq, kArmedAt + 1U) == AnswerCode::kSuccess);
        armed += passes ? 1U : 0U;
    }
    return armed;
}

/**
 * @brief REQ-137: no single bit error makes the arm state armed.
 */
void testBitFlips()
{
    ArmState disarmed;
    ArmState used = armedForSwitch();
    ArmState expired = armedForSwitch();
    ArmState armed = armedForSwitch();

    used.disarm();
    expired.expireWhenDue(kArmedAt + kArmExpiryMs);
    char line[200];
    uint32_t bits = static_cast<uint32_t>(sizeof(ArmState)) * kBitsPerByte;

    std::snprintf(line, sizeof line,
                  "REQ-137: each of the %u bits flipped in turn, from disarmed (start), disarmed "
                  "(used up) and expired: never armed",
                  bits);
    report((armedAfterFlips(disarmed) == 0U) && (armedAfterFlips(used) == 0U) &&
               (armedAfterFlips(expired) == 0U),
           line);

    std::snprintf(line, sizeof line,
                  "REQ-137: each of the %u bits flipped in turn, from armed for SWITCH 7: the flip "
                  "disarms it every time (fails safe)",
                  bits);
    report(armedAfterFlips(armed) == 0U, line);

    /* The same bit flipped in the state word and in its complement keeps the pair matched; the
     * code words still keep it from reading as armed (16 bits apart, arm_state.hpp). */
    uint32_t armed_pairs = 0U;
    for (uint32_t bit = 0U; bit < 32U; bit++) {
        Raw raw{};
        ArmState copy;
        std::memcpy(raw.data(), &disarmed, sizeof(ArmState));
        raw[bit / kBitsPerByte] = static_cast<uint8_t>(raw[bit / kBitsPerByte] ^
                                                       (1U << (bit % kBitsPerByte)));
        raw[4U + (bit / kBitsPerByte)] = static_cast<uint8_t>(raw[4U + (bit / kBitsPerByte)] ^
                                                              (1U << (bit % kBitsPerByte)));
        std::memcpy(&copy, raw.data(), sizeof(ArmState));
        armed_pairs += (copy.check(0U, 0U, 0U) == AnswerCode::kSuccess) ? 1U : 0U;
    }
    report(armed_pairs == 0U,
           "the same bit flipped in the state word and its complement (32 cases, from "
           "disarmed): never armed");
}

}  // namespace

/**
 * @brief Runs every arm-state test.
 *
 * @return 0 if all passed, 1 otherwise
 */
int main()
{
    testParse();
    testCheck();
    testExpireWhenDue();
    testBitFlips();
    if (failures == 0U) {
        std::printf("arm state unit tests: all %u passed\n", tests);
        return 0;
    }
    std::printf("arm state unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
