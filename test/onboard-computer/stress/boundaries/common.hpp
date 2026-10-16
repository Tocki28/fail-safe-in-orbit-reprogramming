/*
 * common.hpp — helpers shared by the boundary stress cases (Parts A.4 and A.6): result lines,
 * payload builders, answer readers, scratch directories, and the log of the updater.
 *
 * One case runs in one child process (boundaries.cpp, runIsolated), so a sanitizer abort in one
 * case does not stop the others. Every result is one line on stdout:
 *   OK       id | what | expected: ... | actual: ...    the code does what its source says
 *   FINDING  id | what | expected: ... | actual: ...    the code does something else
 *   NOTE     id | what | expected: not specified | actual: ...   a question for Tom
 *   INFO     id | what | measured: ...                     a measurement, no verdict
 */
#ifndef FSIR_STRESS_BOUNDARIES_COMMON_HPP
#define FSIR_STRESS_BOUNDARIES_COMMON_HPP

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "bytes.hpp"
#include "clock_record.hpp"
#include "config.hpp"
#include "durable_record.hpp"
#include "protocol.hpp"
#include "rig.hpp"
#include "stress_clock.hpp"

namespace stress
{

// A fixed instant used as the system clock of most cases: 1,700,000,000 s since 1970. Test
// choice: a round number that fits 32 bits and lies far from every boundary tested.
constexpr int64_t kT0 = 1700000000;

// Expiry field meaning "latest possible": 0xFFFFFFFF, the largest 4-byte value (Interfaces
// Sec. 4.0 frame table, Expiry 4 bytes). The updater uses the same value for "never" (kNoExpiry,
// updater.cpp:18).
constexpr uint32_t kFarExpiry = 0xFFFFFFFFU;

// Identity and minimum version sent in SET LIMITS: the defaults of config.hpp:182-187 (minimum
// version 1, identity 0x0431), so that a SET LIMITS changes nothing the other cases rely on.
constexpr uint32_t kLimitsVersion = config::kDefaultMinimumVersion;
constexpr uint16_t kLimitsIdentity = config::kDefaultControllerIdentity;

// Where the cases write their data directories and the updater's log. Set by main().
extern std::string g_scratch;
extern std::string g_log_path;

/**
 * @brief Prints one result line.
 *
 * Flushes at once so that a later abort of the child cannot swallow it.
 *
 * @param[in]     verdict   "OK", "FINDING", "NOTE" or "INFO"
 * @param[in]     id        the case and check, e.g. "expiry-boundary/g-1"
 * @param[in]     what      what was done
 * @param[in]     expected  what the source says should happen, with its locator
 * @param[in]     actual    what the code did
 * @return nothing
 */
void emit(const char * verdict, const std::string & id, const std::string & what,
  const std::string & expected, const std::string & actual);

/**
 * @brief Formats a string like printf.
 *
 * @param[in]     format  printf format
 * @return the formatted text
 */
std::string fmt(const char * format, ...) __attribute__((format(printf, 1, 2)));

/**
 * @brief Gives a clean, new data directory for one case.
 *
 * Removes an old one of the same name first.
 *
 * @param[in]     name  the case's name
 * @return the directory path, under g_scratch
 */
std::string freshDir(const std::string & name);

/**
 * @brief Makes a rig over a directory and opens it; ends the process if that fails.
 *
 * @param[in]     dir  the data directory
 * @return the open rig
 */
std::unique_ptr<Rig> makeRig(const std::string & dir);

/**
 * @brief Writes the clock offset record into a data directory before the updater starts.
 *
 * @param[in]     dir     data directory (made if absent)
 * @param[in]     offset  the offset, seconds, signed 64-bit
 * @return true: written
 */
bool writeOffset(const std::string & dir, int64_t offset);

/**
 * @brief Builds an ARM payload (Interfaces Sec. 5.1 table, row 0x0D).
 *
 * @param[in]     id        the dangerous command it names
 * @param[in]     sequence  that command's ground sequence number
 * @return identifier, named identifier, sequence (little-endian)
 */
std::vector<uint8_t> armPayload(uint8_t id, uint32_t sequence);

/**
 * @brief Builds a SET LIMITS payload (Interfaces Sec. 5.1 table, row 0x06).
 *
 * @param[in]     version   minimum version
 * @param[in]     identity  controller identity
 * @return identifier, version (4), identity (2), little-endian
 */
std::vector<uint8_t> limitsPayload(uint32_t version, uint16_t identity);

/**
 * @brief Builds a SET ONBOARD TIME payload (Interfaces Sec. 5.1 table, row 0x09).
 *
 * @param[in]     ground_time  ground time, unsigned seconds
 * @return identifier, ground time (4), little-endian
 */
std::vector<uint8_t> timePayload(uint32_t ground_time);

/**
 * @brief Names the answers received for one ground sequence number, e.g. "RECEIVED,DONE".
 *
 * @param[in]     answers   what the ground received
 * @param[in]     sequence  the ground sequence number
 * @return the kinds in order, a REFUSED with its code; "none" if nothing came
 */
std::string show(const std::vector<GroundAnswer> & answers, uint32_t sequence);

/**
 * @brief Tells whether the final answer for a sequence number is the one given.
 *
 * @param[in]     answers   what the ground received
 * @param[in]     sequence  the ground sequence number
 * @param[in]     kind      answer::kDone, kRefused or kAlreadyDone
 * @param[in]     code      the code expected; ignored for kAlreadyDone
 * @return true: the last final answer for the number matches
 */
bool finalIs(const std::vector<GroundAnswer> & answers, uint32_t sequence, uint8_t kind,
  uint8_t code);

/**
 * @brief Tells whether any answer for a sequence number has this kind.
 *
 * @param[in]     answers   what the ground received
 * @param[in]     sequence  the ground sequence number
 * @param[in]     kind      the kind sought
 * @return true: at least one answer of that kind
 */
bool hasKind(const std::vector<GroundAnswer> & answers, uint32_t sequence, uint8_t kind);

/**
 * @brief Gives the size of the updater's log so far.
 *
 * @return bytes in the log file
 */
size_t logSize();

/**
 * @brief Counts how often a text occurs in the updater's log after a mark.
 *
 * @param[in]     mark    byte position from logSize()
 * @param[in]     needle  the text sought
 * @return the number of occurrences
 */
size_t logCount(size_t mark, const std::string & needle);


/**
 * @brief Sends one command from the ground, pumps, and returns the answers it caused.
 *
 * Ends the process (status 2) if the datagram does not reach the gate: a broken setup, not a
 * result.
 *
 * @param[in,out] rig          the rig
 * @param[in]     destination  frame::kDestOnboard or kDestController
 * @param[in]     sequence     the ground sequence number
 * @param[in]     payload      the command's payload
 * @param[in]     expiry       the Expiry field
 * @return every answer the ground received since the call began
 */
std::vector<GroundAnswer> step(Rig & rig, uint8_t destination, uint32_t sequence,
  const std::vector<uint8_t> & payload, uint32_t expiry);

/**
 * @brief Compares an actual text with the expected one and prints OK or FINDING.
 *
 * @param[in]     id        the case and check
 * @param[in]     what      what was done
 * @param[in]     expected  the expected text
 * @param[in]     actual    the actual text
 * @param[in]     source    where the expectation is written (file:line, REQ, decision)
 * @return true: equal
 */
bool check(const std::string & id, const std::string & what, const std::string & expected,
  const std::string & actual, const std::string & source);

/**
 * @brief Finds the onboard computer's EVENT 0x0105 among reports and reads its detail.
 *
 * @param[in]     reports  report payloads the ground received
 * @param[out]    detail   the signed difference, seconds (Interfaces Sec. 5.4 row 0x0105)
 * @return true: an EVENT 0x0105 with a 4-byte detail was found (the last one is read)
 */
bool clockEventDetail(const std::vector<std::vector<uint8_t>> & reports, int32_t & detail);

}  // namespace stress

#endif  // FSIR_STRESS_BOUNDARIES_COMMON_HPP
