/*
 * durable_record.hpp — one small fixed-size record kept in a file of the data directory, so that
 * it survives a restart and a power cut, and is never read back damaged without saying so.
 *
 * File layout: the record's bytes, then their CRC-32 (crc32.hpp, Interfaces Sec. 4.0), 4 bytes
 * little-endian (bytes.hpp). The file is exactly record bytes + 4 long; any other length is
 * damage.
 *
 * write(): the whole new file goes to "<name>.tmp", is flushed (fsync), is renamed over
 * "<name>", and the directory is flushed. rename() replaces the name in one step on ext4 and
 * the other POSIX file systems, so a power cut leaves the old record or the new one, never half
 * of each; the directory flush makes the new name itself survive the cut (the same order as
 * update_manager.cpp: data, flush, rename, flush the directory).
 *
 * read(): returns what the file is, as a value (code rules A3, B1, B4): RECORD_OK with the
 * bytes copied out, RECORD_ABSENT when there is no file (first start, or never written),
 * RECORD_DAMAGED when the file is the wrong size or its CRC-32 does not match. Nothing here
 * throws, and no default record is ever made up: a caller that gets anything but RECORD_OK has
 * no record and must decide what that means (the caller's decision, not this class's).
 */
#ifndef FSIR_UPDATER_DURABLE_RECORD_HPP
#define FSIR_UPDATER_DURABLE_RECORD_HPP

#include <array>
#include <cstddef>
#include <cstdint>

class DurableRecord
{
public:
  // What read() found. Unscoped, so the names are DurableRecord::RECORD_OK and so on.
  enum Status : uint8_t
  {
    RECORD_OK,        // the file is whole and its CRC-32 matches: the bytes were copied out
    RECORD_ABSENT,    // no file of that name (first start, or never written)
    RECORD_DAMAGED,   // the file is the wrong size or its CRC-32 does not match
    RECORD_ERROR,     // no verdict: bad request, or the file system failed (error holds errno)
  };

  // The largest record this class keeps. Bench choice (TBR, awaiting the owner's acceptance):
  // the brief for this class says "at most 64 bytes"; the first user is a counter or a short
  // marker, and a larger record is a different design (a log, not a record).
  static constexpr size_t kRecordMaxBytes = 64;

  // The CRC-32 that follows the record's bytes in the file (crc32.hpp: 32 bits).
  static constexpr size_t kCrcBytes = 4;

  // The longest path (directory, "/", name, ".tmp", zero) held. Bench choice (TBR): a fixed
  // buffer so that nothing is allocated; 256 is far longer than the data directory paths of the
  // board (--data-dir) and of the tests; a longer one is not configured.
  static constexpr size_t kPathMaxBytes = 256;

  DurableRecord(const char * directory, const char * name, size_t record_bytes);

  // The directory, name and size were usable: a path fits, the name has no '/', and the size is
  // 1 to kRecordMaxBytes. write() and read() return an error otherwise.
  bool configured() const;

  // Writes the record (record_bytes bytes) so that it survives a power cut. False if not
  // configured, size is not record_bytes, or any step failed; error then holds the errno value
  // (EINVAL for the first two). The old record is left as it was when false is returned.
  [[nodiscard]] bool write(const uint8_t * record, size_t size, int & error);

  // Reads the record into out (out_size must be record_bytes). out is written only when
  // RECORD_OK is returned. RECORD_ERROR: error holds the errno value (EINVAL for a bad request).
  [[nodiscard]] Status read(uint8_t * out, size_t out_size, int & error) const;

private:
  bool configured_;
  size_t record_bytes_;
  std::array<char, kPathMaxBytes> directory_;   // the data directory
  std::array<char, kPathMaxBytes> path_;        // directory/name
  std::array<char, kPathMaxBytes> temp_path_;   // directory/name.tmp
};

#endif  // FSIR_UPDATER_DURABLE_RECORD_HPP
