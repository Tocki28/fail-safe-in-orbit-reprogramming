/*
 * clock_record.hpp — the onboard clock offset as a DurableRecord named "clock-offset" in the data
 * directory (owner decision 25, 15 Oct 2026: "SET ONBOARD TIME stores an offset"; REQ-072,
 * REQ-073).
 *
 * Onboard ground time = the system clock (seconds, Unix time) + the offset. The offset is signed:
 * the board's clock may run ahead of or behind ground time. SET ONBOARD TIME computes it as
 * ground time - system clock and stores it here, so the system clock itself is never set (REQ-073:
 * a correction that does not depend on the onboard clock).
 *
 * Record, 8 bytes (config::kClockOffsetRecordBytes), little-endian:
 *   offset 0  8 bytes  offset in seconds, signed 64-bit (two's complement)
 * Proposal (TBR), awaiting the owner's acceptance: the width and layout. Decision 25 gives
 * neither; 64 bits cannot overflow with any 32-bit ground time and any 64-bit system time.
 *
 * What a read gives (load), as a value (code rules A3):
 *   kStored      a good record: its offset.
 *   kNoRecord    no file (first start): offset 0, the contract's default.
 *   kDamaged     the file is the wrong size or its CRC-32 is wrong: offset 0.
 *   kUnreadable  the file system failed (RECORD_ERROR): offset 0.
 * Proposal (TBR), awaiting the owner's acceptance: a damaged or unreadable record gives offset 0
 * and a log line, not a refusal of everything. Fail-closed (as the limits record is) is not useful
 * for a clock: with no usable time there is nothing to refuse safely, and the next SET ONBOARD
 * TIME writes a good record.
 */
#ifndef FSIR_UPDATER_CLOCK_RECORD_HPP
#define FSIR_UPDATER_CLOCK_RECORD_HPP

#include <cstdint>

#include "durable_record.hpp"

namespace clock_record
{

// What load() found.
enum class Source : uint8_t
{
  kStored,       // a good record
  kNoRecord,     // no file: offset 0
  kDamaged,      // the file is damaged: offset 0
  kUnreadable,   // the file system failed: offset 0
};

// Reads the record. offset is always written: the stored offset, or 0 for every other result.
// error: errno for kUnreadable, else 0.
[[nodiscard]] Source load(const DurableRecord & record, int64_t & offset, int & error);

// Writes the offset as the record, so that it survives a power cut. False if any step failed
// (error: errno); the old record is then as it was, except that a failed final directory flush
// can leave the new one in place (durable_record.hpp).
[[nodiscard]] bool save(DurableRecord & record, int64_t offset, int & error);

// A short text for the log: "stored", "no record", "damaged", "unreadable".
const char * sourceName(Source source);

}  // namespace clock_record

#endif  // FSIR_UPDATER_CLOCK_RECORD_HPP
