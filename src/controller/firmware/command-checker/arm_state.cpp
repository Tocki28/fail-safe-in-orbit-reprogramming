/*
 * arm_state.cpp — see arm_state.hpp.
 */
#include "arm_state.hpp"

#include "fault.hpp"

namespace {

/* Payload offsets (Interfaces Sec. 5.1): identifier, command identifier, sequence number. */
constexpr uint32_t kAtCommand = 1U;
constexpr uint32_t kAtSequence = 2U;

/**
 * @brief Returns the 32-bit little-endian value at bytes (Interfaces Sec. 4.0).
 *
 * @param[in]     bytes  first of the 4 bytes
 * @return the value
 */
uint32_t readU32(const uint8_t *bytes)
{
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

}  // namespace

/**
 * @brief Returns true if the payload is an ARM command (identifier 0x0D), whatever its size.
 *
 * The size is checked by parse(), so an ARM of the wrong size is refused 0x06 as an ARM
 * (REQ-028), not as an unknown command.
 *
 * @param[in]     payload  command payload
 * @param[in]     length   payload bytes
 * @return true if the first byte is 0x0D, false otherwise or for an empty payload
 */
bool ArmState::isArm(const uint8_t *payload, uint16_t length)
{
    return (length >= 1U) && (payload[0] == kCmdArm);
}

/**
 * @brief Reads the fields of an ARM payload (Interfaces Sec. 5.1, little-endian, Sec. 4.0).
 *
 * Any command identifier is accepted: ARM allows only the command it names, and only dangerous
 * commands ask for an ARM, so naming any other identifier allows nothing.
 *
 * @param[in]     payload  command payload, identifier 0x0D first
 * @param[in]     length   payload bytes
 * @param[out]    out      the command identifier and sequence number named; set only on true
 * @return true if the payload is an ARM of exactly 6 bytes, false otherwise
 */
bool ArmState::parse(const uint8_t *payload, uint16_t length, ArmRequest &out)
{
    if ((length != kArmPayloadBytes) || !isArm(payload, length)) {
        return false;
    }
    out.command = payload[kAtCommand];
    out.sequence = readU32(&payload[kAtSequence]);
    return true;
}

/**
 * @brief Returns true if every stored word equals the complement of its partner (REQ-137).
 *
 * @return true if all four pairs match, false if any bit of any pair was changed
 */
bool ArmState::pairsMatch() const
{
    return (state_ == ~state_inv_) && (command_ == ~command_inv_) &&
           (sequence_ == ~sequence_inv_) && (armed_ms_ == ~armed_ms_inv_);
}

/**
 * @brief Checks that the arm state is intact; if it is not, the fault reaction.
 *
 * Intact: every word matches its complement, and the state word is one of the three code words
 * (arm_state.hpp). Anything else can only come from a changed bit in memory, since store() is
 * the one writer: the fault reaction (firmware/fault.hpp; owner's decision, 13 Oct 2026; REQ-041,
 * detect a change in its own memory). REQ-137 holds too: a corrupted state is never armed,
 * because the chip restarts with the arm state disarmed. Before, a broken pair read as not armed.
 *
 * @note Does not return if the state is not intact.
 */
void ArmState::checkIntact() const
{
    if (!pairsMatch()) {
        faultImpossibleValue("ArmState: word and complement differ");
    }
    if ((state_ != kDisarmed) && (state_ != kArmed) && (state_ != kExpired)) {
        faultImpossibleValue("ArmState: state word is no code word");
    }
}

/**
 * @brief Writes all four words and their complements.
 *
 * The state word goes last, so a state left half-written (a fault between the writes) reads as
 * a broken pair, never as armed with old fields. Only the main loop calls this; no interrupt
 * touches the arm state.
 *
 * @param[in]     state     kDisarmed, kArmed or kExpired
 * @param[in]     command   identifier of the command allowed
 * @param[in]     sequence  sequence number that command must carry
 * @param[in]     armed_ms  arrival time of the ARM, milliseconds since start
 */
void ArmState::store(uint32_t state, uint32_t command, uint32_t sequence, uint32_t armed_ms)
{
    state_ = kDisarmed;
    state_inv_ = ~kDisarmed;
    command_ = command;
    command_inv_ = ~command;
    sequence_ = sequence;
    sequence_inv_ = ~sequence;
    armed_ms_ = armed_ms;
    armed_ms_inv_ = ~armed_ms;
    state_ = state;
    state_inv_ = ~state;
}

/**
 * @brief Stores an ARM that arrived at now_ms; it replaces any earlier ARM (Interfaces
 * Sec. 5.2: an ARM names the one command it allows).
 *
 * @param[in]     request  the command identifier and sequence number named
 * @param[in]     now_ms   arrival time, milliseconds since start
 */
void ArmState::arm(const ArmRequest &request, uint32_t now_ms)
{
    store(kArmed, request.command, request.sequence, now_ms);
}

/**
 * @brief The arming check of a dangerous command (REQ-029, REQ-030), in the order of the
 * decided design: no valid ARM, or one naming another command or another sequence number,
 * gives 0x01; an ARM older than kArmExpiryMs gives 0x02.
 *
 * A broken pair or an unknown state word is the fault reaction (checkIntact; REQ-137 holds: it
 * is never armed). Expired: kArmExpiryMs or more since the ARM arrived, or marked expired by
 * expireWhenDue(). The mark keeps an old ARM expired after the millisecond count wraps (49.7
 * days), when now - armed_ms alone would look fresh again.
 *
 * @param[in]     command   identifier of the dangerous command
 * @param[in]     sequence  sequence number of the dangerous command's frame
 * @param[in]     now_ms    arrival time of the dangerous command, milliseconds since start
 * @return kSuccess if the ARM allows this command now; kNotArmed (0x01) or kArmExpired (0x02)
 * @note Does not return if the arm state is not intact (checkIntact).
 */
AnswerCode ArmState::check(uint8_t command, uint32_t sequence, uint32_t now_ms) const
{
    checkIntact();
    if (state_ == kDisarmed) {
        return AnswerCode::kNotArmed;
    }
    if ((command_ != static_cast<uint32_t>(command)) || (sequence_ != sequence)) {
        return AnswerCode::kNotArmed;
    }
    /* Unsigned subtraction stays right when the millisecond count wraps. */
    if ((state_ == kExpired) || ((now_ms - armed_ms_) >= kArmExpiryMs)) {
        return AnswerCode::kArmExpired;
    }
    return AnswerCode::kSuccess;
}

/**
 * @brief Uses the ARM up: the state becomes disarmed, the fields cleared.
 */
void ArmState::disarm()
{
    store(kDisarmed, 0U, 0U, 0U);
}

/**
 * @brief Marks an armed state expired once kArmExpiryMs have passed since the ARM.
 *
 * Called on every main-loop pass. A pass lasts less than the watchdog's 1.88 s
 * (watchdog.hpp), so the mark is set long before the millisecond count could wrap. Being called
 * on every pass, it is also where a changed bit in the arm state is found first (checkIntact).
 *
 * @param[in]     now_ms  milliseconds since start
 * @note Does not return if the arm state is not intact (checkIntact).
 */
void ArmState::expireWhenDue(uint32_t now_ms)
{
    checkIntact();
    if ((state_ == kArmed) && ((now_ms - armed_ms_) >= kArmExpiryMs)) {
        store(kExpired, command_, sequence_, armed_ms_);
    }
}
