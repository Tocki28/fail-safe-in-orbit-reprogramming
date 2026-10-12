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

// Reads or writes all size bytes at offset; false on any shortfall.
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

// Flushes a directory, so a rename or a new file in it survives a power cut (REQ-063).
void syncDir(const char * dir)
{
  int fd = ::open(dir, O_RDONLY);
  if (fd >= 0) {
    (void)::fsync(fd);
    (void)::close(fd);
  }
}

void removeFile(const char * path)
{
  if (::unlink(path) != 0 && errno != ENOENT) {
    logging::line("store: cannot remove %s: %s", path, std::strerror(errno));
  }
}

}  // namespace

UploadStore::UploadStore(const char * dir)
: dir_(dir), fd_(-1), number_(0), length_(0), stored_(0), image_{}
{
}

UploadStore::~UploadStore()
{
  closeUpload();
}

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

bool UploadStore::path(std::array<char, 512> & out, uint16_t number, const char * suffix) const
{
  int n = std::snprintf(out.data(), out.size(), "%s/image-%05u%s", dir_,
    static_cast<unsigned>(number), suffix);
  return n > 0 && static_cast<size_t>(n) < out.size();
}

void UploadStore::closeUpload()
{
  if (fd_ >= 0) {
    (void)::close(fd_);
    fd_ = -1;
  }
  stored_ = 0;
}

bool UploadStore::open() const
{
  return fd_ >= 0;
}

uint32_t UploadStore::stored() const
{
  return stored_;
}

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

// Written to a temporary file first, then renamed: a power cut leaves either the old .img or
// the new one, never half of one.
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

Delivery::Delivery()
: stage_(Stage::kIdle), number_(0), size_(0), offset_(0), ground_sequence_(0), expiry_(0),
  image_{}
{
}

bool Delivery::active() const
{
  return stage_ != Stage::kIdle;
}

uint8_t * Delivery::buffer()
{
  return image_.data();
}

size_t Delivery::capacity() const
{
  return image_.size();
}

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

void Delivery::stop()
{
  stage_ = Stage::kIdle;
}

uint16_t Delivery::number() const
{
  return number_;
}

uint32_t Delivery::groundSequence() const
{
  return ground_sequence_;
}

uint32_t Delivery::expiry() const
{
  return expiry_;
}

size_t Delivery::size() const
{
  return size_;
}

size_t Delivery::sent() const
{
  return offset_;
}
