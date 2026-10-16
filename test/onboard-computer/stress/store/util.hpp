/*
 * util.hpp — small helpers of the store stress test: files, directories, a seeded generator, and
 * an oracle that reads the report store's file by its documented layout (report_store.hpp,
 * "File layout"), without using the store's code. The oracle is the test's independent view of
 * what the file says.
 */
#ifndef FSIR_STRESS_STORE_UTIL_HPP
#define FSIR_STRESS_STORE_UTIL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace util
{

using Bytes = std::vector<uint8_t>;

// Layout of the store's file: report_store.cpp:403-419 (tags, framing, state body) and
// report_store.hpp "File layout". Copied here on purpose: the oracle must not share the code
// under test.
constexpr uint8_t kTagReport = 0x52U;   // report_store.cpp:403
constexpr uint8_t kTagState = 0x53U;    // report_store.cpp:404
constexpr size_t kHeadBytes = 2;        // report_store.cpp:407
constexpr size_t kCrcBytes = 4;         // report_store.cpp:408
constexpr size_t kOverhead = kHeadBytes + kCrcBytes;
constexpr size_t kStateBytes = 24;      // report_store.cpp:412
constexpr size_t kStateNextAt = 0;      // report_store.cpp:413 to 418
constexpr size_t kStateDeletedAt = 4;
constexpr size_t kStateThroughAt = 8;
constexpr size_t kStateCountAt = 12;
constexpr size_t kStateFirstAt = 16;
constexpr size_t kStateLastAt = 20;
// Report layout: protocol.hpp:167-171 (number, onboard time, controller time, kind) and
// Sec. 5.4 lengths (protocol.hpp:171, :247-248).
constexpr size_t kNumberAt = 0;
constexpr size_t kTimeAt = 4;
constexpr size_t kKindAt = 12;
constexpr uint8_t kKindHealth = 0x01U;    // protocol.hpp:140
constexpr uint8_t kKindEvent = 0x02U;     // protocol.hpp:141
constexpr size_t kHealthLen = 24;         // protocol.hpp:171
constexpr size_t kEventMinLen = 15;       // protocol.hpp:247
constexpr size_t kEventMaxLen = 47;       // protocol.hpp:248

// One record of the file as the oracle read it.
struct Rec
{
  size_t offset;
  size_t size;
  uint8_t tag;
  Bytes body;
};

// The good records of a byte string, up to the first one that is not good.
struct Parsed
{
  std::vector<Rec> recs;
  size_t good_end;
};

// One stored report.
struct Rep
{
  uint32_t number;
  uint8_t kind;
  Bytes bytes;
};

// The state in force and the live reports of a parsed file (report_store.hpp: the last good
// state record is in force; a report is live when not deleted and not dropped).
struct View
{
  uint32_t state_next;
  uint32_t deleted_below;
  uint32_t through;
  uint32_t count;
  uint32_t first_s;
  uint32_t last_s;
  bool have_state;
  uint32_t highest;               // highest report number in any report record; 0: none
  size_t live_bytes;              // live report records, framing included
  std::vector<Rep> events;        // live EVENT reports, file order
  std::vector<Rep> health;        // live HEALTH reports, file order
};

bool readFile(const std::string & path, Bytes & out);
bool writeFile(const std::string & path, const uint8_t * data, size_t size);
bool resetDir(const std::string & path);
bool copyDir(const std::string & from, const std::string & to);
uint64_t splitmix(uint64_t & state);
uint32_t readLe32(const uint8_t * p);
void writeLe32(uint8_t * p, uint32_t v);
uint32_t crc32(const uint8_t * data, size_t size);
Bytes encodeRecord(uint8_t tag, const Bytes & body);
Bytes stateBody(uint32_t next, uint32_t deleted, uint32_t through, uint32_t count, uint32_t first,
  uint32_t last);
Parsed parse(const uint8_t * data, size_t size);
View evaluate(const Parsed & parsed);
std::string nowText();
// One case of runForked: gets the context and its index; its return value is the exit code.
using Job = int (*)(const void *, size_t);

void runForked(size_t count, size_t workers, Job job, const void * context,
  std::vector<int> & status);

}  // namespace util

#endif  // FSIR_STRESS_STORE_UTIL_HPP
