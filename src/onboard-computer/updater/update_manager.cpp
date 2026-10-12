#include "update_manager.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bytes.hpp"
#include "log.hpp"
#include "protocol.hpp"

namespace
{

// The .part file starts with the declared image length (4 bytes); the image bytes follow.
constexpr off_t kPartHeadBytes = 4;
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
 * @brief Flushes a directory to storage.
 *
 * Flushes a directory, so a rename or a new file in it survives a power cut (REQ-063).
 * If the directory cannot be opened, nothing is done.
 *
 * @param[in] dir  path of the directory
 * @return nothing
 */
void syncDir(const char * dir)
{
  int fd = ::open(dir, O_RDONLY);
  if (fd >= 0) {
    (void)::fsync(fd);
    (void)::close(fd);
  }
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
 * @brief Makes a store for the data directory dir, with no upload open.
 *
 * dir: the data directory; it is made, with its parents, by ready().
 * The pointer is kept, not copied: the string must outlive the store.
 *
 * @param[in] dir  path of the data directory
 * @return nothing (constructor)
 */
UploadStore::UploadStore(const char * dir)
: dir_(dir), fd_(-1), number_(0), length_(0), stored_(0), image_{}
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
 * upload is closed first; a storage failure is answer::kNoAnswerBench.
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
    return answer::kNoAnswerBench;
  }
  int fd = ::open(part.data(), O_RDWR | O_CREAT, kFileMode);
  if (fd < 0) {
    logging::line("store: cannot open %s: %s", part.data(), std::strerror(errno));
    return answer::kNoAnswerBench;
  }

  // Keep what a .part file of the same declared length holds, in whole chunks.
  struct stat info{};
  std::array<uint8_t, kPartHeadBytes> head{};
  bool keep = ::fstat(fd, &info) == 0 && info.st_size >= kPartHeadBytes &&
              readAll(fd, head.data(), head.size(), 0) && bytes::readU32(head.data()) == length &&
              info.st_size - kPartHeadBytes <= static_cast<off_t>(length);
  uint32_t kept = 0;
  if (keep) {
    size_t bytes_held = static_cast<size_t>(info.st_size - kPartHeadBytes);
    kept = static_cast<uint32_t>((bytes_held / config::kChunkBytes) * config::kChunkBytes);
  } else {
    bytes::writeU32(head.data(), length);
  }
  bool ok = ::ftruncate(fd, kPartHeadBytes + static_cast<off_t>(kept)) == 0 &&
            (keep || writeAll(fd, head.data(), head.size(), 0)) && ::fsync(fd) == 0;
  if (!ok) {
    logging::line("store: cannot prepare %s: %s", part.data(), std::strerror(errno));
    (void)::close(fd);
    return answer::kNoAnswerBench;
  }
  syncDir(dir_);

  fd_ = fd;
  number_ = number;
  length_ = length;
  stored_ = kept;
  resume_at = kept;
  return answer::kSuccess;
}

/**
 * @brief Stores one chunk of the open upload at the next offset, or accepts a resend of a stored chunk.
 *
 * Each returns 0x00 for DONE, or the refusal reason (protocol.hpp).
 * A chunk wholly inside the stored bytes is answered DONE without writing (a resend). A chunk past
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
    return answer::kSuccess;   // a resend of a chunk already stored
  }
  if (offset != stored_ || chunk_end > length_) {
    return answer::kImpossible;   // past the bytes stored, across their end, or past the image
  }
  if (!writeAll(fd_, data, size, kPartHeadBytes + static_cast<off_t>(offset)) ||
      ::fsync(fd_) != 0) {
    logging::line("store: cannot write image %u at %u: %s", number, offset, std::strerror(errno));
    return answer::kNoAnswerBench;
  }
  stored_ = static_cast<uint32_t>(chunk_end);
  written = true;
  return answer::kSuccess;
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
    verdict = ImageVerdict{answer::kNoAnswerBench, "storage", 0, 0};
    return answer::kNoAnswerBench;
  }

  std::array<char, 512> part{};
  (void)path(part, number, ".part");   // same form as at BEGIN, which succeeded
  verdict = checker.check(image_.data(), length_);
  if (verdict.code != answer::kSuccess) {
    // A refused image is of no use; the next BEGIN starts it afresh (REQ-052).
    closeUpload();
    removeFile(part.data());
    return verdict.code;
  }
  if (!writeImage(number, image_.data(), length_)) {
    verdict = ImageVerdict{answer::kNoAnswerBench, "storage", 0, 0};
    return answer::kNoAnswerBench;
  }
  closeUpload();
  removeFile(part.data());
  syncDir(dir_);
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
  syncDir(dir_);
  return answer::kSuccess;
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
  int fd = ::open(temporary.data(), O_WRONLY | O_CREAT | O_TRUNC, kFileMode);
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
  int fd = ::open(name.data(), O_RDONLY);
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
 * The last answer to the command from next(): DONE or ALREADY DONE moves on, REFUSED stops
 * with the controller's code.
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
  // DONE, or ALREADY DONE: the controller has carried the command out before.
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
