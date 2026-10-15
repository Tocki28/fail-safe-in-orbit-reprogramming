/*
 * limits_record.hpp — the image limits (minimum version, controller identity) as a DurableRecord
 * named "limits" in the data directory (owner decision 24, 15 Oct 2026; REQ-045, REQ-046, REQ-053).
 *
 * Record, 6 bytes (config::kLimitsRecordBytes), little-endian, in the field order of SET LIMITS
 * (Interfaces Sec. 5.1 table, row 0x06):
 *   offset 0  4 bytes  minimum version
 *   offset 4  2 bytes  controller identity
 *
 * What a read gives (load), as a value (code rules A3):
 *   kStored      a good record: its limits, usable.
 *   kNoRecord    no file (first start): the defaults of decision 24, usable.
 *   kDamaged     the file is the wrong size or its CRC-32 is wrong: the defaults' values with
 *                usable = false, so the image checker refuses every image 0x0A until a SET LIMITS
 *                writes a good record (fail closed). TBR, awaiting the owner's acceptance.
 *   kUnreadable  the file system failed (RECORD_ERROR): treated as kDamaged. TBR, awaiting the
 *                owner's acceptance: decision 24 names only the damaged record, and a record that
 *                cannot be read is not shown to be absent.
 * The check-only mode (--check-image) takes a different view of the last two and falls back to
 * the defaults, usable (main.cpp), because the restore script must still be able to check an
 * image when the limits are lost (TBR).
 */
#ifndef FSIR_UPDATER_LIMITS_RECORD_HPP
#define FSIR_UPDATER_LIMITS_RECORD_HPP

#include <cstdint>

#include "durable_record.hpp"
#include "image_checker.hpp"

namespace limits_record
{

// What load() found.
enum class Source : uint8_t
{
  kStored,       // a good record
  kNoRecord,     // no file: the defaults
  kDamaged,      // the file is damaged: not usable
  kUnreadable,   // the file system failed: not usable
};

// Reads the record. limits is always written: the stored limits, the defaults (kNoRecord), or the
// defaults' values with usable = false (kDamaged, kUnreadable). error: errno for kUnreadable.
[[nodiscard]] Source load(const DurableRecord & record, ImageLimits & limits, int & error);

// Writes the limits as the record, so that it survives a power cut. False if any step failed
// (error: errno); the old record is then as it was, except that a failed final directory flush
// can leave the new one in place (durable_record.hpp).
[[nodiscard]] bool save(DurableRecord & record, const ImageLimits & limits, int & error);

// A short text for the log: "stored", "no record", "damaged", "unreadable".
const char * sourceName(Source source);

}  // namespace limits_record

#endif  // FSIR_UPDATER_LIMITS_RECORD_HPP
