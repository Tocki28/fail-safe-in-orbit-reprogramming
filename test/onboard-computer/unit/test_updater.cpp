/*
 * test_updater.cpp — host unit tests of the onboard computer's updater
 * (src/onboard-computer/updater), every file except main.cpp.
 *
 *   frame codec   the same bytes as the controller's frame.c of commit 5c0b5f8 (the Makefile
 *                 takes it from git and compiles it in beside the updater's code) for the
 *                 same inputs, and the same frames accepted from the same byte streams; two
 *                 known-answer frames computed apart, with Python's struct and zlib.crc32
 *                 (from test/controller/unit/test_frame.c at commit 57d1530)
 *   image checks  the real signed images from `make -C src/controller` and `make -C
 *                 test/controller`, and copies damaged in memory, each against the refusal
 *                 reason of the Interfaces Sec. 4.2 table it must give
 *   upload store  BEGIN, CHUNK, END, CANCEL, resume after a restart, a half-written chunk
 *   delivery      the commands sent to the controller, step by step
 *   test image    the 1,024 bytes and their CRC-32; the once-per-trial rule; the exact frames
 *                 sent to the controller (an Updater on a fake data link A, a loopback UDP port)
 *   arming        a ground ARM held and paired with its dangerous command on data link A
 *   reports       report number and onboard time filled in; lengths and kinds checked
 *   data link     stop-and-wait, sequence numbers, timeout, the 10 ms gap
 *   options       the command line
 *
 * Usage: test_updater CONTROLLER_IMAGES TEST_IMAGES FLIPPED_IMAGE SCRATCH_DIR
 * Prints PASS or FAIL per test; exit status 1 on a failure.
 */
#include <array>
#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "bytes.hpp"
#include "command_gate.hpp"
#include "config.hpp"
#include "controller_watch.hpp"
#include "crc32.hpp"
#include "data_link.hpp"
#include "frame.hpp"
#include "image_checker.hpp"
#include "keys.h"
#include "log.hpp"
#include "options.hpp"
#include "protocol.hpp"
#include "report_store.hpp"
#include "test_image.hpp"
#include "update_manager.hpp"
#include "updater.hpp"

// The controller's frame layer and CRC-32, in C, as reference.
extern "C" {
#include "crc32.h"
#include "frame.h"
}

namespace
{

int g_tests = 0;
int g_failures = 0;

/**
 * @brief Counts one test and prints PASS or FAIL with its name.
 *
 * Adds one to g_tests; on a failure also adds one to g_failures. main() turns g_failures into
 * the exit status.
 *
 * @param[in]     ok    true if the checked condition held
 * @param[in]     name  the test's name, printed after PASS or FAIL
 * @return nothing
 */
void report(bool ok, const std::string & name)
{
  g_tests++;
  if (!ok) {
    g_failures++;
  }
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name.c_str());
}

/**
 * @brief Reads a whole file into memory.
 *
 * Reads in blocks of 4096 bytes. If the file cannot be opened it prints "cannot read" with the
 * path to stderr and returns an empty vector; callers treat an empty vector as a missing file.
 *
 * @param[in]     path  path of the file to read
 * @return the file's bytes; empty if the file cannot be opened
 */
std::vector<uint8_t> readFile(const std::string & path)
{
  std::vector<uint8_t> data;
  std::FILE * f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    std::fprintf(stderr, "cannot read %s\n", path.c_str());
    return data;
  }
  std::array<uint8_t, 4096> block{};
  size_t n = 0;
  while ((n = std::fread(block.data(), 1, block.size(), f)) > 0) {
    data.insert(data.end(), block.begin(), block.begin() + static_cast<long>(n));
  }
  (void)std::fclose(f);
  return data;
}

// Fixed pseudo-random sequence, the same at every run.
uint32_t g_seed = 12345U;

/**
 * @brief Gives the next number of the fixed pseudo-random sequence.
 *
 * Fixed pseudo-random sequence, the same at every run. Updates g_seed with a linear congruential
 * step (multiplier 1103515245, increment 12345, modulo 2^32) and returns it shifted right by 8.
 *
 * @return the next value, 0 to 2^24 - 1
 */
uint32_t nextRandom()
{
  g_seed = g_seed * 1103515245U + 12345U;
  return g_seed >> 8;
}

// ---- CRC-32 and frames ---------------------------------------------------------------------

/**
 * @brief Checks the updater's CRC-32 against a known value and against the controller's C code.
 *
 * Passes if crc::crc32 of "123456789" is 0xCBF43926, the CRC-32 check value for that text
 * (zlib.crc32(b"123456789")), and if it equals the controller's crc32_compute on 50 random
 * buffers of length 0 up to 1099 bytes.
 *
 * @return nothing
 */
void testCrc()
{
  const char * text = "123456789";
  const uint8_t * data = reinterpret_cast<const uint8_t *>(text);
  // 0xCBF43926: the CRC-32 check value for "123456789" (zlib.crc32(b"123456789")).
  report(crc::crc32(data, 9) == 0xCBF43926U, "CRC-32 of \"123456789\" is 0xCBF43926");
  bool same = true;
  std::array<uint8_t, 1100> buffer{};
  for (int round = 0; round < 50; round++) {
    size_t size = nextRandom() % buffer.size();
    for (size_t i = 0; i < size; i++) {
      buffer[i] = static_cast<uint8_t>(nextRandom());
    }
    same = same && crc::crc32(buffer.data(), size) ==
                     crc32_compute(buffer.data(), static_cast<uint32_t>(size));
  }
  report(same, "CRC-32 equals the controller's crc32_compute on 50 random buffers");
}

// From test/controller/unit/test_frame.c (commit 57d1530), computed with Python's struct and
// zlib.crc32.
const std::array<uint8_t, 21> kGetStatusFrame = {
  0xA5, 0x5A, 0x01, 0x01, 0x02, 0x00, 0x01, 0x00, 0x07, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
  0xFF, 0x25, 0x12, 0x77, 0x1A, 0x06};
const std::array<uint8_t, 26> kAnswerFrame = {
  0xA5, 0x5A, 0x01, 0x02, 0x01, 0x00, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x78, 0x56, 0x34, 0x12, 0x02, 0x00, 0x53, 0x0E, 0x01, 0x68};

/**
 * @brief Checks the frame codec against two known-answer frames.
 *
 * Passes if encoding GET STATUS with sequence 7 gives kGetStatusFrame byte for byte, if encoding
 * the DONE answer for 0x12345678 gives kAnswerFrame byte for byte, and if decoding kAnswerFrame
 * gives back its fields. Both frames were computed apart with Python's struct and zlib.crc32
 * (from test/controller/unit/test_frame.c at commit 57d1530).
 *
 * @return nothing
 */
void testKnownFrames()
{
  std::array<uint8_t, frame::kMaxBytes> out{};
  const uint8_t get_status = 0x25;
  frame::Frame f{frame::kTypeCommand, frame::kDestController, 0, 1, 7, 0xFFFFFFFFU, &get_status};
  size_t size = frame::encode(f, out.data(), out.size());
  report(size == kGetStatusFrame.size() && std::memcmp(out.data(), kGetStatusFrame.data(), size) == 0,
    "GET STATUS seq 7: bytes equal the known answer");

  std::array<uint8_t, frame::kAnswerBytes> payload{};
  frame::encodeAnswer(frame::Answer{0x12345678U, answer::kDone, 0}, payload.data());
  frame::Frame a{frame::kTypeAnswer, frame::kDestOnboard, 0, 6, 1, 0, payload.data()};
  size = frame::encode(a, out.data(), out.size());
  report(size == kAnswerFrame.size() && std::memcmp(out.data(), kAnswerFrame.data(), size) == 0,
    "answer DONE for 0x12345678: bytes equal the known answer");

  frame::Frame d{};
  frame::Answer decoded{};
  bool ok = frame::decode(kAnswerFrame.data(), kAnswerFrame.size(), d) &&
            frame::decodeAnswer(d, decoded) && decoded.answered == 0x12345678U &&
            decoded.kind == answer::kDone && decoded.code == 0 && d.sequence == 1 &&
            d.destination == frame::kDestOnboard;
  report(ok, "known answer frame decodes to its fields");
}

/**
 * @brief Checks that frame::encode gives the same bytes as the controller's frame.c.
 *
 * Builds 300 frames, the first seven with the payload lengths 0, 1, 6, 7, 263, 1023 and 1024, the
 * rest random. Passes if every frame encodes to the same bytes as frame_build of frame.c (commit
 * 5c0b5f8). Also passes if encode returns 0 for a 1025-byte payload and for an output buffer one
 * byte too short.
 *
 * @return nothing
 */
void testEncodeAgainstFrameC()
{
  std::array<uint8_t, frame::kPayloadMax + 1> payload{};
  std::array<uint8_t, frame::kMaxBytes + 8> mine{};
  std::array<uint8_t, frame::kMaxBytes + 8> theirs{};
  const std::array<uint16_t, 7> edges = {0, 1, 6, 7, 263, 1023, 1024};
  bool same = true;
  int frames = 0;
  for (int round = 0; round < 300; round++) {
    uint16_t length = (static_cast<size_t>(round) < edges.size())
                        ? edges[static_cast<size_t>(round)]
                        : static_cast<uint16_t>(nextRandom() % (frame::kPayloadMax + 1));
    for (size_t i = 0; i < length; i++) {
      payload[i] = static_cast<uint8_t>(nextRandom());
    }
    frame::Frame f{static_cast<uint8_t>(nextRandom()), static_cast<uint8_t>(nextRandom()),
      static_cast<uint8_t>(nextRandom()), length, nextRandom() * 977U, nextRandom() * 131U,
      payload.data()};
    frame_t c{f.type, f.destination, f.flags, f.length, f.sequence, f.expiry, payload.data()};
    size_t a = frame::encode(f, mine.data(), mine.size());
    uint32_t b = frame_build(&c, theirs.data(), static_cast<uint32_t>(theirs.size()));
    same = same && a == b && a > 0 && std::memcmp(mine.data(), theirs.data(), a) == 0;
    frames++;
  }
  report(same, "encode gives frame.c's bytes for " + std::to_string(frames) + " frames, payload 0 to 1024");

  frame::Frame big{1, 2, 0, 1025, 1, 1, payload.data()};
  report(frame::encode(big, mine.data(), mine.size()) == 0, "encode refuses a 1025-byte payload");
  frame::Frame small{1, 2, 0, 10, 1, 1, payload.data()};
  report(frame::encode(small, mine.data(), 29) == 0, "encode refuses a buffer one byte short");
}

/**
 * @brief Checks which datagrams frame::decode accepts and which it drops.
 *
 * Passes if the known GET STATUS frame decodes with sequence 7, length 1, payload 0x25 and expiry
 * 0xFFFFFFFF. Passes if each of its 168 single flipped bits makes decode drop the frame. Passes
 * if the frame one byte short and one byte extra are both dropped.
 *
 * @return nothing
 */
void testDecode()
{
  std::array<uint8_t, 64> bytes{};
  std::memcpy(bytes.data(), kGetStatusFrame.data(), kGetStatusFrame.size());
  frame::Frame f{};
  bool good = frame::decode(bytes.data(), kGetStatusFrame.size(), f) && f.sequence == 7 &&
              f.length == 1 && f.payload[0] == 0x25 && f.expiry == 0xFFFFFFFFU;
  report(good, "decode: a good datagram");

  bool all_dropped = true;
  for (size_t bit = 0; bit < kGetStatusFrame.size() * 8; bit++) {
    std::memcpy(bytes.data(), kGetStatusFrame.data(), kGetStatusFrame.size());
    bytes[bit / 8] = static_cast<uint8_t>(bytes[bit / 8] ^ (1U << (bit % 8)));
    all_dropped = all_dropped && !frame::decode(bytes.data(), kGetStatusFrame.size(), f);
  }
  report(all_dropped, "decode: every single flipped bit of a frame is dropped (168 cases)");

  std::memcpy(bytes.data(), kGetStatusFrame.data(), kGetStatusFrame.size());
  report(!frame::decode(bytes.data(), kGetStatusFrame.size() - 1, f), "decode: one byte short is dropped");
  report(!frame::decode(bytes.data(), kGetStatusFrame.size() + 1, f), "decode: one byte extra is dropped");
}

// The same byte stream, with the same arrival times, into both parsers; the frames each
// accepts must match one for one.
struct Accepted
{
  size_t at;
  uint8_t type;
  uint32_t sequence;
  std::vector<uint8_t> payload;
};

/**
 * @brief Checks that frame::Parser accepts the same frames as the controller's frame parser.
 *
 * Builds a byte stream from 3000 pieces: good frames (some with one bit flipped, some cut short)
 * and noise with Sync bytes, with pauses of 0, 3, 10, 11 or 60 ms between pieces. Feeds each
 * byte with its arrival time to frame::Parser and to frame.c's frame_parser_feed. Passes if both
 * accept the same frames at the same byte positions with the same type, sequence and payload,
 * and at least one frame is accepted. See the comment above struct Accepted.
 *
 * @return nothing
 */
void testParserAgainstFrameC()
{
  std::vector<uint8_t> stream;
  std::vector<uint32_t> times;
  uint32_t now = 1000;
  std::array<uint8_t, frame::kMaxBytes> built{};
  std::array<uint8_t, 300> payload{};
  int good_frames = 0;
  for (int piece = 0; piece < 3000; piece++) {
    uint32_t what = nextRandom() % 10;
    // Time passes between pieces: none, a little, just over the 10 ms gap, a lot.
    const std::array<uint32_t, 5> steps = {0, 3, 10, 11, 60};
    now += steps[nextRandom() % steps.size()];
    if (what < 5) {   // a good frame
      uint16_t length = static_cast<uint16_t>(nextRandom() % payload.size());
      for (size_t i = 0; i < length; i++) {
        payload[i] = static_cast<uint8_t>(nextRandom());
      }
      frame::Frame f{2, 1, 0, length, static_cast<uint32_t>(piece), 0, payload.data()};
      size_t size = frame::encode(f, built.data(), built.size());
      bool damaged = what == 4;   // one in five has one bit flipped
      if (damaged) {
        size_t at = nextRandom() % size;
        built[at] = static_cast<uint8_t>(built[at] ^ (1U << (nextRandom() % 8)));
      }
      bool cut = (nextRandom() % 8) == 0;   // sometimes cut short
      size_t send = cut ? nextRandom() % size : size;
      good_frames += (!damaged && !cut) ? 1 : 0;
      for (size_t i = 0; i < send; i++) {
        stream.push_back(built[i]);
        // A frame's bytes mostly arrive together; now and then a pause inside one.
        if ((nextRandom() % 64) == 0) {
          now += steps[nextRandom() % steps.size()];
        }
        times.push_back(now);
      }
    } else {   // noise, with Sync bytes now and then
      uint32_t count = nextRandom() % 40;
      for (uint32_t i = 0; i < count; i++) {
        uint32_t r = nextRandom() % 6;
        stream.push_back(r == 0 ? 0xA5 : (r == 1 ? 0x5A : static_cast<uint8_t>(nextRandom())));
        times.push_back(now);
      }
    }
  }

  std::vector<Accepted> mine;
  std::vector<Accepted> theirs;
  frame::Parser parser;
  static frame_parser_t c_parser;
  frame_parser_init(&c_parser);
  for (size_t i = 0; i < stream.size(); i++) {
    frame::Frame f{};
    if (parser.feed(stream[i], times[i], f)) {
      mine.push_back(Accepted{i, f.type, f.sequence,
        std::vector<uint8_t>(f.payload, f.payload + f.length)});
    }
    frame_t c{};
    if (frame_parser_feed(&c_parser, stream[i], times[i], &c)) {
      theirs.push_back(Accepted{i, c.type, c.sequence,
        std::vector<uint8_t>(c.payload, c.payload + c.length)});
    }
  }
  bool same = mine.size() == theirs.size();
  for (size_t i = 0; same && i < mine.size(); i++) {
    same = mine[i].at == theirs[i].at && mine[i].type == theirs[i].type &&
           mine[i].sequence == theirs[i].sequence && mine[i].payload == theirs[i].payload;
  }
  report(same && !mine.empty(), "parser accepts the same frames as frame.c from " +
    std::to_string(stream.size()) + " bytes: " + std::to_string(mine.size()) + " frames (" +
    std::to_string(good_frames) + " sent whole and undamaged)");
}

// ---- Image checks ---------------------------------------------------------------------------

struct ImageCase
{
  std::string name;
  std::vector<uint8_t> bytes;
  uint8_t expected;
  const char * reason;
};

/**
 * @brief Inverts one byte of an image in memory.
 *
 * XORs the byte with 0xFF, as tools/flip_byte.py.
 *
 * @param[in,out] image   the image bytes; one byte is changed
 * @param[in]     offset  index of the byte to invert
 * @return nothing
 */
void flip(std::vector<uint8_t> & image, size_t offset)
{
  image[offset] = static_cast<uint8_t>(image[offset] ^ 0xFFU);   // as tools/flip_byte.py
}

/**
 * @brief Checks the image checker's verdict code and reason on good and damaged images.
 *
 * Runs ImageChecker on the signed images from the controller build and the test build, on files
 * damaged by the build, and on copies of firmware-B.img damaged in memory. Passes for an image if
 * the verdict code and reason equal the expected pair listed for it, which come from the
 * refusal-reason table of Interfaces Sec. 4.2. A missing file is reported as a failure. Also
 * passes if firmware-B.img with key B given as key A is refused with 0x09 (signature A).
 *
 * @param[in]     controller_images  directory with firmware-A.img and firmware-B.img
 * @param[in]     test_images        directory with the test build's images
 * @param[in]     flipped_file       firmware-B.img flipped at 0x600 by tools/flip_byte.py
 * @return nothing
 */
void testImages(const std::string & controller_images, const std::string & test_images,
  const std::string & flipped_file)
{
  ImageChecker checker(PUBLIC_KEY_A, PUBLIC_KEY_B);
  report(checker.ready(), "OpenSSL accepts keys.h's public keys as Ed25519 keys");

  std::vector<uint8_t> fw_b = readFile(controller_images + "/firmware-B.img");
  std::vector<ImageCase> cases = {
    {"firmware-A.img", readFile(controller_images + "/firmware-A.img"), 0x00, "passed"},
    {"firmware-B.img", fw_b, 0x00, "passed"},
    {"slotA-v1.img", readFile(test_images + "/slotA-v1.img"), 0x00, "passed"},
    {"slotB-v2.img", readFile(test_images + "/slotB-v2.img"), 0x00, "passed"},
    {"slotB-v2-hang.img", readFile(test_images + "/slotB-v2-hang.img"), 0x00, "passed"},
    {"last-resort-v1.img (kind 0x03)", readFile(test_images + "/last-resort-v1.img"), 0x00, "passed"},
    {"slotA-v1-flipped.img (byte 0x600 inverted)", readFile(test_images + "/slotA-v1-flipped.img"),
     0x08, "body check value"},
    {"firmware-B.img flipped at 0x600 by tools/flip_byte.py", readFile(flipped_file), 0x08,
     "body check value"},
    {"slotA-v1-wrong-key-a.img (signed with key-wrong as A)",
     readFile(test_images + "/slotA-v1-wrong-key-a.img"), 0x09, "signature A"},
    {"slotB-v2-wrong-key-b.img (signed with key-wrong as B)",
     readFile(test_images + "/slotB-v2-wrong-key-b.img"), 0x09, "signature B"},
    {"firmware-B-wrong-key-a.img (the firmware, signed with key-wrong as A)",
     readFile(test_images + "/firmware-B-wrong-key-a.img"), 0x09, "signature A"},
  };

  // Damaged copies of firmware-B.img, made here.
  if (fw_b.size() > config::kImageOverheadBytes + 0x600) {
    size_t body = fw_b.size() - config::kImageOverheadBytes;
    std::vector<uint8_t> v = fw_b;
    v[0] = 'X';
    cases.push_back({"proprietary string changed", v, 0x08, "proprietary string"});
    v = fw_b;
    v[IMAGE_OFFSET_FORMAT] = 2;
    cases.push_back({"format 2", v, 0x08, "format"});
    v = fw_b;
    v[IMAGE_OFFSET_KIND] = 0x07;
    cases.push_back({"kind 0x07", v, 0x06, "kind"});
    v = fw_b;
    bytes::writeU16(&v[IMAGE_OFFSET_TARGET], 0x0432);
    cases.push_back({"target 0x0432", v, 0x0B, "target"});
    v = fw_b;
    bytes::writeU32(&v[IMAGE_OFFSET_LENGTH], static_cast<uint32_t>(body + 1));
    cases.push_back({"header Length one too many", v, 0x08, "length"});
    v = fw_b;
    v.pop_back();
    cases.push_back({"last byte missing", v, 0x08, "length"});
    v = fw_b;
    v.push_back(0);
    cases.push_back({"one byte extra", v, 0x08, "length"});
    v = fw_b;
    flip(v, IMAGE_OFFSET_BUILD_ID);
    cases.push_back({"build identifier changed (header: only the signatures cover it)", v, 0x09,
                     "signature A"});
    v = fw_b;
    flip(v, config::kImageHeaderBytes + body + 5);
    cases.push_back({"Signature A damaged", v, 0x09, "signature A"});
    v = fw_b;
    flip(v, fw_b.size() - 1);
    cases.push_back({"Signature B damaged", v, 0x09, "signature B"});
    v.assign(fw_b.begin(), fw_b.begin() + 100);
    cases.push_back({"100 bytes only", v, 0x08, "size"});
  }

  for (const ImageCase & c : cases) {
    if (c.bytes.empty()) {
      report(false, "image " + c.name + ": file missing (build the images first)");
      continue;
    }
    ImageVerdict v = checker.check(c.bytes.data(), c.bytes.size());
    char line[200];
    std::snprintf(line, sizeof line, "image %s: 0x%02x (%s), expected 0x%02x (%s)",
      c.name.c_str(), v.code, v.reason, c.expected, c.reason);
    report(v.code == c.expected && std::strcmp(v.reason, c.reason) == 0, line);
  }

  // The right image with the wrong keys: key B given as key A.
  ImageChecker swapped(PUBLIC_KEY_B, PUBLIC_KEY_B);
  ImageVerdict v = swapped.check(fw_b.data(), fw_b.size());
  report(v.code == 0x09 && std::strcmp(v.reason, "signature A") == 0,
    "firmware-B.img with key B given as key A: 0x09 (signature A)");
}

// ---- Upload store -------------------------------------------------------------------------

/**
 * @brief Deletes the files in a directory, then the directory.
 *
 * Does not descend into subdirectories. Does nothing if the directory cannot be opened.
 *
 * @param[in]     dir  path of the directory
 * @return nothing
 */
void removeTree(const std::string & dir)
{
  DIR * d = opendir(dir.c_str());
  if (d == nullptr) {
    return;
  }
  while (dirent * e = readdir(d)) {
    std::string name = e->d_name;
    if (name != "." && name != "..") {
      (void)unlink((dir + "/" + name).c_str());
    }
  }
  (void)closedir(d);
  (void)rmdir(dir.c_str());
}

/**
 * @brief Tells whether a path exists.
 *
 * @param[in]     path  file or directory path
 * @return true: stat succeeds on the path; false: otherwise
 */
bool exists(const std::string & path)
{
  struct stat info{};
  return stat(path.c_str(), &info) == 0;
}

/**
 * @brief Uploads image[from, to) in chunks of config::kChunkBytes.
 *
 * Uploads image[from, to) in 256-byte chunks; false at the first refusal.
 *
 * @param[in,out] store   the upload store receiving the chunks
 * @param[in]     number  image number
 * @param[in]     image   the whole image
 * @param[in]     from    offset of the first byte to send, bytes
 * @param[in]     to      offset after the last byte to send, bytes
 * @return true: every chunk was accepted; false: the store refused a chunk
 */
bool sendChunks(UploadStore & store, uint16_t number, const std::vector<uint8_t> & image,
  size_t from, size_t to)
{
  for (size_t offset = from; offset < to; offset += config::kChunkBytes) {
    size_t size = std::min(config::kChunkBytes, image.size() - offset);
    bool written = false;
    if (store.chunk(number, static_cast<uint32_t>(offset), &image[offset], size, written) != 0) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Checks the upload store: BEGIN, CHUNK, END, CANCEL, resume after a restart, a half-written chunk.
 *
 * Works in a nested data directory under the scratch directory and removes it at the end. Passes
 * if each call returns the code the test expects: 0 for DONE, 0x06 for a refused BEGIN or CHUNK
 * or an incomplete END, 0x08 for a damaged image at END, 0x09 for a wrong signature at END. Also
 * passes if a new store on the same directory resumes at 512 stored bytes (REQ-063), if a chunk
 * half written at power loss is cut back to whole chunks, and if END keeps only the .img file
 * of a good image and nothing of a damaged one.
 *
 * @param[in]     controller_images  directory with firmware-B.img
 * @param[in]     test_images        directory with slotA-v1-flipped.img and slotA-v1-wrong-key-a.img
 * @param[in]     scratch            directory for the store's files
 * @return nothing
 */
void testUploadStore(const std::string & controller_images, const std::string & test_images,
  const std::string & scratch)
{
  std::string dir = scratch + "/store/nested";
  removeTree(dir);
  ImageChecker checker(PUBLIC_KEY_A, PUBLIC_KEY_B);
  std::vector<uint8_t> image = readFile(controller_images + "/firmware-B.img");
  uint32_t length = static_cast<uint32_t>(image.size());
  uint32_t stored = 99;
  bool written = false;
  ImageVerdict verdict{};

  {
    UploadStore store(dir.c_str());
    report(store.ready() && exists(dir), "store: data directory made with its parents");
    report(store.begin(2, 100, stored) == 0x06, "BEGIN with 100 bytes (no room for header and signatures): 0x06");
    report(store.begin(2, static_cast<uint32_t>(config::kMaxImageBytes) + 1, stored) == 0x06,
      "BEGIN with 40961 bytes (larger than a slot): 0x06");
    report(store.begin(2, length, stored) == 0 && stored == 0, "BEGIN image 2: DONE, 0 stored");
    report(store.chunk(2, 0, &image[0], 256, written) == 0 && written, "CHUNK at 0: written");
    report(store.chunk(2, 0, &image[0], 256, written) == 0 && !written,
      "CHUNK at 0 again: DONE, not written again");
    report(store.chunk(2, 512, &image[512], 256, written) == 0x06, "CHUNK at 512 with 256 stored: 0x06");
    report(store.chunk(2, 128, &image[128], 256, written) == 0x06, "CHUNK across the end of the stored bytes: 0x06");
    report(store.chunk(3, 256, &image[256], 256, written) == 0x06, "CHUNK for another image number: 0x06");
    report(store.chunk(2, 256, &image[256], 256, written) == 0 && store.stored() == 512, "CHUNK at 256: 512 stored");
    report(store.end(2, checker, verdict) == 0x06 && store.open(), "END with the image incomplete: 0x06, upload kept");
  }
  {
    // A restart of the program: a new store on the same directory (REQ-063).
    UploadStore store(dir.c_str());
    report(store.ready(), "store: ready again after a restart");
    report(store.begin(2, length, stored) == 0 && stored == 512,
      "BEGIN image 2, same length, after a restart: resumes at 512");
    report(store.chunk(2, 0, &image[0], 256, written) == 0 && !written,
      "resent CHUNK at 0 after the restart: DONE without writing");
    bool rest = sendChunks(store, 2, image, 512, image.size());
    report(rest && store.stored() == length, "the remaining chunks: all stored");
    bool past = store.chunk(2, length, &image[0], 1, written) == 0x06;
    report(past, "CHUNK past the declared length: 0x06");
    report(store.end(2, checker, verdict) == 0 && verdict.kind == IMAGE_KIND_MAIN,
      "END image 2: checks passed");
    report(!exists(dir + "/image-00002.part") && exists(dir + "/image-00002.img"),
      "after END: .part removed, .img kept");
    std::vector<uint8_t> loaded(config::kMaxImageBytes);
    size_t size = 0;
    report(store.load(2, loaded.data(), loaded.size(), size) && size == image.size() &&
           std::memcmp(loaded.data(), image.data(), size) == 0,
      "load image 2: the same bytes as firmware-B.img");

    // A chunk half written when the power went: cut back to whole chunks at the next BEGIN.
    report(store.begin(3, length, stored) == 0 && sendChunks(store, 3, image, 0, 512),
      "image 3: two chunks stored");
  }
  {
    std::FILE * part = std::fopen((dir + "/image-00003.part").c_str(), "ab");
    std::array<uint8_t, 100> half{};
    bool appended = part != nullptr && std::fwrite(half.data(), 1, half.size(), part) == half.size();
    if (part != nullptr) {
      (void)std::fclose(part);
    }
    UploadStore store(dir.c_str());
    report(appended && store.ready() && store.begin(3, length, stored) == 0 && stored == 512,
      "a half-written chunk (612 bytes in the file): resumes at 512");
    report(store.begin(3, length + 1, stored) == 0 && stored == 0,
      "BEGIN image 3 with another length: starts afresh");

    std::vector<uint8_t> flipped = readFile(test_images + "/slotA-v1-flipped.img");
    uint32_t flipped_length = static_cast<uint32_t>(flipped.size());
    bool sent = store.begin(4, flipped_length, stored) == 0 && sendChunks(store, 4, flipped, 0, flipped.size());
    uint8_t code = store.end(4, checker, verdict);
    report(sent && code == 0x08 && !exists(dir + "/image-00004.part") &&
           !exists(dir + "/image-00004.img"),
      "damaged image 4: END 0x08, nothing kept");

    std::vector<uint8_t> wrong = readFile(test_images + "/slotA-v1-wrong-key-a.img");
    sent = store.begin(5, static_cast<uint32_t>(wrong.size()), stored) == 0 &&
           sendChunks(store, 5, wrong, 0, wrong.size());
    report(sent && store.end(5, checker, verdict) == 0x09, "image 5 signed with the wrong key A: END 0x09");

    std::vector<uint8_t> loaded(config::kMaxImageBytes);
    size_t size = 0;
    report(store.cancel(2) == 0 && !exists(dir + "/image-00002.img") &&
           !store.load(2, loaded.data(), loaded.size(), size),
      "CANCEL image 2: its .img removed; nothing to load");
    report(store.cancel(9) == 0, "CANCEL of an image never uploaded: DONE");
  }
  removeTree(dir);
}

// ---- Delivery --------------------------------------------------------------------------------

/**
 * @brief Checks the commands a Delivery sends to the controller, step by step.
 *
 * Starts a delivery of image 7 with 600 bytes. Passes if it sends UPLOAD BEGIN with a 3 s
 * timeout, then CHUNKs at offsets 0, 256 and 512 of 256, 256 and 88 bytes with a 2 s timeout
 * (ALREADY DONE counts as done), then UPLOAD END, and is then done. Also passes if a refused
 * CHUNK (code 0x0E) ends the delivery as refused with the controller's code and the ground
 * sequence number kept.
 *
 * @return nothing
 */
void testDelivery()
{
  Delivery d;
  for (size_t i = 0; i < 600; i++) {
    d.buffer()[i] = static_cast<uint8_t>(i * 7);
  }
  d.start(7, 600, 42, 99);
  std::array<uint8_t, frame::kPayloadMax> p{};
  uint32_t timeout = 0;
  uint8_t code = 0xEE;

  uint16_t n = d.next(p.data(), p.size(), timeout);
  const std::array<uint8_t, 7> begin = {0x01, 0x07, 0x00, 0x58, 0x02, 0x00, 0x00};
  report(n == 7 && std::memcmp(p.data(), begin.data(), 7) == 0 && timeout == 3000,
    "delivery: UPLOAD BEGIN image 7, 600 bytes, 3 s timeout");
  report(d.onFinalAnswer(answer::kDone, 0, code) == Delivery::Step::kContinue, "BEGIN DONE: continue");

  bool chunks_ok = true;
  const std::array<uint32_t, 3> offsets = {0, 256, 512};
  const std::array<uint16_t, 3> sizes = {256, 256, 88};
  for (size_t i = 0; i < offsets.size(); i++) {
    n = d.next(p.data(), p.size(), timeout);
    chunks_ok = chunks_ok && n == 7 + sizes[i] && p[0] == 0x02 && bytes::readU16(&p[1]) == 7 &&
                bytes::readU32(&p[3]) == offsets[i] && timeout == 2000 &&
                std::memcmp(&p[7], d.buffer() + offsets[i], sizes[i]) == 0;
    uint8_t kind = (i == 1) ? answer::kAlreadyDone : answer::kDone;   // ALREADY DONE counts as done
    chunks_ok = chunks_ok && d.onFinalAnswer(kind, 0, code) == Delivery::Step::kContinue;
  }
  report(chunks_ok, "delivery: CHUNKs at 0, 256, 512 of 256, 256, 88 bytes, 2 s timeout");
  n = d.next(p.data(), p.size(), timeout);
  report(n == 3 && p[0] == 0x03 && bytes::readU16(&p[1]) == 7, "delivery: UPLOAD END image 7");
  report(d.onFinalAnswer(answer::kDone, 0, code) == Delivery::Step::kDone && !d.active(),
    "END DONE: delivery done");

  d.start(8, 600, 43, 0);
  (void)d.onFinalAnswer(answer::kDone, 0, code);
  report(d.onFinalAnswer(answer::kRefused, 0x0E, code) == Delivery::Step::kRefused &&
         code == 0x0E && !d.active() && d.groundSequence() == 43,
    "a CHUNK refused 0x0E: delivery refused with the controller's code");
}

// ---- Reports ---------------------------------------------------------------------------------

/**
 * @brief Checks how ReportStore numbers, stamps, reads and drops reports.
 *
 * Passes if a 24-byte HEALTH report is accepted with report number 1 and the onboard time written
 * in place and the rest unchanged, and its fields read back (mode TRIAL, slot B, version 5, 300 s
 * of trial left). Passes if the next report gets number 2 and a 23-byte HEALTH is dropped. Passes
 * if EVENT reports of 15 and 47 bytes are accepted and one of 48 bytes is dropped. Passes if
 * report kind 0x09 is dropped and dropped reports take no number.
 *
 * @return nothing
 */
void testReports()
{
  ReportStore store;
  std::array<uint8_t, 24> health = {0, 0, 0, 0, 0, 0, 0, 0, 0x10, 0x00, 0x00, 0x00, 0x01, 0x02,
                                    0x02, 0x05, 0x00, 0x00, 0x00, 0x02, 0x01, 0x2C, 0x01, 0x00};
  std::array<uint8_t, report::kEventMaxBytes> out{};
  bool ok = store.accept(health.data(), health.size(), 0xA1B2C3D4U, 500, out.data(), out.size());
  report(ok && bytes::readU32(&out[0]) == 1 && bytes::readU32(&out[4]) == 0xA1B2C3D4U &&
         std::memcmp(&out[8], &health[8], 16) == 0,
    "HEALTH: report number 1 and onboard time written in place, the rest unchanged");
  const HealthReport & h = store.lastHealth();
  report(store.haveHealth() && h.mode == report::kModeTrial && h.slot == 2 && h.firmware_version == 5 &&
         h.trial_left_s == 300 && h.controller_time == 16 && store.lastHealthMs() == 500,
    "HEALTH fields read: mode TRIAL, slot B, version 5, 300 s of trial left");
  ok = store.accept(health.data(), health.size(), 1, 600, out.data(), out.size());
  report(ok && bytes::readU32(&out[0]) == 2, "second report: number 2");
  report(!store.accept(health.data(), 23, 1, 600, out.data(), out.size()), "HEALTH of 23 bytes: dropped");

  std::array<uint8_t, 48> event{};
  event[report::kOffsetKind] = report::kEvent;
  report(store.accept(event.data(), 15, 1, 700, out.data(), out.size()) &&
         bytes::readU32(&out[0]) == 3, "EVENT of 15 bytes: number 3");
  report(store.accept(event.data(), 47, 1, 700, out.data(), out.size()), "EVENT of 47 bytes (32 of detail): accepted");
  report(!store.accept(event.data(), 48, 1, 700, out.data(), out.size()), "EVENT of 48 bytes: dropped");
  event[report::kOffsetKind] = 0x09;
  report(!store.accept(event.data(), 20, 1, 700, out.data(), out.size()), "report kind 0x09: dropped");
  report(store.lastNumber() == 4, "dropped reports take no number");
}

// ---- Data link -------------------------------------------------------------------------------

class FakeTransport final : public Transport
{
public:
  std::vector<uint8_t> sent;
  std::vector<uint8_t> incoming;

  /**
   * @brief Keeps the bytes the code under test sends.
   *
   * @param[in]     data  the bytes to send
   * @param[in]     size  number of bytes
   * @return true, always (the bytes are appended to sent)
   */
  bool sendBytes(const uint8_t * data, size_t size) override
  {
    sent.insert(sent.end(), data, data + size);
    return true;
  }

  /**
   * @brief Hands the code under test the bytes the test put in incoming.
   *
   * Moves up to capacity bytes from the front of incoming into buffer and removes them from incoming.
   *
   * @param[out]    buffer    where the bytes are written
   * @param[in]     capacity  size of buffer, bytes
   * @return the number of bytes written to buffer
   */
  long receiveBytes(uint8_t * buffer, size_t capacity) override
  {
    size_t n = std::min(capacity, incoming.size());
    std::memcpy(buffer, incoming.data(), n);
    incoming.erase(incoming.begin(), incoming.begin() + static_cast<long>(n));
    return static_cast<long>(n);
  }

  /**
   * @brief Gives the file descriptor to poll for incoming bytes.
   *
   * @return -1, always (the fake has no descriptor)
   */
  int pollFd() const override
  {
    return -1;
  }
};

class Collector final : public ControllerFrameHandler
{
public:
  std::vector<frame::Answer> answers;

  /**
   * @brief Keeps the answer carried by a frame from the controller.
   *
   * A frame that does not decode as an answer is ignored.
   *
   * @param[in]     f  a frame the data link accepted from the controller
   * @return nothing
   */
  void onControllerFrame(const frame::Frame & f) override
  {
    frame::Answer a{};
    if (frame::decodeAnswer(f, a)) {
      answers.push_back(a);
    }
  }
};

/**
 * @brief Builds the bytes of an answer frame from the controller to the onboard computer.
 *
 * @param[in]     sequence  the frame's sequence number
 * @param[in]     answered  the sequence number of the command being answered
 * @param[in]     kind      answer::kReceived, kDone, kRefused...
 * @return the encoded frame
 */
std::vector<uint8_t> answerBytes(uint32_t sequence, uint32_t answered, uint8_t kind)
{
  std::array<uint8_t, 6> payload{};
  frame::encodeAnswer(frame::Answer{answered, kind, 0}, payload.data());
  frame::Frame f{frame::kTypeAnswer, frame::kDestOnboard, 0, 6, sequence, 0, payload.data()};
  std::array<uint8_t, 64> out{};
  size_t n = frame::encode(f, out.data(), out.size());
  return std::vector<uint8_t>(out.begin(), out.begin() + static_cast<long>(n));
}

/**
 * @brief Checks DataLink: stop-and-wait, sequence numbers, timeout and the 10 ms gap.
 *
 * Passes if GET STATUS is sent as sequence 1 to Destination 0x02 and the link is then busy, a
 * second command while busy is not sent, and a RECEIVED answer arrives as a frame. Passes if the
 * timeout fires at 2000 ms after sending (not at 2999 ms, yes at 3000 ms) and frees the link, and
 * if later commands get sequence numbers 2 and 3. The 10 ms gap (Interfaces Sec. 6.2): half a
 * frame, then the rest 20 ms later: dropped; 5 ms later: taken.
 *
 * @return nothing
 */
void testDataLink()
{
  FakeTransport transport;
  DataLink link(transport);
  Collector collector;
  const uint8_t get_status = 0x25;
  uint32_t sequence = 0;

  bool sent = link.sendCommand(&get_status, 1, 0xFFFFFFFFU, 2000, 1000, sequence);
  frame::Frame f{};
  bool decoded = frame::decode(transport.sent.data(), transport.sent.size(), f);
  report(sent && sequence == 1 && link.busy() && decoded && f.destination == frame::kDestController &&
         f.type == frame::kTypeCommand && f.sequence == 1,
    "data link: GET STATUS sent as sequence 1 to Destination 0x02; link busy");
  report(!link.sendCommand(&get_status, 1, 0, 2000, 1000, sequence), "data link: a second command while busy is not sent");

  transport.incoming = answerBytes(1, 1, answer::kReceived);
  report(link.receive(collector, 0) && collector.answers.size() == 1 &&
         collector.answers[0].kind == answer::kReceived,
    "data link: RECEIVED arrives as a frame");
  report(!link.timedOut(2999) && link.timedOut(3000) && !link.busy(),
    "data link: timeout at 2000 ms after sending, not before");

  sent = link.sendCommand(&get_status, 1, 0, 2000, 4000, sequence);
  link.release();
  bool third = link.sendCommand(&get_status, 1, 0, 2000, 4000, sequence);
  report(sent && third && sequence == 3, "data link: sequence numbers 1, 2, 3");

  // The 10 ms gap (Interfaces Sec. 6.2): half a frame, then the rest 20 ms later: dropped;
  // 5 ms later: taken.
  std::vector<uint8_t> bytes = answerBytes(9, 3, answer::kDone);
  size_t half = bytes.size() / 2;
  collector.answers.clear();
  transport.incoming.assign(bytes.begin(), bytes.begin() + static_cast<long>(half));
  (void)link.receive(collector, 100);
  transport.incoming.assign(bytes.begin() + static_cast<long>(half), bytes.end());
  (void)link.receive(collector, 120);
  report(collector.answers.empty(), "data link: a frame with a 20 ms gap inside is dropped");
  transport.incoming.assign(bytes.begin(), bytes.begin() + static_cast<long>(half));
  (void)link.receive(collector, 200);
  transport.incoming.assign(bytes.begin() + static_cast<long>(half), bytes.end());
  (void)link.receive(collector, 205);
  report(collector.answers.size() == 1 && collector.answers[0].answered == 3,
    "data link: a frame with a 5 ms gap inside is taken");
}

// ---- Gate tables, options ------------------------------------------------------------------

/**
 * @brief Checks the command gate's ForwardTable and ForwardQueue.
 *
 * Passes if the forward table finds added links, loses a removed one and replaces its oldest
 * entry when full. Passes if the forward queue holds config::kForwardQueueSize commands (8),
 * refuses one more, gives the oldest first with its fields kept, and gives the next after pop.
 *
 * @return nothing
 */
void testTables()
{
  ForwardTable table;
  uint32_t ground = 0;
  table.add(5, 105);
  table.add(6, 106);
  report(table.find(5, ground) && ground == 105 && table.find(6, ground) && ground == 106 &&
         !table.find(7, ground), "forward table: link 5 -> ground 105, link 6 -> ground 106");
  table.remove(5);
  report(!table.find(5, ground), "forward table: removed entry is gone");
  for (uint32_t i = 0; i < config::kForwardTableSize; i++) {
    table.add(100 + i, i);
  }
  report(!table.find(6, ground) && table.find(100, ground) && ground == 0,
    "forward table: the oldest entry is replaced when full");

  ForwardQueue queue;
  const uint8_t payload[1] = {0x20};
  frame::Frame c{frame::kTypeCommand, frame::kDestController, 0, 1, 77, 5, payload};
  bool pushed = true;
  for (size_t i = 0; i < config::kForwardQueueSize; i++) {
    c.sequence = 77 + static_cast<uint32_t>(i);
    pushed = pushed && queue.push(c, false);
  }
  report(pushed && queue.full() && !queue.push(c, false), "forward queue: holds 8, refuses a ninth");
  report(queue.front().ground_sequence == 77 && queue.front().payload[0] == 0x20 &&
         queue.front().expiry == 5, "forward queue: oldest first, fields kept");
  queue.pop();
  report(queue.front().ground_sequence == 78, "forward queue: pop gives the next");
}

/**
 * @brief Checks the command-line parser.
 *
 * Passes if no arguments give the defaults (/dev/ttyACM0, UDP port 47001, /var/lib/fsir/images,
 * beat off, not verbose) and if every option is read. Passes if port 70000, an unknown word, a
 * missing value and a negative number are each refused.
 *
 * @return nothing
 */
void testOptions()
{
  Options o{};
  const char * none[] = {"fsir-updater"};
  report(parseOptions(1, none, o) && std::strcmp(o.device, "/dev/ttyACM0") == 0 &&
         o.udp_port == 47001 && std::strcmp(o.data_dir, "/var/lib/fsir/images") == 0 &&
         o.key_a_file == nullptr && o.beat_s == 0 && !o.verbose,
    "options: defaults /dev/ttyACM0, UDP 47001, /var/lib/fsir/images, beat off");
  const char * all[] = {"x", "--device", "/dev/ttys004", "--udp-port", "5000", "--data-dir",
                        "/tmp/d", "--key-a", "a.key", "--key-b", "b.key", "--beat-s", "2",
                        "--verbose"};
  report(parseOptions(14, all, o) && o.udp_port == 5000 && o.beat_s == 2 && o.verbose &&
         std::strcmp(o.key_b_file, "b.key") == 0, "options: every option read");
  const char * bad_port[] = {"x", "--udp-port", "70000"};
  const char * bad_word[] = {"x", "--speed", "9600"};
  const char * missing[] = {"x", "--device"};
  const char * negative[] = {"x", "--beat-s", "-1"};
  report(!parseOptions(3, bad_port, o) && !parseOptions(3, bad_word, o) &&
         !parseOptions(2, missing, o) && !parseOptions(3, negative, o),
    "options: port 70000, an unknown word, a missing value, a negative number refused");
}

/**
 * @brief Checks that readKeyFile accepts a 32-byte key file and refuses a 33-byte one.
 *
 * Writes PUBLIC_KEY_A to a file in the scratch directory, reads it back and compares, then
 * appends one byte and expects a refusal. Removes the file at the end.
 *
 * @param[in]     scratch  directory for the key file
 * @return nothing
 */
void testKeyFile(const std::string & scratch)
{
  std::string path = scratch + "/key-a.bin";
  std::FILE * f = std::fopen(path.c_str(), "wb");
  bool written = f != nullptr && std::fwrite(PUBLIC_KEY_A, 1, 32, f) == 32;
  if (f != nullptr) {
    (void)std::fclose(f);
  }
  std::array<uint8_t, 32> key{};
  report(written && readKeyFile(path.c_str(), key.data()) &&
         std::memcmp(key.data(), PUBLIC_KEY_A, 32) == 0, "key file of 32 bytes: read");
  f = std::fopen(path.c_str(), "ab");
  if (f != nullptr) {
    (void)std::fputc(0, f);
    (void)std::fclose(f);
  }
  report(!readKeyFile(path.c_str(), key.data()), "key file of 33 bytes: refused");
  (void)unlink(path.c_str());
}

// ---- Test image and arming, through an Updater -----------------------------------------------

/**
 * @brief Checks the test image bytes: size, pattern, and the CRC-32 at the end.
 *
 * Interfaces Sec. 5.1: the test image's last four bytes are the CRC-32 of the bytes before them.
 * The pattern is test_image.hpp's (byte i = i mod 251); the CRC is also compared with the
 * controller's own crc32_compute (commit 5c0b5f8), so it is not only checked against itself.
 *
 * @return nothing
 */
void testTestImageBytes()
{
  std::array<uint8_t, test_image::kBytes> image{};
  test_image::build(image);
  bool pattern = true;
  for (size_t i = 0; i < 1020; i++) {
    pattern = pattern && image[i] == static_cast<uint8_t>(i % 251);
  }
  report(image.size() == 1024 && pattern, "test image: 1,024 bytes, the first 1,020 are i mod 251");
  uint32_t stored = bytes::readU32(&image[1020]);
  report(stored == crc::crc32(image.data(), 1020) && stored == crc32_compute(image.data(), 1020),
    "test image: last 4 bytes are the CRC-32 of the first 1,020 (also the controller's crc32_compute)");
  report(image[1020] == static_cast<uint8_t>(stored) &&
         image[1023] == static_cast<uint8_t>(stored >> 24), "test image: CRC-32 is little-endian");
  report(image[256] == 5 && image[512] == 10 && image[768] == 15,
    "test image: the chunks at 256, 512 and 768 start with 5, 10 and 15 (251 does not divide 256)");
  std::array<uint8_t, test_image::kBytes> flipped = image;
  flipped[600] ^= 0x01U;
  report(crc::crc32(flipped.data(), 1020) != bytes::readU32(&flipped[1020]),
    "test image: one flipped bit no longer matches its CRC-32");
  report(std::strcmp(eventName(0x000A), "trial tests passed") == 0 &&
         std::strcmp(eventName(0x000B), "trial tests failed") == 0 &&
         std::strcmp(eventName(0x0007), "firmware confirmed") == 0 &&
         std::strcmp(eventName(0x7777), "unknown") == 0,
    "event names: 0x000A trial tests passed, 0x000B trial tests failed, 0x0007, unknown");
}

/**
 * @brief Checks the once-per-trial rule of TestImageTrigger.
 *
 * Owner's rule: a trial runs from the first HEALTH report showing TRIAL until a HEALTH report
 * shows another mode; the image is sent once in it.
 *
 * @return nothing
 */
void testTestImageTrigger()
{
  TestImageTrigger t;
  report(!t.due(), "trigger: nothing due before any HEALTH report");
  t.onHealthMode(report::kModeOperate);
  report(!t.due(), "trigger: OPERATE: not due");
  t.onHealthMode(report::kModeTrial);
  report(t.due(), "trigger: first TRIAL: due");
  t.onHealthMode(report::kModeTrial);
  report(t.due(), "trigger: still TRIAL and not started: still due");
  t.markStarted();
  report(!t.due(), "trigger: started: not due");
  t.onHealthMode(report::kModeTrial);
  t.onHealthMode(report::kModeTrial);
  report(!t.due(), "trigger: more TRIAL reports in the same trial: not due again");
  t.onHealthMode(report::kModeOperate);
  report(!t.due(), "trigger: OPERATE ends the trial: not due");
  t.onHealthMode(report::kModeTrial);
  report(t.due(), "trigger: TRIAL after OPERATE is a new trial: due");
  t.onHealthMode(report::kModeFallback);
  report(!t.due(), "trigger: FALLBACK before the image started: withdrawn");
  t.onHealthMode(report::kModeTrial);
  t.markStarted();
  t.onHealthMode(report::kModeFallback);
  t.onHealthMode(report::kModeTrial);
  report(t.due(), "trigger: FALLBACK then TRIAL: new trial, due");
}

// A frame the updater sent on data link A, taken apart.
struct SentFrame
{
  uint8_t type;
  uint8_t destination;
  uint32_t sequence;
  uint32_t expiry;
  std::vector<uint8_t> payload;
};

// An answer the updater sent to the ground.
struct GroundAnswer
{
  uint32_t answered;
  uint8_t kind;
  uint8_t code;
};

// An Updater on a fake data link A and a loopback UDP port; the test plays the ground (UDP) and
// the controller (the fake transport) and moves the clock. Every frame the updater sends on
// data link A is kept in order, so a frame sent from inside a ground command is not lost.
class UpdaterRig
{
public:
  static constexpr uint16_t kPort = 47931;   // bench choice; the bench test uses 47001

  /**
   * @brief Builds the rig: a fake transport, the gate on kPort, the store and the Updater.
   *
   * The clock starts at 1000 ms. Call open() before use.
   *
   * @param[in]     dir      data directory of the upload store
   * @param[in]     beat_ms  the Updater's beat period, ms
   */
  UpdaterRig(const std::string & dir, uint32_t beat_ms)
  : transport_(), link_(transport_), gate_(kPort), store_(dir.c_str()),
    checker_(PUBLIC_KEY_A, PUBLIC_KEY_B), reports_(), watch_(5000, 0),
    updater_(gate_, link_, store_, checker_, reports_, watch_, beat_ms), client_(-1), now_(1000),
    listen_(0), next_sequence_(1), parser_(), pending_()
  {
  }

  /**
   * @brief Closes the test's own socket if it is open.
   *
   * @return nothing
   */
  ~UpdaterRig()
  {
    if (client_ >= 0) {
      (void)::close(client_);
    }
  }

  UpdaterRig(const UpdaterRig &) = delete;
  UpdaterRig & operator=(const UpdaterRig &) = delete;

  /**
   * @brief Opens the ground socket and the test's own socket.
   *
   * @return true: both open and the store is ready; false: the port is in use or a call failed
   */
  bool open()
  {
    client_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    return client_ >= 0 && gate_.open() && store_.ready() && checker_.ready();
  }

  /**
   * @brief Gives access to the rig's upload store.
   *
   * @return the upload store
   */
  UploadStore & store() { return store_; }

  /**
   * @brief Gives access to the rig's image checker.
   *
   * @return the image checker, read only
   */
  const ImageChecker & checker() const { return checker_; }

  /** @brief Moves the clock on. @param[in] ms time, ms @return nothing */
  void advance(uint64_t ms) { now_ += ms; }

  /**
   * @brief Puts one frame from the controller into data link A; the updater reads it.
   *
   * @param[in]     type     frame::kType...
   * @param[in]     payload  the frame's payload
   * @return nothing
   */
  void fromController(uint8_t type, const std::vector<uint8_t> & payload)
  {
    frame::Frame f{type, frame::kDestOnboard, 0, static_cast<uint16_t>(payload.size()),
                   next_sequence_++, 0, payload.data()};
    std::array<uint8_t, frame::kMaxBytes> out{};
    size_t n = frame::encode(f, out.data(), out.size());
    transport_.incoming.assign(out.begin(), out.begin() + static_cast<long>(n));
    listen_ += 100;
    (void)updater_.onLinkReadable(listen_, now_);
    collect();
  }

  /** @brief A HEALTH report showing this mode. @param[in] mode report::kMode... @return nothing */
  void health(uint8_t mode)
  {
    std::vector<uint8_t> p(report::kHealthBytes, 0);
    p[report::kOffsetKind] = report::kHealth;
    p[report::kOffsetMode] = mode;
    p[report::kOffsetFirmwareKind] = 0x02;
    fromController(frame::kTypeReport, p);
  }

  /**
   * @brief An answer from the controller to a command of the updater.
   *
   * @param[in]     answered  the data-link sequence number of that command
   * @param[in]     kind      answer::kReceived, kDone, kRefused
   * @param[in]     code      the answer's code
   * @return nothing
   */
  void answer(uint32_t answered, uint8_t kind, uint8_t code)
  {
    std::vector<uint8_t> p(6, 0);
    frame::encodeAnswer(frame::Answer{answered, kind, code}, p.data());
    fromController(frame::kTypeAnswer, p);
  }

  /**
   * @brief Runs the updater's timers and sending, then takes the oldest unread frame sent.
   *
   * @param[out]    out  the frame
   * @return true: a frame was sent since the last call that returned one; false: none
   */
  bool tick(SentFrame & out)
  {
    updater_.tick(now_);
    collect();
    if (pending_.empty()) {
      return false;
    }
    out = pending_.front();
    pending_.erase(pending_.begin());
    return true;
  }

  /**
   * @brief A command from the ground, over the loopback socket.
   *
   * Waits at most 1 s for the datagram to reach the gate.
   *
   * @param[in]     destination  frame::kDestOnboard or kDestController
   * @param[in]     sequence     the ground's sequence number
   * @param[in]     payload      the command's payload
   * @return true: the gate received it; false: it did not arrive in 1 s
   */
  bool fromGround(uint8_t destination, uint32_t sequence, const std::vector<uint8_t> & payload)
  {
    frame::Frame f{frame::kTypeCommand, destination, 0, static_cast<uint16_t>(payload.size()),
                   sequence, 0xFFFFFFFFU, payload.data()};
    std::array<uint8_t, frame::kMaxBytes> out{};
    size_t n = frame::encode(f, out.data(), out.size());
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kPort);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::sendto(client_, out.data(), n, 0, reinterpret_cast<const sockaddr *>(&to),
          sizeof to) != static_cast<ssize_t>(n)) {
      return false;
    }
    pollfd p{};
    p.fd = gate_.fd();
    p.events = POLLIN;
    if (::poll(&p, 1, 1000) != 1) {
      return false;
    }
    updater_.onGroundReadable(now_);
    collect();
    return true;
  }

  /**
   * @brief Reads the answers the updater has sent to the ground since the last call.
   *
   * @return the answers in order (reports are skipped)
   */
  std::vector<GroundAnswer> groundAnswers()
  {
    std::vector<GroundAnswer> result;
    std::array<uint8_t, 2048> buffer{};
    pollfd p{};
    p.fd = client_;
    p.events = POLLIN;
    while (::poll(&p, 1, 20) == 1) {
      ssize_t n = ::recv(client_, buffer.data(), buffer.size(), 0);
      frame::Frame f{};
      frame::Answer a{};
      if (n > 0 && frame::decode(buffer.data(), static_cast<size_t>(n), f) &&
          f.type == frame::kTypeAnswer && frame::decodeAnswer(f, a)) {
        result.push_back(GroundAnswer{a.answered, a.kind, a.code});
      }
    }
    return result;
  }

private:
  /**
   * @brief Moves the bytes the updater sent into whole frames, in order.
   *
   * @return nothing
   */
  void collect()
  {
    for (uint8_t byte : transport_.sent) {
      frame::Frame f{};
      if (parser_.feed(byte, 0, f)) {
        pending_.push_back(SentFrame{f.type, f.destination, f.sequence, f.expiry,
                                     std::vector<uint8_t>(f.payload, f.payload + f.length)});
      }
    }
    transport_.sent.clear();
  }

  FakeTransport transport_;
  DataLink link_;
  CommandGate gate_;
  UploadStore store_;
  ImageChecker checker_;
  ReportStore reports_;
  ControllerWatch watch_;
  Updater updater_;
  int client_;
  uint64_t now_;
  uint64_t listen_;
  uint32_t next_sequence_;
  frame::Parser parser_;
  std::vector<SentFrame> pending_;
};

/**
 * @brief True when f is a command to the controller whose payload is exactly expected.
 *
 * @param[in]     f         a frame the updater sent
 * @param[in]     expected  the payload it must carry
 * @return true: type command, Destination 0x02, payload equal; false: otherwise
 */
bool isCommand(const SentFrame & f, const std::vector<uint8_t> & expected)
{
  return f.type == frame::kTypeCommand && f.destination == frame::kDestController &&
         f.payload == expected;
}

/**
 * @brief The UPLOAD CHUNK of the test image at this offset: header, then 256 image bytes.
 *
 * @param[in]     image   the test image
 * @param[in]     offset  0, 256, 512 or 768
 * @return the command payload (Interfaces Sec. 5.1: image number, offset, data)
 */
std::vector<uint8_t> chunkOf(const std::array<uint8_t, test_image::kBytes> & image, uint32_t offset)
{
  std::vector<uint8_t> p = {0x02, 0xFF, 0xFF, 0, 0, 0, 0};
  bytes::writeU32(&p[3], offset);
  p.insert(p.end(), image.begin() + offset, image.begin() + offset + 256);
  return p;
}

/**
 * @brief Runs the rig's timers and tells whether the updater sent nothing.
 *
 * @param[in,out] rig  the updater under test
 * @return true: no frame was sent; false: a frame was sent (it is consumed)
 */
bool quiet(UpdaterRig & rig)
{
  SentFrame f{};
  return !rig.tick(f);
}

/**
 * @brief The test image sent through an Updater: when, how often, and the exact frames.
 *
 * Nothing before TRIAL; once per trial; DONE, REFUSED 0x0F and no answer, none retried; again in
 * the next trial; a ground command that arrives during the exchange waits for it; no answer
 * goes to the ground.
 *
 * @param[in]     scratch  scratch directory
 * @return nothing
 */
void testTestImageSending(const std::string & scratch)
{
  std::string dir = scratch + "/test-image";
  removeTree(dir);
  UpdaterRig rig(dir, 0);
  if (!rig.open()) {
    report(false, "test image: updater rig could not open (UDP port 47931 in use?)");
    return;
  }
  std::array<uint8_t, test_image::kBytes> image{};
  test_image::build(image);
  const std::vector<uint8_t> begin = {0x01, 0xFF, 0xFF, 0x00, 0x04, 0x00, 0x00};
  const std::vector<uint8_t> end = {0x03, 0xFF, 0xFF};
  SentFrame f{};

  bool silent = quiet(rig);
  rig.health(report::kModeOperate);
  silent = silent && quiet(rig);
  rig.health(report::kModeFallback);
  silent = silent && quiet(rig);
  report(silent, "test image: nothing sent before any HEALTH report, in OPERATE or in FALLBACK");

  // Trial 1: every step answered DONE; a ground GET STATUS arrives during the exchange.
  rig.health(report::kModeTrial);
  bool ok = rig.tick(f) && isCommand(f, begin) && f.expiry == 0xFFFFFFFFU;
  report(ok, "test image: first TRIAL report: UPLOAD BEGIN image 0xFFFF, 1024 bytes, to Destination 0x02");
  report(quiet(rig), "test image: nothing more while BEGIN is outstanding");
  ok = rig.fromGround(frame::kDestController, 1, {command::kGetStatus}) && quiet(rig);
  report(ok, "test image: a ground command that arrives during the exchange waits");
  rig.answer(f.sequence, answer::kReceived, 0);
  report(quiet(rig), "test image: RECEIVED is not the last answer: nothing sent");
  rig.answer(f.sequence, answer::kDone, 0);
  ok = true;
  for (uint32_t offset = 0; offset < 1024; offset += 256) {
    bool sent = rig.tick(f) && isCommand(f, chunkOf(image, offset));
    ok = ok && sent;
    rig.answer(f.sequence, answer::kDone, 0);
  }
  report(ok, "test image: UPLOAD CHUNKs at offsets 0, 256, 512, 768 of 256 bytes each, image 0xFFFF");
  ok = rig.tick(f) && isCommand(f, end);
  report(ok, "test image: then UPLOAD END image 0xFFFF");
  rig.answer(f.sequence, answer::kDone, 0);
  ok = rig.tick(f) && isCommand(f, {command::kGetStatus});
  report(ok, "test image: after END DONE the waiting ground command goes next");
  rig.answer(f.sequence, answer::kDone, 0);
  rig.health(report::kModeTrial);
  rig.health(report::kModeTrial);
  report(quiet(rig), "test image: more TRIAL reports in the same trial: not sent again");
  std::vector<GroundAnswer> to_ground = rig.groundAnswers();
  bool only_ground_command = !to_ground.empty();
  for (const GroundAnswer & a : to_ground) {
    only_ground_command = only_ground_command && a.answered == 1;
  }
  report(only_ground_command, "test image: no answer to the ground except for its own command");

  // Trial 2: the controller refuses UPLOAD BEGIN with 0x0F; no retry.
  rig.health(report::kModeOperate);
  rig.health(report::kModeTrial);
  ok = rig.tick(f) && isCommand(f, begin);
  report(ok, "test image: OPERATE, then TRIAL again: sent again in the new trial");
  rig.answer(f.sequence, answer::kRefused, answer::kTrialTestsNotPassed);
  rig.health(report::kModeTrial);
  report(quiet(rig), "test image: refused 0x0F: not retried");

  // Trial 3: no answer; bounded by the BEGIN timeout (3 s), then not retried.
  rig.health(report::kModeFallback);
  rig.health(report::kModeTrial);
  ok = rig.tick(f) && isCommand(f, begin);
  rig.advance(2999);
  ok = ok && quiet(rig);
  rig.advance(2);
  report(ok && quiet(rig), "test image: no answer: waits 3 s, then stops, sends nothing");
  rig.health(report::kModeTrial);
  report(quiet(rig), "test image: after no answer: not retried in the same trial");
}

/**
 * @brief The test image waits for a running delivery, then goes.
 *
 * A delivery of the real firmware-B image (image 3) is started in OPERATE; a TRIAL report arrives
 * while it runs. Every frame until its UPLOAD END must be the delivery's (image 3); the first
 * frame after the controller's answer to that END must be the test image's UPLOAD BEGIN.
 *
 * @param[in]     controller_images  directory with firmware-B.img
 * @param[in]     scratch            scratch directory
 * @return nothing
 */
void testTestImageWaitsForDelivery(const std::string & controller_images,
  const std::string & scratch)
{
  std::string dir = scratch + "/test-image-delivery";
  removeTree(dir);
  UpdaterRig rig(dir, 0);
  if (!rig.open()) {
    report(false, "test image and delivery: updater rig could not open");
    return;
  }
  std::vector<uint8_t> image = readFile(controller_images + "/firmware-B.img");
  uint32_t resume = 0;
  ImageVerdict verdict{};
  bool stored = rig.store().begin(3, static_cast<uint32_t>(image.size()), resume) == 0 &&
                sendChunks(rig.store(), 3, image, 0, image.size()) &&
                rig.store().end(3, rig.checker(), verdict) == 0;
  report(stored, "test image and delivery: image 3 stored");

  rig.health(report::kModeOperate);
  bool ok = rig.fromGround(frame::kDestOnboard, 1, {command::kDeliver, 3, 0});
  SentFrame f{};
  ok = ok && rig.tick(f) && f.payload.size() == 7 && f.payload[0] == 0x01 && f.payload[1] == 3;
  report(ok, "test image and delivery: DELIVER image 3 starts with UPLOAD BEGIN image 3");

  rig.health(report::kModeTrial);
  report(quiet(rig), "test image and delivery: TRIAL report during the delivery: nothing sent");

  bool only_delivery = true;
  bool reached_end = false;
  for (int step = 0; step < 400 && !reached_end; step++) {
    uint8_t id = f.payload[0];
    rig.answer(f.sequence, answer::kDone, 0);
    if (id == 0x03) {
      reached_end = true;
      break;
    }
    only_delivery = rig.tick(f) && only_delivery && f.payload.size() >= 3 && f.payload[1] == 3 &&
                    f.payload[2] == 0;
  }
  report(reached_end && only_delivery,
    "test image and delivery: every frame until END is the delivery's (image 3)");
  const std::vector<uint8_t> begin = {0x01, 0xFF, 0xFF, 0x00, 0x04, 0x00, 0x00};
  report(rig.tick(f) && isCommand(f, begin),
    "test image and delivery: the first frame after the delivery's END is the test image's BEGIN");
}

/**
 * @brief The pairing of a ground ARM with its dangerous command (Interfaces Sec. 5.2).
 *
 * Cases: ARM held and not passed on; the pair sent back to back with the right sequence number;
 * the ARM's answers do not go to the ground; REFUSED 0x06, 0x01, 0x02; a replaced ARM; an expired
 * ARM; the controller refusing the ARM; nothing sent between the two frames.
 *
 * @param[in]     scratch  scratch directory
 * @return nothing
 */
void testArmPairing(const std::string & scratch)
{
  std::string dir = scratch + "/arm";
  removeTree(dir);
  UpdaterRig rig(dir, 1000);
  if (!rig.open()) {
    report(false, "arm: updater rig could not open (UDP port 47931 in use?)");
    return;
  }
  const uint32_t kDest = frame::kDestController;
  SentFrame f{};
  // The beat goes first on an idle link; answer it so it is out of the way.
  bool ok = rig.tick(f) && isCommand(f, {command::kGetStatus});
  rig.answer(f.sequence, answer::kDone, 0);
  report(ok, "arm: setup: the idle beat GET STATUS is answered");
  (void)rig.groundAnswers();

  auto armPayload = [](uint8_t id, uint32_t sequence) {
    std::vector<uint8_t> p = {command::kArm, id, 0, 0, 0, 0};
    bytes::writeU32(&p[2], sequence);
    return p;
  };
  auto has = [](const std::vector<GroundAnswer> & v, uint32_t answered, uint8_t kind, uint8_t code) {
    for (const GroundAnswer & a : v) {
      if (a.answered == answered && a.kind == kind && a.code == code) {
        return true;
      }
    }
    return false;
  };

  // 1. An ARM is held, answered RECEIVED and DONE, and not passed on.
  ok = rig.fromGround(kDest, 1, armPayload(command::kSwitch, 7));
  std::vector<GroundAnswer> g = rig.groundAnswers();
  report(ok && g.size() == 2 && has(g, 1, answer::kReceived, 0) && has(g, 1, answer::kDone, 0) &&
         quiet(rig), "arm: ARM for SWITCH held; RECEIVED then DONE to the ground; nothing sent on data link A");

  // 2. The pair: ARM naming the command's data-link sequence number, then the command.
  ok = rig.fromGround(kDest, 7, {command::kSwitch});
  SentFrame arm{};
  ok = ok && rig.tick(arm) && arm.destination == frame::kDestController &&
       arm.type == frame::kTypeCommand && arm.payload.size() == 6 && arm.payload[0] == 0x0D &&
       arm.payload[1] == command::kSwitch;
  uint32_t named = (arm.payload.size() == 6) ? bytes::readU32(&arm.payload[2]) : 0;
  report(ok && named == arm.sequence + 1, "arm: first frame is ARM Destination 0x02 naming SWITCH and the next data-link sequence number");
  report(quiet(rig), "arm: the SWITCH waits for the ARM's last answer");
  rig.answer(arm.sequence, answer::kReceived, 0);
  report(quiet(rig), "arm: RECEIVED to the ARM is not the last answer: nothing sent");
  rig.answer(arm.sequence, answer::kDone, 0);
  SentFrame cmd{};
  ok = rig.tick(cmd) && isCommand(cmd, {command::kSwitch}) && cmd.sequence == named;
  report(ok, "arm: next frame is SWITCH, carrying exactly the sequence number the ARM named");
  report(rig.groundAnswers().empty(), "arm: the ARM's answers are not passed to the ground");
  rig.answer(cmd.sequence, answer::kReceived, 0);
  rig.answer(cmd.sequence, answer::kDone, 0);
  g = rig.groundAnswers();
  report(g.size() == 2 && has(g, 7, answer::kReceived, 0) && has(g, 7, answer::kDone, 0),
    "arm: the command's answers reach the ground with the ground's sequence number 7");

  // 3. The ARM is used up.
  ok = rig.fromGround(kDest, 7, {command::kSwitch});
  g = rig.groundAnswers();
  report(ok && g.size() == 1 && has(g, 7, answer::kRefused, 0x01) && quiet(rig),
    "arm: the same SWITCH again, ARM used up: REFUSED 0x01, nothing sent");

  // 4. Refusals 0x06.
  ok = rig.fromGround(kDest, 20, armPayload(0x23, 9)) && rig.fromGround(kDest, 21, armPayload(command::kArm, 9)) &&
       rig.fromGround(kDest, 22, {command::kArm, command::kSwitch});
  g = rig.groundAnswers();
  report(ok && g.size() == 3 && has(g, 20, answer::kRefused, 0x06) && has(g, 21, answer::kRefused, 0x06) &&
         has(g, 22, answer::kRefused, 0x06) && quiet(rig),
    "arm: ARM naming LEAVE FALLBACK, naming ARM, or too short: REFUSED 0x06, nothing held or sent");

  // 5. Refusals 0x01: no ARM at all; wrong identifier; wrong sequence number. The ARM stays.
  ok = rig.fromGround(kDest, 30, {command::kReturnToPrevious});
  report(ok && has(rig.groundAnswers(), 30, answer::kRefused, 0x01) && quiet(rig),
    "arm: dangerous command with no ARM held: REFUSED 0x01");
  ok = rig.fromGround(kDest, 31, armPayload(command::kSwitch, 40));
  (void)rig.groundAnswers();
  ok = ok && rig.fromGround(kDest, 40, {command::kReturnToPrevious}) &&
       rig.fromGround(kDest, 41, {command::kSwitch});
  g = rig.groundAnswers();
  report(ok && g.size() == 2 && has(g, 40, answer::kRefused, 0x01) && has(g, 41, answer::kRefused, 0x01) &&
         quiet(rig), "arm: ARM names SWITCH 40: RETURN TO PREVIOUS 40 and SWITCH 41 both REFUSED 0x01");
  ok = rig.fromGround(kDest, 40, {command::kSwitch}) && rig.tick(arm) && arm.payload.size() == 6;
  report(ok, "arm: the ARM was kept by the refusals: SWITCH 40 then sends the pair");
  rig.answer(arm.sequence, answer::kDone, 0);
  ok = rig.tick(cmd) && isCommand(cmd, {command::kSwitch});
  rig.answer(cmd.sequence, answer::kDone, 0);
  report(ok, "arm: SWITCH 40 sent after its ARM");
  (void)rig.groundAnswers();

  // 6. A new ARM replaces the held one.
  ok = rig.fromGround(kDest, 50, armPayload(command::kSwitch, 60)) &&
       rig.fromGround(kDest, 51, armPayload(command::kSwitch, 61));
  (void)rig.groundAnswers();
  ok = ok && rig.fromGround(kDest, 60, {command::kSwitch});
  g = rig.groundAnswers();
  report(ok && has(g, 60, answer::kRefused, 0x01) && quiet(rig),
    "arm: second ARM replaces the first: SWITCH 60 REFUSED 0x01");
  ok = rig.fromGround(kDest, 61, {command::kSwitch}) && rig.tick(arm) && arm.payload.size() == 6;
  report(ok, "arm: SWITCH 61 sends the pair");
  rig.answer(arm.sequence, answer::kDone, 0);
  ok = rig.tick(cmd) && isCommand(cmd, {command::kSwitch});
  rig.answer(cmd.sequence, answer::kDone, 0);
  (void)rig.groundAnswers();

  // 7. Expiry: 30 s, a bench value (config::kArmExpiryMs).
  ok = rig.fromGround(kDest, 70, armPayload(command::kConfirm, 71));
  rig.advance(config::kArmExpiryMs);
  ok = ok && rig.fromGround(kDest, 71, {command::kConfirm}) && rig.tick(arm) &&
       arm.payload.size() == 6 && arm.payload[1] == command::kConfirm;
  report(ok, "arm: CONFIRM at exactly 30 s: still valid; the pair goes out");
  rig.answer(arm.sequence, answer::kDone, 0);
  ok = rig.tick(cmd) && isCommand(cmd, {command::kConfirm});
  rig.answer(cmd.sequence, answer::kDone, 0);
  (void)rig.groundAnswers();
  ok = rig.fromGround(kDest, 72, armPayload(command::kConfirm, 73));
  rig.advance(config::kArmExpiryMs + 1);
  ok = ok && rig.fromGround(kDest, 73, {command::kConfirm});
  (void)rig.tick(f);   // the idle beat, which has come due in the meantime, if any
  g = rig.groundAnswers();
  report(ok && has(g, 73, answer::kRefused, 0x02), "arm: CONFIRM 30.001 s after its ARM: REFUSED 0x02");
  ok = rig.fromGround(kDest, 73, {command::kConfirm});
  report(ok && has(rig.groundAnswers(), 73, answer::kRefused, 0x01),
    "arm: an expired ARM is dropped: the same command again is REFUSED 0x01");
  if (f.type == frame::kTypeCommand && f.payload == std::vector<uint8_t>{command::kGetStatus}) {
    rig.answer(f.sequence, answer::kDone, 0);
  }

  // 8. The controller refuses the ARM: the command is not sent; the ground is refused with its code.
  (void)rig.tick(f);
  ok = rig.fromGround(kDest, 80, armPayload(command::kSwitch, 81));
  (void)rig.groundAnswers();
  ok = ok && rig.fromGround(kDest, 81, {command::kSwitch});
  SentFrame refused_arm{};
  // A beat already outstanding would hold the ARM back; answer it first.
  while (rig.tick(refused_arm) && refused_arm.payload[0] != command::kArm) {
    rig.answer(refused_arm.sequence, answer::kDone, 0);
  }
  ok = ok && refused_arm.payload.size() == 6 && refused_arm.payload[0] == command::kArm;
  rig.answer(refused_arm.sequence, answer::kRefused, answer::kNotInThisMode);
  g = rig.groundAnswers();
  report(ok && has(g, 81, answer::kRefused, answer::kNotInThisMode),
    "arm: controller refuses the ARM 0x05: the ground's SWITCH is REFUSED 0x05");
  bool no_switch = true;
  SentFrame later{};
  while (rig.tick(later)) {
    no_switch = no_switch && later.payload[0] != command::kSwitch;
    rig.answer(later.sequence, answer::kDone, 0);
  }
  report(no_switch, "arm: and the SWITCH itself is never sent");

  // 9. Nothing between the pair: a test image due, a ground command waiting, the beat due.
  rig.advance(5000);
  ok = true;
  rig.health(report::kModeOperate);
  while (rig.tick(later)) {   // clear any beat
    rig.answer(later.sequence, answer::kDone, 0);
  }
  rig.advance(5000);
  ok = rig.fromGround(kDest, 90, armPayload(command::kSwitch, 91));
  (void)rig.groundAnswers();
  ok = ok && rig.fromGround(kDest, 91, {command::kSwitch}) && rig.tick(arm) && arm.payload.size() == 6;
  ok = ok && rig.fromGround(kDest, 92, {command::kGetStatus});
  rig.health(report::kModeTrial);   // the test image is now due; the beat is due; a command waits
  rig.advance(1500);   // past the beat period (1 s), inside the ARM's 2 s answer time
  ok = ok && quiet(rig);
  rig.answer(arm.sequence, answer::kDone, 0);
  ok = ok && rig.tick(cmd) && isCommand(cmd, {command::kSwitch}) && cmd.sequence == arm.sequence + 1;
  report(ok, "arm: with a test image due, a command waiting and the beat due, the frame after the ARM is still the SWITCH");
}

}  // namespace

/**
 * @brief Runs every test and prints the count.
 *
 * Usage: test_updater CONTROLLER_IMAGES TEST_IMAGES FLIPPED_IMAGE SCRATCH_DIR. With any other
 * number of arguments it prints the usage line to stderr and returns 2. Otherwise it prints
 * PASS or FAIL per test and then "unit tests: N of M passed".
 *
 * @param[in]     argc  number of command-line arguments; 5 expected
 * @param[in]     argv  program name, then CONTROLLER_IMAGES, TEST_IMAGES, FLIPPED_IMAGE, SCRATCH_DIR
 * @return exit status: 0 if every test passed; 1 if a test failed; 2 on a wrong argument count
 */
int main(int argc, char ** argv)
{
  if (argc != 5) {
    std::fprintf(stderr, "usage: %s CONTROLLER_IMAGES TEST_IMAGES FLIPPED_IMAGE SCRATCH_DIR\n", argv[0]);
    return 2;
  }
  logging::start(0, false);
  testCrc();
  testKnownFrames();
  testEncodeAgainstFrameC();
  testDecode();
  testParserAgainstFrameC();
  testImages(argv[1], argv[2], argv[3]);
  testUploadStore(argv[1], argv[2], argv[4]);
  testDelivery();
  testTestImageBytes();
  testTestImageTrigger();
  testTestImageSending(argv[4]);
  testTestImageWaitsForDelivery(argv[1], argv[4]);
  testArmPairing(argv[4]);
  testReports();
  testDataLink();
  testTables();
  testOptions();
  testKeyFile(argv[4]);
  std::printf("unit tests: %d of %d passed\n", g_tests - g_failures, g_tests);
  return g_failures == 0 ? 0 : 1;
}
