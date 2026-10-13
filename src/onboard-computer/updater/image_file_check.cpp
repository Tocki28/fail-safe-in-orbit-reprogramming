#include "image_file_check.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bytes.hpp"
#include "config.hpp"
#include "protocol.hpp"

namespace
{

/**
 * @brief Gives the name of an image kind for the result line.
 *
 * One case per kind of image_format.h; a value outside the three reads "unknown".
 *
 * @param[in]     kind  kind byte of the image header
 * @return text of the name; never null
 */
const char * kindName(uint8_t kind)
{
  switch (kind) {
    case IMAGE_KIND_BOOTLOADER:  return "bootloader";
    case IMAGE_KIND_MAIN:        return "main";
    case IMAGE_KIND_LAST_RESORT: return "last-resort";
  }
  return "unknown";   // a value outside the three, handled after the switch
}

/**
 * @brief Gives the number of bytes of the flash region an image of this kind is written to.
 *
 * What the restore script writes is: the whole image for main (slot A) and last-resort, and the
 * body only for the bootloader (Interfaces Sec. 5.5, every kind has the same layout; the
 * bootloader's body is the program exactly as linked at 0x08000000). So for the bootloader the
 * limit is on the body, for the others on the whole file (layout.h).
 *
 * @param[in]     kind  kind byte of the image header
 * @return region size in bytes; 0 for a kind outside the three
 */
size_t regionBytes(uint8_t kind)
{
  switch (kind) {
    case IMAGE_KIND_BOOTLOADER:  return static_cast<size_t>(BOOTLOADER_BYTES);
    case IMAGE_KIND_MAIN:        return static_cast<size_t>(SLOT_BYTES);
    case IMAGE_KIND_LAST_RESORT: return static_cast<size_t>(LAST_RESORT_BYTES);
  }
  return 0;
}

/**
 * @brief Writes a result line into the caller's buffer.
 *
 * Cut at the buffer size, always zero-terminated; a zero-size buffer is left alone.
 *
 * @param[out]    line        receives the text
 * @param[in]     line_bytes  size of line, bytes
 * @param[in]     text        the text to copy
 * @return nothing
 */
void setLine(char * line, size_t line_bytes, const char * text)
{
  if (line_bytes == 0) {
    return;
  }
  (void)std::snprintf(line, line_bytes, "%s", text);
}

/**
 * @brief Reads a whole file of at most config::kMaxImageBytes into a buffer.
 *
 * Opened with O_CLOEXEC; a file larger than the buffer is not read (the image checker would
 * refuse its size anyway). Retries after EINTR; every read is bounded by the file size.
 *
 * @param[in]     path     file to read
 * @param[out]    buffer   receives the file
 * @param[in]     room     size of buffer, bytes
 * @param[out]    size     bytes read
 * @param[out]    reason   text for the result line when false is returned
 * @return true: the whole file is in buffer; false: it cannot be opened, read, or is too large
 */
bool readImageFile(const char * path, uint8_t * buffer, size_t room, size_t & size,
  const char *& reason)
{
  size = 0;
  int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    reason = "cannot open the file";
    return false;
  }
  struct stat info{};
  if (::fstat(fd, &info) != 0 || (info.st_mode & S_IFMT) != S_IFREG || info.st_size < 0) {
    (void)::close(fd);
    reason = "not a regular file";
    return false;
  }
  if (static_cast<uint64_t>(info.st_size) > room) {
    (void)::close(fd);
    reason = "file larger than any image";
    return false;
  }
  size_t want = static_cast<size_t>(info.st_size);
  while (size < want) {
    ssize_t n = ::read(fd, buffer + size, want - size);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      (void)::close(fd);
      reason = "cannot read the file";
      return false;
    }
    size += static_cast<size_t>(n);
  }
  (void)::close(fd);
  return true;
}

}  // namespace

/**
 * @brief Turns the word after --kind into the kind byte.
 *
 * The three words are the ones tools/make_image.py takes for --kind (firmware_layout.KINDS).
 *
 * @param[in]     name  "bootloader", "main" or "last-resort"; may be nullptr
 * @param[out]    kind  kind byte; set only when true is returned
 * @return true: a known word; false: any other text
 */
bool parseKindName(const char * name, uint8_t & kind)
{
  if (name == nullptr) {
    return false;
  }
  if (std::strcmp(name, "bootloader") == 0) {
    kind = static_cast<uint8_t>(IMAGE_KIND_BOOTLOADER);
    return true;
  }
  if (std::strcmp(name, "main") == 0) {
    kind = static_cast<uint8_t>(IMAGE_KIND_MAIN);
    return true;
  }
  if (std::strcmp(name, "last-resort") == 0) {
    kind = static_cast<uint8_t>(IMAGE_KIND_LAST_RESORT);
    return true;
  }
  return false;
}

/**
 * @brief Runs the upload's image checks on a file and requires the kind asked for.
 *
 * REQ-118: the restore script must write only images that passed the checks. This is the
 * ImageChecker of an upload, unchanged (both signatures, damage, target, kind, length), then
 * two more tests the restore needs: the header kind equals the kind asked for, and the image
 * fits the flash region of that kind (the body for the bootloader, whose body alone is written
 * at 0x08000000; the whole file for the others). The line starts "image check passed" or
 * "image check FAILED" and names the reason, so the script can print it.
 *
 * @param[in]     path        file to check
 * @param[in]     kind        kind the caller expects (IMAGE_KIND_...)
 * @param[in]     checker     the image checker with the two public keys
 * @param[out]    line        one line of text, no newline
 * @param[in]     line_bytes  size of line, bytes
 * @return true: the image passed every check and is of the kind asked for; false: otherwise
 */
bool checkImageFile(const char * path, uint8_t kind, const ImageChecker & checker, char * line,
  size_t line_bytes)
{
  static std::array<uint8_t, config::kMaxImageBytes> image;   // 40 KB: kept off the stack
  size_t size = 0;
  const char * reason = "";
  std::array<char, 256> text{};

  if (!readImageFile(path, image.data(), image.size(), size, reason)) {
    (void)std::snprintf(text.data(), text.size(), "image check FAILED: %s: %s", path, reason);
    setLine(line, line_bytes, text.data());
    return false;
  }
  ImageVerdict verdict = checker.check(image.data(), size);
  if (verdict.code != answer::kSuccess) {
    (void)std::snprintf(text.data(), text.size(), "image check FAILED: %s: %s (refusal 0x%02x)",
      path, verdict.reason, verdict.code);
    setLine(line, line_bytes, text.data());
    return false;
  }
  if (verdict.kind != kind) {
    (void)std::snprintf(text.data(), text.size(),
      "image check FAILED: %s: kind is %s, expected %s", path, kindName(verdict.kind),
      kindName(kind));
    setLine(line, line_bytes, text.data());
    return false;
  }
  size_t body = static_cast<size_t>(bytes::readU32(&image[IMAGE_OFFSET_LENGTH]));
  size_t written = (kind == IMAGE_KIND_BOOTLOADER) ? body : size;
  if (written > regionBytes(kind)) {
    (void)std::snprintf(text.data(), text.size(),
      "image check FAILED: %s: %zu bytes do not fit the %s region of %zu bytes", path, written,
      kindName(kind), regionBytes(kind));
    setLine(line, line_bytes, text.data());
    return false;
  }
  (void)std::snprintf(text.data(), text.size(),
    "image check passed: %s: kind %s, version %u, %zu bytes, both signatures valid", path,
    kindName(kind), verdict.version, size);
  setLine(line, line_bytes, text.data());
  return true;
}
