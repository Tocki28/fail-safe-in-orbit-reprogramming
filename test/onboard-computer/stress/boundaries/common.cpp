/*
 * common.cpp — helpers shared by the boundary stress cases; see common.hpp.
 */
#include "common.hpp"

#include <array>
#include <cinttypes>

namespace stress
{

std::string g_scratch;
std::string g_log_path;

namespace
{

// Exit status of a child that could not set up its rig. Distinct from 0 (case ran) and from the
// signal exits of a sanitizer abort, so the parent can tell a broken setup from a finding.
constexpr int kSetupFailedStatus = 2;

// Longest text one formatted string may hold, bytes. Test choice: the longest line this audit
// prints is a result row of about 400 bytes.
constexpr size_t kFormatMax = 1024;

}  // namespace

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
  const std::string & expected, const std::string & actual)
{
  if (std::string(verdict) == "INFO") {
    std::printf("%-8s %s | %s | measured: %s\n", verdict, id.c_str(), what.c_str(),
      actual.c_str());
  } else {
    std::printf("%-8s %s | %s | expected: %s | actual: %s\n", verdict, id.c_str(),
      what.c_str(), expected.c_str(), actual.c_str());
  }
  (void)std::fflush(stdout);
}

/**
 * @brief Formats a string like printf.
 *
 * @param[in]     format  printf format
 * @return the formatted text
 */
std::string fmt(const char * format, ...)
{
  std::array<char, kFormatMax> text{};
  va_list args;
  va_start(args, format);
  (void)std::vsnprintf(text.data(), text.size(), format, args);
  va_end(args);
  return std::string(text.data());
}

/**
 * @brief Gives a clean, new data directory for one case.
 *
 * Removes an old one of the same name first.
 *
 * @param[in]     name  the case's name
 * @return the directory path, under g_scratch
 */
std::string freshDir(const std::string & name)
{
  const std::string dir = g_scratch + "/" + name;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir;
}

/**
 * @brief Makes a rig over a directory and opens it; ends the process if that fails.
 *
 * @param[in]     dir  the data directory
 * @return the open rig
 */
std::unique_ptr<Rig> makeRig(const std::string & dir)
{
  std::unique_ptr<Rig> rig(new Rig(dir));
  if (!rig->open()) {
    std::printf("SETUP    rig did not open (UDP port %u in use?) in %s\n", kPort, dir.c_str());
    (void)std::fflush(stdout);
    std::_Exit(kSetupFailedStatus);
  }
  return rig;
}

/**
 * @brief Writes the clock offset record into a data directory before the updater starts.
 *
 * @param[in]     dir     data directory (made if absent)
 * @param[in]     offset  the offset, seconds, signed 64-bit
 * @return true: written
 */
bool writeOffset(const std::string & dir, int64_t offset)
{
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  DurableRecord record(dir.c_str(), config::kClockOffsetRecordName,
    config::kClockOffsetRecordBytes);
  int error = 0;
  return clock_record::save(record, offset, error);
}

/**
 * @brief Builds an ARM payload (Interfaces Sec. 5.1 table, row 0x0D).
 *
 * @param[in]     id        the dangerous command it names
 * @param[in]     sequence  that command's ground sequence number
 * @return identifier, named identifier, sequence (little-endian)
 */
std::vector<uint8_t> armPayload(uint8_t id, uint32_t sequence)
{
  std::vector<uint8_t> p(command::kArmBytes, 0);
  p[0] = command::kArm;
  p[1] = id;
  bytes::writeU32(&p[2], sequence);
  return p;
}

/**
 * @brief Builds a SET LIMITS payload (Interfaces Sec. 5.1 table, row 0x06).
 *
 * @param[in]     version   minimum version
 * @param[in]     identity  controller identity
 * @return identifier, version (4), identity (2), little-endian
 */
std::vector<uint8_t> limitsPayload(uint32_t version, uint16_t identity)
{
  std::vector<uint8_t> p(command::kSetLimitsBytes, 0);
  p[0] = command::kSetLimits;
  bytes::writeU32(&p[command::kSetLimitsVersionAt], version);
  bytes::writeU16(&p[command::kSetLimitsIdentityAt], identity);
  return p;
}

/**
 * @brief Builds a SET ONBOARD TIME payload (Interfaces Sec. 5.1 table, row 0x09).
 *
 * @param[in]     ground_time  ground time, unsigned seconds
 * @return identifier, ground time (4), little-endian
 */
std::vector<uint8_t> timePayload(uint32_t ground_time)
{
  std::vector<uint8_t> p(command::kSetOnboardTimeBytes, 0);
  p[0] = command::kSetOnboardTime;
  bytes::writeU32(&p[command::kSetOnboardTimeAt], ground_time);
  return p;
}

/**
 * @brief Names one answer kind as text.
 *
 * @param[in]     a  the answer
 * @return "RECEIVED", "DONE", "ALREADY_DONE" or "REFUSED(0xNN)"; "KIND?" for any other value
 */
static std::string name(const GroundAnswer & a)
{
  if (a.kind == answer::kReceived) {
    return "RECEIVED";
  }
  if (a.kind == answer::kDone) {
    return "DONE";
  }
  if (a.kind == answer::kAlreadyDone) {
    return "ALREADY_DONE";
  }
  if (a.kind == answer::kRefused) {
    return fmt("REFUSED(0x%02x)", a.code);
  }
  return fmt("KIND?(0x%02x)", a.kind);
}

/**
 * @brief Names the answers received for one ground sequence number, e.g. "RECEIVED,DONE".
 *
 * @param[in]     answers   what the ground received
 * @param[in]     sequence  the ground sequence number
 * @return the kinds in order, a REFUSED with its code; "none" if nothing came
 */
std::string show(const std::vector<GroundAnswer> & answers, uint32_t sequence)
{
  std::string text;
  for (const GroundAnswer & a : answers) {
    if (a.answered == sequence) {
      text += (text.empty() ? "" : ",") + name(a);
    }
  }
  return text.empty() ? "none" : text;
}

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
  uint8_t code)
{
  const GroundAnswer * last = nullptr;
  for (const GroundAnswer & a : answers) {
    if (a.answered == sequence && a.kind != answer::kReceived) {
      last = &a;
    }
  }
  if (last == nullptr || last->kind != kind) {
    return false;
  }
  return kind == answer::kAlreadyDone || last->code == code;
}

/**
 * @brief Tells whether any answer for a sequence number has this kind.
 *
 * @param[in]     answers   what the ground received
 * @param[in]     sequence  the ground sequence number
 * @param[in]     kind      the kind sought
 * @return true: at least one answer of that kind
 */
bool hasKind(const std::vector<GroundAnswer> & answers, uint32_t sequence, uint8_t kind)
{
  for (const GroundAnswer & a : answers) {
    if (a.answered == sequence && a.kind == kind) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Reads the whole updater log file.
 *
 * @return the file's text; empty if it cannot be read
 */
static std::string readLog()
{
  std::string text;
  std::FILE * f = std::fopen(g_log_path.c_str(), "rb");
  if (f == nullptr) {
    return text;
  }
  std::array<char, 4096> block{};
  size_t n = 0;
  while ((n = std::fread(block.data(), 1, block.size(), f)) > 0) {
    text.append(block.data(), n);
  }
  (void)std::fclose(f);
  return text;
}

/**
 * @brief Gives the size of the updater's log so far.
 *
 * @return bytes in the log file
 */
size_t logSize()
{
  return readLog().size();
}

/**
 * @brief Counts how often a text occurs in the updater's log after a mark.
 *
 * @param[in]     mark    byte position from logSize()
 * @param[in]     needle  the text sought
 * @return the number of occurrences
 */
size_t logCount(size_t mark, const std::string & needle)
{
  const std::string text = readLog();
  size_t count = 0;
  size_t at = (mark < text.size()) ? mark : text.size();
  // Each hit moves `at` forward by at least one byte, so text.size() + 1 passes are enough
  // (code rule B2).
  for (size_t pass = 0; pass <= text.size() && !needle.empty(); pass++) {
    at = text.find(needle, at);
    if (at == std::string::npos) {
      break;
    }
    count++;
    at += needle.size();
  }
  return count;
}


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
  const std::vector<uint8_t> & payload, uint32_t expiry)
{
  rig.discard();
  if (!rig.ground(destination, sequence, payload, expiry)) {
    std::printf("SETUP    the datagram of sequence %u did not reach the gate\n", sequence);
    (void)std::fflush(stdout);
    std::_Exit(kSetupFailedStatus);
  }
  return rig.answers();
}

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
  const std::string & actual, const std::string & source)
{
  const bool same = (expected == actual);
  emit(same ? "OK" : "FINDING", id, what, expected + " [" + source + "]", actual);
  return same;
}

/**
 * @brief Finds the onboard computer's EVENT 0x0105 among reports and reads its detail.
 *
 * @param[in]     reports  report payloads the ground received
 * @param[out]    detail   the signed difference, seconds (Interfaces Sec. 5.4 row 0x0105)
 * @return true: an EVENT 0x0105 with a 4-byte detail was found (the last one is read)
 */
bool clockEventDetail(const std::vector<std::vector<uint8_t>> & reports, int32_t & detail)
{
  bool found = false;
  for (const std::vector<uint8_t> & p : reports) {
    if (p.size() >= report::kEventDetailOffset + report::kClockDifferenceDetailBytes &&
        p[report::kOffsetKind] == report::kEvent &&
        bytes::readU16(&p[report::kOffsetEventCode]) == report::kEventClockDiffers) {
      detail = static_cast<int32_t>(bytes::readU32(&p[report::kEventDetailOffset]));
      found = true;
    }
  }
  return found;
}

}  // namespace stress
