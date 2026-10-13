/*
 * update_manager.hpp — the update manager (Architecture Sec. 6.0): stores new firmware as it
 * arrives from the ground, checks it, and delivers it to the controller over data link A
 * (Interfaces Sec. 5.5, Figure 7, steps 2-4).
 *
 * UploadStore — the ground's UPLOAD BEGIN, CHUNK, END and CANCEL (Interfaces Sec. 5.1).
 *   Files in the data directory, one set per image number n:
 *     image-n.part  an upload in progress: a head of 40 bytes (tag "PRT2", the declared image
 *                   length, 4 bytes little-endian, and the SHA-256 of the image's first 64 bytes,
 *                   zero until 64 bytes are stored), then the bytes stored so far
 *     image-n.img   a whole image that passed its checks at UPLOAD END; DELIVER reads it
 *   Resume (REQ-049, REQ-063), kept simple:
 *     - UPLOAD BEGIN for an image number and length that match a .part file keeps the bytes
 *       stored, cut back to a whole number of 256-byte chunks (a chunk half written when the
 *       power went is dropped). Any other BEGIN starts that image number afresh.
 *     - UPLOAD CHUNK at offset = bytes stored: written, answered DONE.
 *       UPLOAD CHUNK wholly inside the bytes stored: answered DONE without writing; it is a
 *       resend, e.g. after its DONE was lost.
 *       UPLOAD CHUNK past the bytes stored, or across their end: REFUSED 0x06.
 *     So a ground that kept its DONE answers resumes at the first chunk without one; a ground
 *     that kept nothing sends BEGIN and starts again at offset 0, and loses only link time on
 *     the chunks already stored. The answer payload (6 bytes) has no room for the offset.
 *     A resent chunk is not compared with what is stored, except the chunk at offset 0: if its
 *     header differs from the stored one it is another image under the same number and length,
 *     and the stored bytes are dropped. A resumed upload that never resends chunk 0 is not
 *     compared; if it differed, the image fails its CRC-32 or a signature at UPLOAD END, and the
 *     upload starts again.
 *   One upload is open at a time (the last BEGIN); CHUNK and END must name its image number.
 *
 * Delivery — DELIVER sends a stored image to the controller (Destination 0x02) as UPLOAD BEGIN,
 *   UPLOAD CHUNKs of 256 bytes and UPLOAD END, stop-and-wait (data_link.hpp), under the same
 *   image number the ground used.
 *
 * BENCH LIMITS: CANCEL does not stop a delivery already running (REQ-051 partly); storage is
 * freed by CANCEL only, not when an update ends (REQ-052 partly); no minimum version (REQ-045).
 */
#ifndef FSIR_UPDATER_UPDATE_MANAGER_HPP
#define FSIR_UPDATER_UPDATE_MANAGER_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "config.hpp"
#include "image_checker.hpp"

// Flushes a directory to storage; false if opening or flushing failed (REQ-063).
[[nodiscard]] bool syncDirectory(const char * dir);

class UploadStore
{
public:
  using DirectorySync = bool (*)(const char * dir);

  // dir: the data directory; it is made, with its parents, by ready().
  explicit UploadStore(const char * dir);
  ~UploadStore();
  UploadStore(const UploadStore &) = delete;
  UploadStore & operator=(const UploadStore &) = delete;

  // The directory exists or could be made. False otherwise; errno tells why.
  bool ready();

  // Each returns 0x00 for DONE, or the refusal reason (protocol.hpp).
  uint8_t begin(uint16_t number, uint32_t length, uint32_t & resume_at);
  uint8_t chunk(uint16_t number, uint32_t offset, const uint8_t * data, size_t size,
    bool & written);
  uint8_t end(uint16_t number, const ImageChecker & checker, ImageVerdict & verdict);
  uint8_t cancel(uint16_t number);

  // A whole image that passed its checks at UPLOAD END, read into out. False if there is none.
  bool load(uint16_t number, uint8_t * out, size_t room, size_t & size) const;

  // Replaces the directory flush (tests only).
  void setDirectorySync(DirectorySync sync);

  // The open upload, for the log and the tests.
  bool open() const;
  uint32_t stored() const;

private:
  bool sameHeader(const uint8_t * data, size_t size) const;
  bool recordHeaderHash();
  bool path(std::array<char, 512> & out, uint16_t number, const char * suffix) const;
  void closeUpload();
  bool writeImage(uint16_t number, const uint8_t * image, size_t size);

  const char * dir_;
  int fd_;            // the open .part file; -1 if none
  uint16_t number_;
  uint32_t length_;   // declared at BEGIN
  uint32_t stored_;   // bytes stored so far
  std::array<uint8_t, 32> hash_;   // SHA-256 of the first 64 bytes, valid when have_hash_
  bool have_hash_;
  DirectorySync sync_;   // flushes the data directory; syncDirectory unless a test replaced it
  std::array<uint8_t, config::kMaxImageBytes> image_;   // UPLOAD END reads the image here
};

class Delivery
{
public:
  enum class Step : uint8_t
  {
    kContinue,   // send next()
    kDone,       // the controller accepted UPLOAD END
    kRefused,    // the controller refused a step; code says why
  };

  Delivery();

  bool active() const;

  // DELIVER loads the image here, checks it, then calls start().
  uint8_t * buffer();
  size_t capacity() const;

  void start(uint16_t image_number, size_t image_size, uint32_t ground_sequence,
    uint32_t command_expiry);

  // The command to send now (payload, its length, its timeout). Only while active().
  uint16_t next(uint8_t * payload, size_t room, uint32_t & timeout_ms) const;

  // The last answer to the command from next(): DONE moves on, REFUSED stops with the
  // controller's code, ALREADY DONE stops with 0x04 (the updater never resends a step).
  Step onFinalAnswer(uint8_t kind, uint8_t answer_code, uint8_t & code);

  void stop();

  uint16_t number() const;
  uint32_t groundSequence() const;
  uint32_t expiry() const;
  size_t size() const;
  size_t sent() const;   // body bytes the controller has acknowledged

private:
  enum class Stage : uint8_t
  {
    kIdle,
    kBegin,
    kChunks,
    kEnd,
  };

  Stage stage_;
  uint16_t number_;
  size_t size_;
  size_t offset_;
  uint32_t ground_sequence_;
  uint32_t expiry_;
  std::array<uint8_t, config::kMaxImageBytes> image_;
};

#endif  // FSIR_UPDATER_UPDATE_MANAGER_HPP
