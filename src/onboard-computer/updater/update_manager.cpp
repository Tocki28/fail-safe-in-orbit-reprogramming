#include "update_manager.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// OpenSSL's headers use C casts in inline functions; the warning is switched off for this
// include only, as in image_checker.cpp.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#include <openssl/evp.h>
#pragma GCC diagnostic pop

#include "bytes.hpp"
#include "log.hpp"
#include "protocol.hpp"

namespace
{

// The .part file starts with a head, then the image bytes:
//   offset 0   4 bytes  tag "PRT2" (a .part file of the older form has no tag and is not resumed)
//   offset 4   4 bytes  the declared image length, little-endian
//   offset 8  32 bytes  SHA-256 of the image's first 64 bytes (its header), all zero until the
//                       stored bytes reach 64
constexpr off_t kPartHeadBytes = 40;
constexpr off_t kPartLengthAt = 4;
constexpr off_t kPartHashAt = 8;
constexpr size_t kHashBytes = 32;
constexpr size_t kHeaderBytes = 64;   // the image header (Interfaces Sec. 5.5, Figure 6)
constexpr std::array<uint8_t, 4> kPartTag = {'P', 'R', 'T', '2'};
constexpr mode_t kDirMode = 0755;
constexpr mode_t kFileMode = 0644;

/**
 * @brief Reads size bytes from a file at a byte offset, retrying after EINTR.
 *
 * Reads or writes all size bytes at offset; false on any shortfall.
 * Uses pread, so the file position of fd is not used or changed.
 *
 * @param[in] fd  open file descriptor
 * @param[out] out  buffer that receives the bytes; room for size bytes
 * @param[in] size  number of bytes to read, in bytes
 * @param[in] offset  byte offset in the file where reading starts, in bytes
 * @return true: all size bytes were read; false: end of file or an error came first
 */
bool readAll(int fd, uint8_t * out, size_t size, off_t offset)
{
  size_t done = 0;
  while (done < size) {
    ssize_t n = ::pread(fd, out + done, size - done, offset + static_cast<off_t>(done));
    if (n <= 0) {
      if (n < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    done += static_cast<size_t>(n);
  }
  return true;
}

/**
 * @brief Writes size bytes to a file at a byte offset, retrying after EINTR.
 *
 * Counterpart of readAll: all size bytes at offset; false on any shortfall.
 * Uses pwrite, so the file position of fd is not used or changed.
 *
 * @param[in] fd  open file descriptor
 * @param[in] data  bytes to write; size bytes
 * @param[in] size  number of bytes to write, in bytes
 * @param[in] offset  byte offset in the file where writing starts, in bytes
 * @return true: all size bytes were written; false: an error came first
 */
bool writeAll(int fd, const uint8_t * data, size_t size, off_t offset)
{
  size_t done = 0;
  while (done < size) {
    ssize_t n = ::pwrite(fd, data + done, size - done, offset + static_cast<off_t>(done));
    if (n <= 0) {
      if (n < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    done += static_cast<size_t>(n);
  }
  return true;
}

/**
 * @brief Computes the SHA-256 of a byte range.
 *
 * The .part file keeps this hash of the image's header so that a different image with the same
 * number and length is told apart from the one stored (audit finding #15). OpenSSL's EVP_Digest
 * returns 1 on success.
 *
 * @param[in]     data  bytes to hash
 * @param[in]     size  number of bytes
 * @param[out]    out   the 32-byte hash; written only when true is returned
 * @return true: the hash was computed; false: OpenSSL failed
 */
bool sha256(const uint8_t * data, size_t size, std::array<uint8_t, kHashBytes> & out)
{
  std::array<uint8_t, kHashBytes> hash{};
  unsigned int length = 0;
  if (EVP_Digest(data, size, hash.data(), &length, EVP_sha256(), nullptr) != 1 ||
      length != kHashBytes) {
    return false;
  }
  out = hash;
  return true;
}

/**
 * @brief Deletes a file; a file that is not there is not an error.
 *
 * Any other failure is logged and not reported to the caller.
 *
 * @param[in] path  path of the file
 * @return nothing
 */
void removeFile(const char * path)
{
  if (::unlink(path) != 0 && errno != ENOENT) {
    logging::line("store: cannot remove %s: %s", path, std::strerror(errno));
  }
}

}  // namespace

/**
 * @brief Flushes a directory to storage and says whether it worked.
 *
 * A rename or a new file in a directory survives a power cut only after the directory itself is
 * flushed (REQ-063). Opening and flushing are both checked: a directory that cannot be flushed
 * is not a place to rely on, so the caller must not delete the old copy of what was renamed
 * (audit finding #16). Opened with O_CLOEXEC.
 *
 * @param[in]     dir  path of the directory
 * @return true: opened and flushed; false: either failed (logged)
 */
bool syncDirectory(const char * dir)
{
  int fd = ::open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    logging::line("store: cannot open directory %s to flush it: %s", dir, std::strerror(errno));
    return false;
  }
  bool ok = ::fsync(fd) == 0;
  if (!ok) {
    logging::line("store: cannot flush directory %s: %s", dir, std::strerror(errno));
  }
  (void)::close(fd);   // read-only descriptor; nothing more can be done if it fails
  return ok;
}

/**
 * @brief Makes a store for the data directory dir, with no upload open.
 *
 * dir: the data directory; it is made, with its parents, by ready().
 * The pointer is kept, not copied: the string must outlive the store.
 *
 * @param[in] dir  path of the data directory
 * @return nothing (constructor)
 */
UploadStore::UploadStore(const char * dir)
: dir_(dir), fd_(-1), number_(0), length_(0), stored_(0), hash_{}, have_hash_(false),
  sync_(syncDirectory), limits_(defaultImageLimits()), image_{}
{
}

/**
 * @brief Closes the open upload, if any.
 *
 * The .part file stays on disk, so a later BEGIN can resume it (REQ-049, REQ-063).
 *
 * @return nothing (destructor)
 */
UploadStore::~UploadStore()
{
  closeUpload();
}

/**
 * @brief Makes the data directory with its parents, and checks that it can be written.
 *
 * The directory exists or could be made. False otherwise; errno tells why.
 * The code makes each parent in turn (mkdir -p). A path of 0 or 512 or more characters sets ENAMETOOLONG;
 * a path that is not a directory sets ENOTDIR.
 *
 * @return true: the directory exists and is writable; false: otherwise, errno says why
 */
bool UploadStore::ready()
{
  // mkdir -p: each parent in turn, then the directory itself.
  std::array<char, 512> partial{};
  size_t length = std::strlen(dir_);
  if (length == 0 || length >= partial.size()) {
    errno = ENAMETOOLONG;
    return false;
  }
  for (size_t i = 0; i <= length; i++) {
    if (i == length || (dir_[i] == '/' && i > 0)) {
      std::memcpy(partial.data(), dir_, i);
      partial[i] = '\0';
      if (::mkdir(partial.data(), kDirMode) != 0 && errno != EEXIST) {
        return false;
      }
    }
  }
  struct stat info{};
  if (::stat(dir_, &info) != 0) {
    return false;
  }
  if ((info.st_mode & S_IFMT) != S_IFDIR) {
    errno = ENOTDIR;
    return false;
  }
  return ::access(dir_, W_OK) == 0;
}

/**
 * @brief Builds the file name of an image number, "<dir>/image-NNNNN<suffix>", with the number in 5 digits.
 *
 * The names are the ones in update_manager.hpp: image-n.part for an upload in progress, image-n.img
 * for a stored image.
 *
 * @param[out] out  receives the file name, zero-terminated
 * @param[in] number  image number
 * @param[in] suffix  text appended after the number, e.g. ".part" or ".img"
 * @return true: the name fits in out; false: it was cut off or the formatting failed
 */
bool UploadStore::path(std::array<char, 512> & out, uint16_t number, const char * suffix) const
{
  int n = std::snprintf(out.data(), out.size(), "%s/image-%05u%s", dir_,
    static_cast<unsigned>(number), suffix);
  return n > 0 && static_cast<size_t>(n) < out.size();
}

/**
 * @brief Replaces the function that flushes the data directory.
 *
 * For the tests: they put in a function that fails, or that looks at the folder at the moment
 * of the call, to show the order of rename, flush and delete. The default is syncDirectory.
 *
 * @param[in]     sync  function that flushes a directory and says whether it worked
 * @return nothing
 */
void UploadStore::setDirectorySync(DirectorySync sync)
{
  sync_ = sync;
}

/**
 * @brief Closes the open .part file and forgets the bytes stored count.
 *
 * The file on disk is not removed. Does nothing to the file descriptor if none is open.
 *
 * @return nothing
 */
void UploadStore::closeUpload()
{
  if (fd_ >= 0) {
    (void)::close(fd_);
    fd_ = -1;
  }
  stored_ = 0;
  have_hash_ = false;
}

/**
 * @brief Tells whether an upload is open.
 *
 * The open upload, for the log and the tests.
 *
 * @return true: a .part file is open; false: none
 */
bool UploadStore::open() const
{
  return fd_ >= 0;
}

/**
 * @brief Gives the bytes stored so far in the open upload.
 *
 * The open upload, for the log and the tests.
 *
 * @return bytes stored, in bytes; 0 if no upload is open
 */
uint32_t UploadStore::stored() const
{
  return stored_;
}

/**
 * @brief Opens an upload of an image number, keeping the bytes of a .part file of the same declared length.
 *
 * Each returns 0x00 for DONE, or the refusal reason (protocol.hpp).
 * A length of kImageOverheadBytes or less, or over kMaxImageBytes, is refused (answer::kImpossible).
 * The kept bytes are cut back to a whole number of kChunkBytes chunks, as update_manager.hpp
 * says (REQ-049, REQ-063). A BEGIN for another length starts that image number afresh. Any earlier
 * upload is closed first; a storage failure is answer::kStorageBench. A .part file also keeps the
 * SHA-256 of the image's first 64 bytes (its header): bytes whose first 64 no longer match it are
 * not resumed (audit finding #15). The directory is flushed with the function set by
 * setDirectorySync, and a flush that fails is a storage failure.
 *
 * @param[in] number  image number
 * @param[in] length  declared image length, in bytes
 * @param[out] resume_at  byte offset where the ground resumes: bytes kept, 0 if it starts afresh; set to 0 on entry
 * @return answer::kSuccess (0x00): the upload is open; otherwise the refusal reason
 */
uint8_t UploadStore::begin(uint16_t number, uint32_t length, uint32_t & resume_at)
{
  resume_at = 0;
  if (length <= config::kImageOverheadBytes || length > config::kMaxImageBytes) {
    return answer::kImpossible;
  }
  closeUpload();
  std::array<char, 512> part{};
  if (!path(part, number, ".part")) {
    return answer::kStorageBench;
  }
  int fd = ::open(part.data(), O_RDWR | O_CREAT | O_CLOEXEC, kFileMode);
  if (fd < 0) {
    logging::line("store: cannot open %s: %s", part.data(), std::strerror(errno));
    return answer::kStorageBench;
  }

  // Keep what a .part file of the same declared length holds, in whole chunks, if the hash of
  // its header still matches its first 64 bytes.
  struct stat info{};
  std::array<uint8_t, kPartHeadBytes> head{};
  bool keep = ::fstat(fd, &info) == 0 && info.st_size >= kPartHeadBytes &&
              readAll(fd, head.data(), head.size(), 0) &&
              std::memcmp(head.data(), kPartTag.data(), kPartTag.size()) == 0 &&
              bytes::readU32(&head[kPartLengthAt]) == length &&
              info.st_size - kPartHeadBytes <= static_cast<off_t>(length);
  uint32_t kept = 0;
  std::array<uint8_t, kHashBytes> hash{};
  if (keep) {
    size_t bytes_held = static_cast<size_t>(info.st_size - kPartHeadBytes);
    kept = static_cast<uint32_t>((bytes_held / config::kChunkBytes) * config::kChunkBytes);
    if (kept >= kHeaderBytes) {
      std::array<uint8_t, kHeaderBytes> first{};
      std::array<uint8_t, kHashBytes> now{};
      keep = readAll(fd, first.data(), first.size(), kPartHeadBytes) &&
             sha256(first.data(), first.size(), now) &&
             std::memcmp(now.data(), &head[kPartHashAt], kHashBytes) == 0;
      hash = now;
    }
    if (!keep) {
      logging::line("store: %s does not match its header hash; started afresh", part.data());
    }
  }
  if (!keep) {
    kept = 0;
    hash = {};
  }
  std::memcpy(head.data(), kPartTag.data(), kPartTag.size());
  bytes::writeU32(&head[kPartLengthAt], length);
  std::memcpy(&head[kPartHashAt], hash.data(), kHashBytes);
  bool ok = ::ftruncate(fd, kPartHeadBytes + static_cast<off_t>(kept)) == 0 &&
            writeAll(fd, head.data(), head.size(), 0) && ::fsync(fd) == 0;
  if (!ok) {
    logging::line("store: cannot prepare %s: %s", part.data(), std::strerror(errno));
    (void)::close(fd);
    return answer::kStorageBench;
  }
  if (!sync_(dir_)) {
    (void)::close(fd);
    return answer::kStorageBench;
  }

  fd_ = fd;
  number_ = number;
  length_ = length;
  stored_ = kept;
  hash_ = hash;
  have_hash_ = kept >= kHeaderBytes;
  resume_at = kept;
  return answer::kSuccess;
}

/**
 * @brief Stores one chunk of the open upload at the next offset, or accepts a resend of a stored chunk.
 *
 * Each returns 0x00 for DONE, or the refusal reason (protocol.hpp).
 * A chunk wholly inside the stored bytes is answered DONE without writing (a resend), except a
 * chunk at offset 0 whose header differs from the one stored: that is another image with the same
 * number and length, so the stored bytes are dropped and the chunk is written as the start of a new
 * upload (audit finding #15; the header is compared by its SHA-256 for 64 bytes or more, by the
 * bytes themselves for a shorter chunk). A chunk past
 * the bytes stored, across their end, or past the declared length, a size of 0 or over kChunkBytes,
 * or another image number is refused (answer::kImpossible). The chunk is flushed to disk before it
 * counts as stored.
 *
 * @param[in] number  image number; must be the open upload
 * @param[in] offset  byte offset of the chunk in the image, in bytes
 * @param[in] data  chunk bytes; size bytes
 * @param[in] size  chunk length, in bytes
 * @param[out] written  true if the chunk was written, false for a resend or a refusal; set to false on entry
 * @return answer::kSuccess (0x00): stored or already stored; otherwise the refusal reason
 */
uint8_t UploadStore::chunk(uint16_t number, uint32_t offset, const uint8_t * data, size_t size,
  bool & written)
{
  written = false;
  if (fd_ < 0 || number != number_ || size == 0 || size > config::kChunkBytes) {
    return answer::kImpossible;
  }
  uint64_t chunk_end = static_cast<uint64_t>(offset) + size;
  if (chunk_end <= stored_) {
    if (offset != 0 || sameHeader(data, size)) {
      return answer::kSuccess;   // a resend of a chunk already stored
    }
    // Another image under the same number and length: start over.
    logging::line("store: image %u: the first chunk differs from the stored one; the %u stored "
      "bytes are dropped", number, stored_);
    if (::ftruncate(fd_, kPartHeadBytes) != 0) {
      logging::line("store: cannot cut %u back: %s", number, std::strerror(errno));
      return answer::kStorageBench;
    }
    stored_ = 0;
    have_hash_ = false;
    chunk_end = size;
  }
  if (offset != stored_ || chunk_end > length_) {
    return answer::kImpossible;   // past the bytes stored, across their end, or past the image
  }
  if (!writeAll(fd_, data, size, kPartHeadBytes + static_cast<off_t>(offset)) ||
      ::fsync(fd_) != 0) {
    logging::line("store: cannot write image %u at %u: %s", number, offset, std::strerror(errno));
    return answer::kStorageBench;
  }
  stored_ = static_cast<uint32_t>(chunk_end);
  if (!have_hash_ && stored_ >= kHeaderBytes && !recordHeaderHash()) {
    return answer::kStorageBench;
  }
  written = true;
  return answer::kSuccess;
}

/**
 * @brief Tells whether a chunk at offset 0 carries the same header as the stored bytes.
 *
 * With 64 bytes or more the SHA-256 of the first 64 is compared with the hash in the .part file;
 * with fewer, the bytes themselves are compared with the stored ones (the hash needs 64).
 *
 * @param[in]     data  the chunk's bytes
 * @param[in]     size  the chunk's length, bytes
 * @return true: same header (or not enough to tell, never the case here); false: it differs
 */
bool UploadStore::sameHeader(const uint8_t * data, size_t size) const
{
  if (size >= kHeaderBytes && have_hash_) {
    std::array<uint8_t, kHashBytes> now{};
    return sha256(data, kHeaderBytes, now) && now == hash_;
  }
  std::array<uint8_t, kHeaderBytes> stored{};
  size_t n = (size < kHeaderBytes) ? size : kHeaderBytes;
  return readAll(fd_, stored.data(), n, kPartHeadBytes) && std::memcmp(stored.data(), data, n) == 0;
}

/**
 * @brief Writes the SHA-256 of the stored image's first 64 bytes into the .part file head.
 *
 * Done once the stored bytes reach 64 (the header is complete), so a later BEGIN can check that
 * the bytes still match and a later first chunk can be compared (audit finding #15).
 *
 * @return true: hash computed, written and flushed; false: a read, write or flush failed (logged)
 */
bool UploadStore::recordHeaderHash()
{
  std::array<uint8_t, kHeaderBytes> first{};
  std::array<uint8_t, kHashBytes> hash{};
  if (!readAll(fd_, first.data(), first.size(), kPartHeadBytes) ||
      !sha256(first.data(), first.size(), hash) ||
      !writeAll(fd_, hash.data(), hash.size(), kPartHashAt) || ::fsync(fd_) != 0) {
    logging::line("store: cannot record the header hash: %s", std::strerror(errno));
    return false;
  }
  hash_ = hash;
  have_hash_ = true;
  return true;
}

/**
 * @brief Ends the open upload: reads the image back, runs the checks, and stores it as image-n.img.
 *
 * Each returns 0x00 for DONE, or the refusal reason (protocol.hpp).
 * An incomplete image is refused and kept, so the ground can send the missing chunks. An image the
 * checker refuses is removed; the next BEGIN starts it afresh (REQ-052). The .part file is removed
 * once the .img is written.
 *
 * @param[in] number  image number; must be the open upload
 * @param[in] checker  the image checks
 * @param[out] verdict  result of the checks: code, reason, kind, version
 * @return answer::kSuccess (0x00): the image passed and is stored; otherwise the refusal reason
 */
uint8_t UploadStore::end(uint16_t number, const ImageChecker & checker, ImageVerdict & verdict)
{
  verdict = ImageVerdict{answer::kImpossible, "no open upload of this image", 0, 0};
  if (fd_ < 0 || number != number_) {
    return answer::kImpossible;
  }
  if (stored_ != length_) {
    verdict.reason = "image incomplete";
    return answer::kImpossible;   // kept: the ground can still send the missing chunks
  }
  if (!readAll(fd_, image_.data(), length_, kPartHeadBytes)) {
    logging::line("store: cannot read image %u back: %s", number, std::strerror(errno));
    verdict = ImageVerdict{answer::kStorageBench, "storage", 0, 0};
    return answer::kStorageBench;
  }

  std::array<char, 512> part{};
  (void)path(part, number, ".part");   // same form as at BEGIN, which succeeded
  verdict = checker.check(image_.data(), length_, limits_);
  if (verdict.code != answer::kSuccess) {
    // A refused image is of no use; the next BEGIN starts it afresh (REQ-052).
    closeUpload();
    removeFile(part.data());
    return verdict.code;
  }
  if (!writeImage(number, image_.data(), length_)) {
    verdict = ImageVerdict{answer::kStorageBench, "storage", 0, 0};
    return answer::kStorageBench;
  }
  // The rename must be on storage before the .part file, the only other copy, is deleted
  // (REQ-063; audit finding #16). If the flush fails the .part stays.
  if (!sync_(dir_)) {
    verdict = ImageVerdict{answer::kStorageBench, "storage", 0, 0};
    return answer::kStorageBench;
  }
  closeUpload();
  removeFile(part.data());
  if (!sync_(dir_)) {
    logging::line("store: image %u is stored, but the deletion of its .part file is not flushed",
      number);
  }
  return answer::kSuccess;
}

/**
 * @brief Removes the stored bytes of an image number: its .part and its .img files.
 *
 * If that image is the open upload, it is closed first.
 * The directory is flushed afterwards.
 *
 * @param[in] number  image number
 * @return answer::kSuccess (0x00), always
 */
uint8_t UploadStore::cancel(uint16_t number)
{
  if (fd_ >= 0 && number == number_) {
    closeUpload();
  }
  std::array<char, 512> name{};
  if (path(name, number, ".part")) {
    removeFile(name.data());
  }
  if (path(name, number, ".img")) {
    removeFile(name.data());
  }
  if (!sync_(dir_)) {
    logging::line("store: image %u removed, but the removal is not flushed", number);
  }
  return answer::kSuccess;
}

/**
 * @brief Removes the stored image-n.img of an image number; leaves its .part file alone.
 *
 * Owner decision 22 (15 Oct 2026, REQ-052): once the controller reports its firmware confirmed
 * (EVENT 0x0007) or given up (0x0008) after a delivery, the delivered image is of no further use
 * and its storage is freed. Only the .img goes: a .part file belongs to an upload in progress
 * (possibly of the same image number) and is not this function's to delete. The directory is
 * flushed afterwards; a flush that fails is logged and the removal still counts as done.
 *
 * @param[in] number  image number
 * @return true: the .img existed and was removed; false: there was no such file, or it could not
 *         be removed (logged)
 */
bool UploadStore::removeImage(uint16_t number)
{
  std::array<char, 512> name{};
  if (!path(name, number, ".img")) {
    return false;
  }
  if (::unlink(name.data()) != 0) {
    if (errno != ENOENT) {
      logging::line("store: cannot remove %s: %s", name.data(), std::strerror(errno));
    }
    return false;
  }
  if (!sync_(dir_)) {
    logging::line("store: image %u removed, but the removal is not flushed", number);
  }
  return true;
}

/**
 * @brief Gives the data directory the store was made with.
 *
 * The limits record is kept in the same directory (durable_record.hpp).
 *
 * @return the directory path; valid as long as the string given to the constructor
 */
const char * UploadStore::directory() const
{
  return dir_;
}

/**
 * @brief Sets the limits UPLOAD END applies from now on.
 *
 * Called by the updater after loading the limits record and after a SET LIMITS (owner decision 24,
 * 15 Oct 2026). Images already stored are not touched; DELIVER checks them again against the
 * limits then in force (REQ-047).
 *
 * @param[in] limits  the minimum version, the controller identity and whether they are usable
 * @return nothing
 */
void UploadStore::setLimits(const ImageLimits & limits)
{
  limits_ = limits;
}

/**
 * @brief Gives the limits UPLOAD END applies.
 *
 * @return the limits in force (defaultImageLimits() until setLimits() is called)
 */
const ImageLimits & UploadStore::limits() const
{
  return limits_;
}

/**
 * @brief Writes a whole image as image-n.img.
 *
 * Written to a temporary file first, then renamed: a power cut leaves either the old .img or
 * the new one, never half of one.
 * The temporary file is image-n.img.tmp; it is removed if the write or the rename fails.
 *
 * @param[in] number  image number
 * @param[in] image  image bytes; size bytes
 * @param[in] size  image length, in bytes
 * @return true: the .img file is in place; false: the write or the rename failed
 */
bool UploadStore::writeImage(uint16_t number, const uint8_t * image, size_t size)
{
  std::array<char, 512> temporary{};
  std::array<char, 512> final_name{};
  if (!path(temporary, number, ".img.tmp") || !path(final_name, number, ".img")) {
    return false;
  }
  int fd = ::open(temporary.data(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kFileMode);
  if (fd < 0) {
    logging::line("store: cannot create %s: %s", temporary.data(), std::strerror(errno));
    return false;
  }
  bool ok = writeAll(fd, image, size, 0) && ::fsync(fd) == 0;
  (void)::close(fd);
  if (!ok || ::rename(temporary.data(), final_name.data()) != 0) {
    logging::line("store: cannot write %s: %s", final_name.data(), std::strerror(errno));
    removeFile(temporary.data());
    return false;
  }
  return true;
}

/**
 * @brief Reads a stored image into a caller's buffer.
 *
 * A whole image that passed its checks at UPLOAD END, read into out. False if there is none.
 * Also false if the file is empty or larger than room.
 *
 * @param[in] number  image number
 * @param[out] out  buffer that receives the image; room bytes
 * @param[in] room  size of out, in bytes
 * @param[out] size  image length read, in bytes; 0 if none
 * @return true: the image is in out; false: there is none, or it does not fit
 */
bool UploadStore::load(uint16_t number, uint8_t * out, size_t room, size_t & size) const
{
  size = 0;
  std::array<char, 512> name{};
  if (!path(name, number, ".img")) {
    return false;
  }
  int fd = ::open(name.data(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  struct stat info{};
  bool ok = ::fstat(fd, &info) == 0 && info.st_size > 0 &&
            static_cast<size_t>(info.st_size) <= room &&
            readAll(fd, out, static_cast<size_t>(info.st_size), 0);
  (void)::close(fd);
  if (ok) {
    size = static_cast<size_t>(info.st_size);
  }
  return ok;
}

/**
 * @brief Makes a delivery in the idle stage.
 *
 * No image is held and nothing is sent until start() is called.
 *
 * @return nothing (constructor)
 */
Delivery::Delivery()
: stage_(Stage::kIdle), number_(0), size_(0), offset_(0), ground_sequence_(0), expiry_(0),
  image_{}
{
}

/**
 * @brief Tells whether a delivery is running.
 *
 * The delivery is running from start() until stop() or its last answer.
 *
 * @return true: a delivery is running; false: idle
 */
bool Delivery::active() const
{
  return stage_ != Stage::kIdle;
}

/**
 * @brief Gives the buffer that holds the image to deliver.
 *
 * DELIVER loads the image here, checks it, then calls start().
 *
 * @return pointer to the image buffer; capacity() bytes
 */
uint8_t * Delivery::buffer()
{
  return image_.data();
}

/**
 * @brief Gives the size of the image buffer.
 *
 * The largest image buffer() can hold: config::kMaxImageBytes.
 *
 * @return buffer size, in bytes
 */
size_t Delivery::capacity() const
{
  return image_.size();
}

/**
 * @brief Starts a delivery of the image now in buffer(), at the UPLOAD BEGIN step.
 *
 * Under the same image number the ground used. The ground sequence number and the expiry are kept
 * for the answer to the ground and for the frames of the delivery.
 *
 * @param[in] image_number  image number
 * @param[in] image_size  image length, in bytes
 * @param[in] ground_sequence  sequence number of the ground DELIVER command
 * @param[in] command_expiry  expiry of the DELIVER command, in the units of the frame expiry field
 * @return nothing
 */
void Delivery::start(uint16_t image_number, size_t image_size, uint32_t ground_sequence,
  uint32_t command_expiry)
{
  stage_ = Stage::kBegin;
  number_ = image_number;
  size_ = image_size;
  offset_ = 0;
  ground_sequence_ = ground_sequence;
  expiry_ = command_expiry;
}

/**
 * @brief Builds the command to send now: UPLOAD BEGIN, UPLOAD CHUNK or UPLOAD END, by stage.
 *
 * The command to send now (payload, its length, its timeout). Only while active().
 * A chunk is kChunkBytes, or what is left of the image. UPLOAD BEGIN gets kBeginTimeoutMs; the
 * others kAnswerTimeoutMs.
 *
 * @param[out] payload  receives the command payload
 * @param[in] room  size of payload, in bytes
 * @param[out] timeout_ms  how long to wait for the last answer, in ms; set on entry to kAnswerTimeoutMs
 * @return payload length, in bytes; 0 if idle or if room is too small
 */
uint16_t Delivery::next(uint8_t * payload, size_t room, uint32_t & timeout_ms) const
{
  timeout_ms = config::kAnswerTimeoutMs;
  switch (stage_) {
    case Stage::kBegin:
      if (room < command::kUploadBeginBytes) {
        return 0;
      }
      payload[0] = command::kUploadBegin;
      bytes::writeU16(&payload[1], number_);
      bytes::writeU32(&payload[3], static_cast<uint32_t>(size_));
      timeout_ms = config::kBeginTimeoutMs;
      return static_cast<uint16_t>(command::kUploadBeginBytes);
    case Stage::kChunks: {
      size_t chunk = (size_ - offset_ < config::kChunkBytes) ? size_ - offset_ : config::kChunkBytes;
      if (room < command::kUploadChunkHeadBytes + chunk) {
        return 0;
      }
      payload[0] = command::kUploadChunk;
      bytes::writeU16(&payload[1], number_);
      bytes::writeU32(&payload[3], static_cast<uint32_t>(offset_));
      std::memcpy(&payload[command::kUploadChunkHeadBytes], &image_[offset_], chunk);
      return static_cast<uint16_t>(command::kUploadChunkHeadBytes + chunk);
    }
    case Stage::kEnd:
      if (room < command::kImageNumberOnlyBytes) {
        return 0;
      }
      payload[0] = command::kUploadEnd;
      bytes::writeU16(&payload[1], number_);
      return static_cast<uint16_t>(command::kImageNumberOnlyBytes);
    case Stage::kIdle:
      break;
  }
  return 0;
}

/**
 * @brief Takes the last answer to the command from next() and moves to the next stage.
 *
 * The last answer to the command from next(): DONE moves on; REFUSED stops with the controller's
 * code; ALREADY DONE stops with 0x04, because the updater never resends a step.
 *
 * @param[in] kind  answer kind: DONE, ALREADY DONE or REFUSED
 * @param[in] answer_code  the answer code
 * @param[out] code  the refusal reason when REFUSED, else answer::kSuccess
 * @return Step::kContinue: send next(); Step::kDone: the controller accepted UPLOAD END; Step::kRefused: the controller refused a step
 */
Delivery::Step Delivery::onFinalAnswer(uint8_t kind, uint8_t answer_code, uint8_t & code)
{
  code = answer::kSuccess;
  if (kind == answer::kRefused) {
    code = answer_code;
    stop();
    return Step::kRefused;
  }
  if (kind == answer::kAlreadyDone) {
    // The updater never sends a step twice, so ALREADY DONE can only mean that the sequence
    // number clashed with an earlier command the controller already ran: the step was not
    // done by this delivery. Treated as a refusal, 0x04 "Old command sent again" (REQ-065;
    // audit finding: ALREADY DONE counted as success). Open: keeping the sequence counter
    // across restarts comes with REQ-065.
    code = answer::kOldCommand;
    stop();
    return Step::kRefused;
  }
  // DONE: the controller carried the step out.
  switch (stage_) {
    case Stage::kBegin:
      offset_ = 0;
      stage_ = Stage::kChunks;
      return Step::kContinue;
    case Stage::kChunks: {
      size_t chunk = (size_ - offset_ < config::kChunkBytes) ? size_ - offset_ : config::kChunkBytes;
      offset_ += chunk;
      if (offset_ >= size_) {
        stage_ = Stage::kEnd;
      }
      return Step::kContinue;
    }
    case Stage::kEnd:
      stop();
      return Step::kDone;
    case Stage::kIdle:
      break;
  }
  return Step::kContinue;
}

/**
 * @brief Ends the delivery and returns to idle.
 *
 * The image buffer is not cleared.
 *
 * @return nothing
 */
void Delivery::stop()
{
  stage_ = Stage::kIdle;
}

/**
 * @brief Gives the image number of the delivery.
 *
 * Kept from start().
 *
 * @return image number
 */
uint16_t Delivery::number() const
{
  return number_;
}

/**
 * @brief Gives the sequence number of the ground DELIVER command.
 *
 * Kept from start(); the answer to the ground carries it.
 *
 * @return ground sequence number
 */
uint32_t Delivery::groundSequence() const
{
  return ground_sequence_;
}

/**
 * @brief Gives the expiry of the DELIVER command.
 *
 * Kept from start().
 *
 * @return command expiry, as given to start()
 */
uint32_t Delivery::expiry() const
{
  return expiry_;
}

/**
 * @brief Gives the image length of the delivery.
 *
 * Kept from start().
 *
 * @return image length, in bytes
 */
size_t Delivery::size() const
{
  return size_;
}

/**
 * @brief Gives the body bytes the controller has acknowledged.
 *
 * body bytes the controller has acknowledged.
 *
 * @return acknowledged image bytes, in bytes
 */
size_t Delivery::sent() const
{
  return offset_;
}
