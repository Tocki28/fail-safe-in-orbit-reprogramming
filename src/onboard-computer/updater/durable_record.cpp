#include "durable_record.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bytes.hpp"
#include "crc32.hpp"

namespace
{

// Upper bound on read() and write() calls for one file. A call that makes progress moves at
// least one byte, and a file is at most 64 + 4 = 68 bytes, so 68 progressing calls end any loop;
// the rest of the 128 allows for interrupted calls (EINTR). No other source: bench choice.
constexpr size_t kMaxIoCalls = 128;

// Permission bits of the record file: read and write for the owner only (the record may hold
// state that decides the next start). Bench choice (TBR): no source requires a value.
constexpr mode_t kFileMode = 0600;

// The suffix of the file the new record is written to before the rename.
constexpr const char * kTempSuffix = ".tmp";

/**
 * @brief Writes all of a byte block to a file descriptor, with a bounded number of calls.
 *
 * @param[in]     fd     file descriptor open for writing
 * @param[in]     data   the bytes
 * @param[in]     size   number of bytes
 * @param[out]    error  errno value when false is returned; 0 otherwise
 * @return true: all bytes written; false: a write failed or the call bound was reached
 */
bool writeAll(int fd, const uint8_t * data, size_t size, int & error)
{
  error = 0;
  size_t done = 0;
  for (size_t i = 0; i < kMaxIoCalls && done < size; i++) {
    ssize_t n = ::write(fd, &data[done], size - done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;   // interrupted before any byte moved: try again, counted by the bound
      }
      error = errno;
      return false;
    }
    done += static_cast<size_t>(n);
  }
  if (done != size) {
    error = EIO;   // the bound was reached: a file system that keeps writing nothing
    return false;
  }
  return true;
}

/**
 * @brief Flushes a directory so that a rename or a new file in it survives a power cut.
 *
 * A rename is stored in the directory, not in the file; the file's own fsync does not cover it.
 *
 * @param[in]     directory  path of the directory
 * @param[out]    error      errno value when false is returned; 0 otherwise
 * @return true: the directory was flushed; false: it could not be opened or flushed
 */
bool syncDirectory(const char * directory, int & error)
{
  error = 0;
  int fd = ::open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    error = errno;
    return false;
  }
  bool ok = (::fsync(fd) == 0);
  error = ok ? 0 : errno;
  if (::close(fd) != 0 && ok) {
    error = errno;
    ok = false;
  }
  return ok;
}

}  // namespace

/**
 * @brief Makes a record handle for one file of the data directory; touches no file.
 *
 * The paths are built into fixed buffers. A name that is empty or has a '/', a size outside 1 to
 * kRecordMaxBytes, or a path that does not fit, leaves the handle not configured: write() and
 * read() then return an error and never reach the file system.
 *
 * @param[in]     directory     the data directory (--data-dir); null or empty: not configured
 * @param[in]     name          file name inside it, without '/'; null or empty: not configured
 * @param[in]     record_bytes  size of the record, 1 to kRecordMaxBytes
 * @return nothing (constructor)
 */
DurableRecord::DurableRecord(const char * directory, const char * name, size_t record_bytes)
: configured_(false), record_bytes_(record_bytes), directory_{}, path_{}, temp_path_{}
{
  bool usable = (directory != nullptr) && (name != nullptr) && directory[0] != '\0' &&
                name[0] != '\0' && std::strchr(name, '/') == nullptr && record_bytes >= 1 &&
                record_bytes <= kRecordMaxBytes;
  if (!usable) {
    return;
  }
  int dir_n = std::snprintf(directory_.data(), directory_.size(), "%s", directory);
  int path_n = std::snprintf(path_.data(), path_.size(), "%s/%s", directory, name);
  int temp_n = std::snprintf(temp_path_.data(), temp_path_.size(), "%s/%s%s", directory, name,
    kTempSuffix);
  configured_ = dir_n > 0 && path_n > 0 && temp_n > 0 &&
                static_cast<size_t>(dir_n) < directory_.size() &&
                static_cast<size_t>(path_n) < path_.size() &&
                static_cast<size_t>(temp_n) < temp_path_.size();   // snprintf did not cut
}

/**
 * @brief Tells whether the handle can reach a file.
 *
 * @return true: directory, name and size were usable; false: not
 */
bool DurableRecord::configured() const
{
  return configured_;
}

/**
 * @brief Writes the record so that a power cut leaves the old record or the new one.
 *
 * Order: bytes and CRC-32 to "<name>.tmp" (created afresh), fsync, close, rename over "<name>",
 * fsync of the directory. If a step before the rename fails the temporary file is removed and
 * the old record is untouched. If the directory flush fails after the rename the new record is
 * in place but not shown to be durable: false is returned, so the caller does not rely on it.
 *
 * @param[in]     record  the record's bytes
 * @param[in]     size    number of bytes; must be the record_bytes the handle was made with
 * @param[out]    error   errno value when false is returned (EINVAL: not configured or wrong size)
 * @return true: written and flushed; false: not written, or not shown to be durable
 */
bool DurableRecord::write(const uint8_t * record, size_t size, int & error)
{
  error = 0;
  if (!configured_ || record == nullptr || size != record_bytes_) {
    error = EINVAL;
    return false;
  }
  std::array<uint8_t, kRecordMaxBytes + kCrcBytes> file{};
  std::memcpy(file.data(), record, size);
  bytes::writeU32(&file[size], crc::crc32(record, size));
  size_t file_bytes = size + kCrcBytes;

  int fd = ::open(temp_path_.data(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kFileMode);
  if (fd < 0) {
    error = errno;
    return false;
  }
  bool ok = writeAll(fd, file.data(), file_bytes, error);
  if (ok && ::fsync(fd) != 0) {
    error = errno;
    ok = false;
  }
  if (::close(fd) != 0 && ok) {
    error = errno;
    ok = false;
  }
  if (ok && ::rename(temp_path_.data(), path_.data()) != 0) {
    error = errno;
    ok = false;
  }
  if (!ok) {
    int kept = error;
    (void)::unlink(temp_path_.data());   // best effort: a leftover .tmp is overwritten next time
    error = kept;
    return false;
  }
  return syncDirectory(directory_.data(), error);
}

/**
 * @brief Reads the record and checks it.
 *
 * The file's size is taken from the opened file (fstat) and must be exactly record_bytes + 4
 * before any byte is read into the caller's buffer; the bytes are read into a local buffer, the
 * CRC-32 is compared, and only then are they copied to out (code rules B4: length first,
 * validate before use). A missing file is RECORD_ABSENT; any other failure to open or read is
 * RECORD_ERROR, not damage, since the file may be fine.
 *
 * @param[out]    out       receives the record's bytes when RECORD_OK is returned
 * @param[in]     out_size  size of out; must be the record_bytes the handle was made with
 * @param[out]    error     errno value for RECORD_ERROR (EINVAL: not configured or wrong size)
 * @return RECORD_OK, RECORD_ABSENT, RECORD_DAMAGED or RECORD_ERROR
 */
DurableRecord::Status DurableRecord::read(uint8_t * out, size_t out_size, int & error) const
{
  error = 0;
  if (!configured_ || out == nullptr || out_size != record_bytes_) {
    error = EINVAL;
    return RECORD_ERROR;
  }
  int fd = ::open(path_.data(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) {
      return RECORD_ABSENT;
    }
    error = errno;
    return RECORD_ERROR;
  }
  struct stat info{};
  if (::fstat(fd, &info) != 0) {
    error = errno;
    (void)::close(fd);   // read only: nothing to recover from a failed close
    return RECORD_ERROR;
  }
  size_t file_bytes = record_bytes_ + kCrcBytes;
  if (info.st_size < 0 || static_cast<uintmax_t>(info.st_size) != file_bytes) {
    (void)::close(fd);   // read only: nothing to recover from a failed close
    return RECORD_DAMAGED;
  }
  std::array<uint8_t, kRecordMaxBytes + kCrcBytes> file{};
  size_t have = 0;
  for (size_t i = 0; i < kMaxIoCalls && have < file_bytes; i++) {
    ssize_t n = ::read(fd, &file[have], file_bytes - have);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0) {
      error = errno;
      (void)::close(fd);   // read only: nothing to recover from a failed close
      return RECORD_ERROR;
    }
    if (n == 0) {
      break;   // the file ended early: shorter than fstat said, so it changed under us
    }
    have += static_cast<size_t>(n);
  }
  (void)::close(fd);   // read only: nothing to recover from a failed close
  if (have != file_bytes) {
    return RECORD_DAMAGED;
  }
  if (bytes::readU32(&file[record_bytes_]) != crc::crc32(file.data(), record_bytes_)) {
    return RECORD_DAMAGED;
  }
  std::memcpy(out, file.data(), record_bytes_);
  return RECORD_OK;
}
