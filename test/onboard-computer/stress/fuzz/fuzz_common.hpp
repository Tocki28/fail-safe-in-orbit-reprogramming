/*
 * fuzz_common.hpp — what every fuzz target of the onboard computer's parsers shares: a seeded
 * random generator, a byte mutator, a per-input watchdog that saves a hanging or crashing input,
 * timing of the slowest inputs, the log, and an independent CRC-32.
 *
 * The fuzzer is mutation-based and has no coverage feedback: clang 17 on this Mac has no
 * libFuzzer runtime and AFL and honggfuzz are not installed (task brief). Every input is a pure
 * function of (seed, index), so any input is rebuilt with --only INDEX.
 *
 * Bench tool only; nothing here is part of the operating software (REQ-101).
 */
#ifndef FSIR_FUZZ_COMMON_HPP
#define FSIR_FUZZ_COMMON_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace fuzz
{

// Command line of one run.
struct Options
{
  std::string target;       // frame, parser, updater, image, imagefile, records, reports
  uint64_t seed;            // PRNG seed, printed in the log
  uint64_t count;           // inputs per target
  bool has_only;            // --only given: build and run input `only` alone
  uint64_t only;
  std::string out_dir;      // logs and saved inputs
  std::string work_dir;     // scratch files and data directories
  std::string ctrl_images;  // src/controller/build/images (read-only seeds)
  std::string test_images;  // test/controller/build/images (read-only seeds)
};

// A small fast PRNG (splitmix64): the state is one number, so an input can be rebuilt from
// (seed, index) alone.
struct Rng
{
  uint64_t state;
};

// What one run knows: options, the open log, counters, the slowest inputs.
struct Report
{
  Options opt;
  FILE * log;
  uint64_t inputs;
  uint64_t findings;
  uint64_t notes;
  double total_ms;
  double slow_ms[10];
  uint64_t slow_index[10];
};

// A header or record field a mutation may aim a boundary value at.
struct Field
{
  size_t offset;
  size_t width;   // 1, 2, 4 or 8 bytes
};

uint64_t nextU64(Rng & rng);
uint32_t below(Rng & rng, uint32_t bound);
bool chance(Rng & rng, uint32_t percent);
Rng rngFor(uint64_t seed, uint64_t index);
std::vector<uint8_t> randomBytes(Rng & rng, size_t count);
uint32_t readLe32(const uint8_t * b);
uint16_t readLe16(const uint8_t * b);
void writeLe32(uint8_t * b, uint32_t value);
void writeLe16(uint8_t * b, uint16_t value);

// CRC-32 of Interfaces Sec. 4.0, written again here as the reference (bitwise, no table).
uint32_t refCrc32(const uint8_t * data, size_t size);

// Mutates v in place: 1 to 4 of bit flip, special byte, boundary value at a field, truncate,
// extend, delete a range, duplicate a range, splice in a piece of other (may be null).
void mutate(Rng & rng, std::vector<uint8_t> & v, const std::vector<Field> & fields,
  size_t max_size, const std::vector<uint8_t> * other);

// A boundary value for a field of the given width.
uint64_t boundaryValue(Rng & rng, size_t width);

// Parses the command line. False: usage printed.
bool parseOptions(int argc, char ** argv, Options & opt);

// Opens the log, starts the watchdog; returns false if the log cannot be opened.
bool reportOpen(Report & r, const Options & opt, const char * command_line);
void reportClose(Report & r);

// Brackets one input: arms the watchdog on data, times the input, keeps the 10 slowest.
void inputBegin(Report & r, uint64_t index, const uint8_t * data, size_t size);
void inputEnd(Report & r, uint64_t index);

// A finding: counted, written to the log with its title and detail, the input saved as a file
// named after the target, the finding number, the seed and the index. At most kSavedMax files
// per run are written; the count goes on.
void finding(Report & r, const char * title, uint64_t index, const uint8_t * data, size_t size,
  const char * detail);

// A note: information, not a failure (an observation to put to the owner).
void note(Report & r, const char * text);

// Writes a progress line to the log every kProgressEvery inputs.
void progress(Report & r, uint64_t index);

// True when index is to run: all of them, or only the one asked for.
bool wanted(const Report & r, uint64_t index);

// Empties the stderr file so that the updater's own log lines do not fill the disk.
void trimStderr();

// Milliseconds on a monotonic clock.
double nowMs();

}  // namespace fuzz

#endif  // FSIR_FUZZ_COMMON_HPP
