/*
 * arm_state.hpp — the arming command and the arm state it leaves (Interfaces Sec. 5.2,
 * REQ-029, REQ-030, REQ-137). The first piece of the command checker (Architecture: the checks
 * on arrival); sequence numbers, expiry and scheduled commands follow in step 2b.
 *
 * ARM (0x0D) names the one dangerous command it allows, by identifier and sequence number
 * (Interfaces Sec. 5.1, command table: command identifier (1), its sequence number (4),
 * little-endian, Sec. 4.0). It allows no other command and expires after kArmExpiryMs. A new
 * ARM replaces the old one. A dangerous command that passes the arming check uses the ARM up.
 *
 * REQ-137, no single bit error makes it armed: every stored word sits next to its bitwise
 * complement, and the state counts only while every pair matches. One flipped bit breaks its
 * pair, and a broken pair is the fault reaction (firmware/fault.hpp; owner's decision of 13 Oct
 * 2026: a value that cannot exist is a fault). Before, a broken pair read as "not armed", a
 * refusal: a normal-looking answer for corrupted memory. The state word itself is a code word:
 * disarmed
 * 0x00000000, armed 0xA5C33C5A, expired 0x3C5AA5C3. Any two differ in 16 bits, and each is
 * 16 bits away from the other's complement, so turning one valid state into another takes 32
 * flipped bits (16 in the word, the same 16 in its complement). Rejected: a plain bool
 * "armed", which one flipped bit sets; a CRC over the fields, which needs more code for the
 * same single-bit guarantee.
 *
 * No hardware here: the same file compiles for the board and for the unit tests
 * (test/controller/unit/test_arm.cpp).
 */
#ifndef ARM_STATE_HPP
#define ARM_STATE_HPP

#include <cstdint>

#include "frame.hpp"

/* ARM: identifier 0x0D, then the command identifier (1) and its sequence number (4)
 * (Interfaces Sec. 5.1). */
constexpr uint8_t kCmdArm = 0x0DU;
constexpr uint16_t kArmPayloadBytes = 6U;

/* How long an ARM stays valid: 30 s from its arrival. Bench value for TBD-arm (REQ-030,
 * Interfaces Sec. 5.2). Accepted by the owner, 13 Oct 2026 (decision Q), as the bench value; the
 * flight value stays TBD-arm. */
constexpr uint32_t kArmExpiryMs = 30000U;

/* The fields of one ARM command. */
struct ArmRequest {
    uint8_t command;     /* identifier of the dangerous command it allows */
    uint32_t sequence;   /* sequence number that command's frame will carry */
};

class ArmState {
public:
    /** @brief True if the payload's identifier is ARM (0x0D). */
    [[nodiscard]] static bool isArm(const uint8_t *payload, uint16_t length);

    /** @brief Read an ARM payload; false if its size is not exactly 6 bytes. */
    [[nodiscard]] static bool parse(const uint8_t *payload, uint16_t length, ArmRequest &out);

    /** @brief Store an ARM that arrived at now_ms, replacing any earlier one. */
    void arm(const ArmRequest &request, uint32_t now_ms);

    /** @brief The arming check of one dangerous command: kSuccess, kNotArmed or kArmExpired. */
    AnswerCode check(uint8_t command, uint32_t sequence, uint32_t now_ms) const;

    /** @brief Use the ARM up: the state becomes disarmed. */
    void disarm();

    /** @brief Mark an armed state expired once kArmExpiryMs have passed; called every loop pass. */
    void expireWhenDue(uint32_t now_ms);

private:
    [[nodiscard]] bool pairsMatch() const;
    void checkIntact() const;
    void store(uint32_t state, uint32_t command, uint32_t sequence, uint32_t armed_ms);

    /* Code words of the state (see the file comment). */
    static constexpr uint32_t kDisarmed = 0x00000000U;
    static constexpr uint32_t kArmed = 0xA5C33C5AU;
    static constexpr uint32_t kExpired = 0x3C5AA5C3U;

    /* Each word next to its complement. Starts disarmed with matching pairs, so the object is
     * constant-initialized (main.cpp) and sits in .data: 32 bytes. */
    uint32_t state_ = kDisarmed;
    uint32_t state_inv_ = ~kDisarmed;
    uint32_t command_ = 0U;
    uint32_t command_inv_ = ~0U;
    uint32_t sequence_ = 0U;
    uint32_t sequence_inv_ = ~0U;
    uint32_t armed_ms_ = 0U;
    uint32_t armed_ms_inv_ = ~0U;
};

#endif /* ARM_STATE_HPP */
