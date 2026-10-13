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
 *   options       the command line, including --restore-script
 *   restart       RestartRunner on mock scripts (result lines, deadline, TERM, KILL, never blocks);
 *                 the Updater's trial-overrun path with a mock restore.sh (decision H); ARM with no
 *                 answer (decision S); names and decoded event detail
 *
 * Usage: test_updater CONTROLLER_IMAGES TEST_IMAGES FLIPPED_IMAGE SCRATCH_DIR
 * Prints PASS or FAIL per test; exit status 1 on a failure.
 */
#include <algorithm>
#include <array>
#include <fcntl.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
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
#include "clock.hpp"
#include "command_gate.hpp"
#include "config.hpp"
#include "controller_watch.hpp"
#include "crc32.hpp"
#include "data_link.hpp"
#include "frame.hpp"
#include "image_checker.hpp"
#include "image_file_check.hpp"
#include "keys.h"
#include "log.hpp"
#include "options.hpp"
#include "protocol.hpp"
#include "report_store.hpp"
#include "restart_runner.hpp"
#include "serial_port.hpp"
#include "test_image.hpp"
#include "trial_watch.hpp"
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
    chunks_ok = chunks_ok && d.onFinalAnswer(answer::kDone, 0, code) == Delivery::Step::kContinue;
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

  // Owner's decision 4: ALREADY DONE to a step is not success. The updater never resends a
  // step, so it can only mean a sequence-number clash; it ends the delivery, refused 0x04.
  d.start(9, 600, 44, 0);
  report(d.onFinalAnswer(answer::kAlreadyDone, 0, code) == Delivery::Step::kRefused &&
         code == answer::kOldCommand && !d.active(),
    "ALREADY DONE to UPLOAD BEGIN: delivery refused 0x04, not carried on");
  d.start(9, 600, 44, 0);
  (void)d.onFinalAnswer(answer::kDone, 0, code);
  report(d.onFinalAnswer(answer::kAlreadyDone, 0, code) == Delivery::Step::kRefused &&
         code == answer::kOldCommand,
    "ALREADY DONE to a CHUNK: delivery refused 0x04");
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
  bool fail_send = false;   // the device is missing: sendBytes fails

  /**
   * @brief Keeps the bytes the code under test sends.
   *
   * @param[in]     data  the bytes to send
   * @param[in]     size  number of bytes
   * @return true (the bytes are appended to sent), or false when fail_send is set
   */
  bool sendBytes(const uint8_t * data, size_t size) override
  {
    if (fail_send) {
      return false;
    }
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
    pushed = pushed && queue.push(c, false, 0);
  }
  report(pushed && queue.full() && !queue.push(c, false, 0), "forward queue: holds 8, refuses a ninth");
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
   * @param[in]     restart_script  path of the restart script the Updater runs on a trial overrun
   *                                (decision H); empty: none configured
   */
  UpdaterRig(const std::string & dir, uint32_t beat_ms, const std::string & restart_script = "")
  : transport_(), link_(transport_), gate_(kPort), store_(dir.c_str()),
    checker_(PUBLIC_KEY_A, PUBLIC_KEY_B), reports_(), watch_(5000, 0), script_(restart_script),
    updater_(gate_, link_, store_, checker_, reports_, watch_, beat_ms, script_.c_str()), client_(-1), now_(1000),
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

  /** @brief Makes data link A's device fail (or work again). @param[in] fail true: sends fail @return nothing */
  void failSending(bool fail) { transport_.fail_send = fail; }

  /** @brief The reports the ground has received since the last call, payloads. @return the payloads */
  std::vector<std::vector<uint8_t>> groundReports()
  {
    (void)groundAnswers();
    std::vector<std::vector<uint8_t>> r;
    r.swap(reports_seen_);
    return r;
  }

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
      } else if (n > 0 && frame::decode(buffer.data(), static_cast<size_t>(n), f) &&
                 f.type == frame::kTypeReport) {
        reports_seen_.emplace_back(f.payload, f.payload + f.length);
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
  std::string script_;   // before updater_: the Updater keeps the pointer
  Updater updater_;
  int client_;
  uint64_t now_;
  uint64_t listen_;
  uint32_t next_sequence_;
  frame::Parser parser_;
  std::vector<SentFrame> pending_;
  std::vector<std::vector<uint8_t>> reports_seen_;
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
  report(ok && g.size() == 1 && has(g, 7, answer::kAlreadyDone, 0) && quiet(rig),
    "arm: the same SWITCH 7 again: ALREADY DONE (a resend of a finished command; REQ-031, owner 13 Oct), nothing sent");

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

// ---- Safety-audit fixes ------------------------------------------------------------------------
// One block per owner decision; each test failed on the code before the fix (see the report).

/**
 * @brief The payload of an ARM naming a dangerous command and its ground sequence number.
 *
 * @param[in]     id        the dangerous command's identifier
 * @param[in]     sequence  that command's ground sequence number
 * @return the ARM payload (Interfaces Sec. 5.1: 0x0D, identifier, sequence)
 */
std::vector<uint8_t> armOf(uint8_t id, uint32_t sequence)
{
  std::vector<uint8_t> p = {command::kArm, id, 0, 0, 0, 0};
  bytes::writeU32(&p[2], sequence);
  return p;
}

/**
 * @brief Tells whether the ground got this answer.
 *
 * @param[in]     v         the answers read
 * @param[in]     answered  ground sequence number
 * @param[in]     kind      answer kind
 * @param[in]     code      answer code
 * @return true: found; false: not
 */
bool hasAnswer(const std::vector<GroundAnswer> & v, uint32_t answered, uint8_t kind, uint8_t code)
{
  for (const GroundAnswer & a : v) {
    if (a.answered == answered && a.kind == kind && a.code == code) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Stores a whole image through the rig's store (BEGIN, CHUNKs, END).
 *
 * @param[in,out] rig     the updater under test
 * @param[in]     number  image number
 * @param[in]     image   the image bytes
 * @return true: stored and checked
 */
bool storeThrough(UpdaterRig & rig, uint16_t number, const std::vector<uint8_t> & image)
{
  uint32_t resume = 0;
  ImageVerdict verdict{};
  return rig.store().begin(number, static_cast<uint32_t>(image.size()), resume) == 0 &&
         sendChunks(rig.store(), number, image, 0, image.size()) &&
         rig.store().end(number, rig.checker(), verdict) == 0;
}

/**
 * @brief Decision 1: an ARM that waited in the queue past 30 s is not sent; a dangerous command is refused while a delivery or the test image is in the way.
 *
 * @param[in]     controller_images  directory with firmware-B.img
 * @param[in]     scratch            scratch directory
 * @return nothing
 */
void testArmAge(const std::string & controller_images, const std::string & scratch)
{
  const uint32_t kDest = frame::kDestController;
  SentFrame f{};
  {
    std::string dir = scratch + "/audit-age";
    removeTree(dir);
    UpdaterRig rig(dir, 0);
    if (!rig.open()) { report(false, "arm age: rig did not open"); return; }
    // A ground command occupies data link A; the ARM and SWITCH wait behind it.
    bool ok = rig.fromGround(kDest, 1, {command::kGetStatus}) && rig.tick(f);
    ok = ok && rig.fromGround(kDest, 2, armOf(command::kSwitch, 3)) &&
         rig.fromGround(kDest, 3, {command::kSwitch});
    (void)rig.groundAnswers();
    rig.advance(config::kArmExpiryMs + 1000);   // the first command times out; the pair's turn comes late
    bool sent_arm = rig.tick(f);
    std::vector<GroundAnswer> g = rig.groundAnswers();
    report(ok && !sent_arm && hasAnswer(g, 3, answer::kRefused, answer::kArmExpired),
      "arm age: an ARM older than 30 s when its turn comes: REFUSED 0x02, nothing sent to the controller");
    report(!rig.tick(f), "arm age: and the SWITCH is dropped, not sent later");
  }
  {
    std::string dir = scratch + "/audit-busy";
    removeTree(dir);
    UpdaterRig rig(dir, 0);
    if (!rig.open()) { report(false, "arm busy: rig did not open"); return; }
    std::vector<uint8_t> image = readFile(controller_images + "/firmware-B.img");
    bool ok = storeThrough(rig, 3, image);
    rig.health(report::kModeOperate);
    ok = ok && rig.fromGround(frame::kDestOnboard, 1, {command::kDeliver, 3, 0}) && rig.tick(f);
    (void)rig.groundAnswers();
    ok = ok && rig.fromGround(kDest, 10, armOf(command::kSwitch, 11));
    (void)rig.groundAnswers();
    ok = ok && rig.fromGround(kDest, 11, {command::kSwitch});
    std::vector<GroundAnswer> g = rig.groundAnswers();
    report(ok && hasAnswer(g, 11, answer::kRefused, answer::kNotInThisMode),
      "arm busy: SWITCH while a delivery runs: REFUSED 0x05, not queued");
    // Finish the delivery; no ARM or SWITCH may follow it.
    bool only_delivery = true;
    for (int step = 0; step < 400; step++) {
      uint8_t id = f.payload[0];
      rig.answer(f.sequence, answer::kDone, 0);
      if (id == command::kUploadEnd) { break; }
      only_delivery = rig.tick(f) && only_delivery && f.payload[0] != command::kArm;
    }
    report(only_delivery && quiet(rig), "arm busy: nothing of the refused SWITCH goes to the controller after the delivery");
  }
  {
    std::string dir = scratch + "/audit-busy2";
    removeTree(dir);
    UpdaterRig rig(dir, 0);
    if (!rig.open()) { report(false, "arm busy test image: rig did not open"); return; }
    rig.health(report::kModeTrial);   // the test image starts at the next tick
    bool ok = rig.tick(f) && f.payload[0] == command::kUploadBegin;
    ok = ok && rig.fromGround(kDest, 10, armOf(command::kSwitch, 11));
    (void)rig.groundAnswers();
    ok = ok && rig.fromGround(kDest, 11, {command::kSwitch});
    report(ok && hasAnswer(rig.groundAnswers(), 11, answer::kRefused, answer::kNotInThisMode),
      "arm busy: SWITCH while the test image runs: REFUSED 0x05");
  }
}

/**
 * @brief Decision 2: the ground cannot send UPLOAD commands or unknown identifiers to the controller.
 *
 * @param[in]     scratch  scratch directory
 * @return nothing
 */
void testControllerAllowedList(const std::string & scratch)
{
  std::string dir = scratch + "/audit-allow";
  removeTree(dir);
  UpdaterRig rig(dir, 0);
  if (!rig.open()) { report(false, "allowed list: rig did not open"); return; }
  const uint32_t kDest = frame::kDestController;
  SentFrame f{};
  const std::vector<std::vector<uint8_t>> refused_payloads = {
    {command::kUploadBegin, 7, 0, 0, 0, 0, 0},
    {command::kUploadChunk, 7, 0, 0, 0, 0, 0, 1},
    {command::kUploadEnd, 7, 0},
    {command::kUploadCancel, 7, 0},
    {command::kDeliver, 7, 0},
    {0x3F, 1, 2, 3},
    {0x40},
    {0x1F},
    {},
    {command::kGetStatus, 0},          // too long
    {command::kSwitch, 0},             // too long
    {command::kStartFailed},           // too short
    {command::kSetControllerTime, 1, 2, 3},   // too short
  };
  bool all_refused = true;
  bool none_sent = true;
  uint32_t seq = 100;
  for (const std::vector<uint8_t> & payload : refused_payloads) {
    bool got = rig.fromGround(kDest, seq, payload);
    all_refused = all_refused && got && hasAnswer(rig.groundAnswers(), seq, answer::kRefused, 0x06);
    none_sent = none_sent && quiet(rig);
    seq++;
  }
  report(all_refused, "allowed list: UPLOAD commands, unknown identifiers, an empty payload and wrong lengths to Destination 0x02: REFUSED 0x06");
  report(none_sent, "allowed list: and none of them reaches data link A");
  const std::vector<std::vector<uint8_t>> passed = {
    {command::kLeaveFallback}, {command::kGetStatus}};
  bool ok = true;
  for (const std::vector<uint8_t> & payload : passed) {
    ok = ok && rig.fromGround(kDest, seq++, payload) && rig.tick(f) && f.payload == payload;
    rig.answer(f.sequence, answer::kDone, 0);
  }
  report(ok, "allowed list: LEAVE FALLBACK and GET STATUS with the right length are passed on");
}

/**
 * @brief Decision 4: ALREADY DONE to a delivery step or an ARM is a refusal.
 *
 * @param[in]     controller_images  directory with firmware-B.img
 * @param[in]     scratch            scratch directory
 * @return nothing
 */
void testAlreadyDone(const std::string & controller_images, const std::string & scratch)
{
  std::string dir = scratch + "/audit-already";
  removeTree(dir);
  bool ok = false;
  {
  UpdaterRig rig(dir, 0);
  if (!rig.open()) { report(false, "already done: rig did not open"); return; }
  std::vector<uint8_t> image = readFile(controller_images + "/firmware-B.img");
  ok = storeThrough(rig, 3, image);
  rig.health(report::kModeOperate);
  SentFrame f{};
  ok = ok && rig.fromGround(frame::kDestOnboard, 1, {command::kDeliver, 3, 0}) && rig.tick(f);
  (void)rig.groundAnswers();
  rig.answer(f.sequence, answer::kAlreadyDone, 0);
  std::vector<GroundAnswer> g = rig.groundAnswers();
  report(ok && hasAnswer(g, 1, answer::kRefused, answer::kOldCommand) &&
         !hasAnswer(g, 1, answer::kDone, 0) && quiet(rig),
    "already done: ALREADY DONE to UPLOAD BEGIN of a delivery: the ground's DELIVER REFUSED 0x04, nothing more sent");
  }

  std::string dir2 = scratch + "/audit-already2";
  removeTree(dir2);
  UpdaterRig rig2(dir2, 0);
  if (!rig2.open()) { report(false, "already done (ARM): rig did not open"); return; }
  const uint32_t kDest = frame::kDestController;
  ok = rig2.fromGround(kDest, 20, armOf(command::kSwitch, 21)) &&
       rig2.fromGround(kDest, 21, {command::kSwitch});
  (void)rig2.groundAnswers();
  SentFrame arm{};
  ok = ok && rig2.tick(arm) && arm.payload[0] == command::kArm;
  rig2.answer(arm.sequence, answer::kAlreadyDone, 0);
  report(ok && quiet(rig2) && hasAnswer(rig2.groundAnswers(), 21, answer::kRefused, answer::kOldCommand),
    "already done: ALREADY DONE to the ARM: the SWITCH is not sent, the ground is refused 0x04");
}

/**
 * @brief Decision 5: a ground sequence number used by an accepted ARM or dangerous command is refused 0x04; the memory is the last 64.
 *
 * @param[in]     scratch  scratch directory
 * @return nothing
 */
void testUsedSequences(const std::string & scratch)
{
  std::string dir = scratch + "/audit-used";
  removeTree(dir);
  UpdaterRig rig(dir, 0);
  if (!rig.open()) { report(false, "used sequences: rig did not open"); return; }
  const uint32_t kDest = frame::kDestController;
  SentFrame arm{};
  SentFrame cmd{};
  bool ok = rig.fromGround(kDest, 10, armOf(command::kSwitch, 11)) &&
            rig.fromGround(kDest, 11, {command::kSwitch}) && rig.tick(arm);
  rig.answer(arm.sequence, answer::kDone, 0);
  ok = ok && rig.tick(cmd);
  rig.answer(cmd.sequence, answer::kDone, 0);
  (void)rig.groundAnswers();
  report(ok, "used sequences: ARM 10 and SWITCH 11 go through once");
  // The captured pair sent again.
  ok = rig.fromGround(kDest, 10, armOf(command::kSwitch, 11));
  std::vector<GroundAnswer> g = rig.groundAnswers();
  report(ok && g.size() == 1 && hasAnswer(g, 10, answer::kAlreadyDone, 0),
    "used sequences: the same ARM again (same bytes): ALREADY DONE, not held (REQ-031, owner 13 Oct)");
  ok = rig.fromGround(kDest, 11, {command::kSwitch});
  g = rig.groundAnswers();
  report(ok && hasAnswer(g, 11, answer::kAlreadyDone, 0) && quiet(rig),
    "used sequences: the captured SWITCH again (same bytes): ALREADY DONE, nothing sent");
  // A fresh ARM for the old command's number does not bring it back.
  ok = rig.fromGround(kDest, 12, armOf(command::kSwitch, 11)) &&
       rig.fromGround(kDest, 11, {command::kSwitch});
  report(ok && hasAnswer(rig.groundAnswers(), 11, answer::kAlreadyDone, 0) && quiet(rig),
    "used sequences: a new ARM naming the used sequence number 11 does not let SWITCH 11 run again");
  // The ring holds 64: after 64 newer ones, number 10 is forgotten.
  for (uint32_t i = 0; i < config::kUsedSequenceCount; i++) {
    (void)rig.fromGround(kDest, 1000 + i, armOf(command::kSwitch, 5000 + i));
  }
  (void)rig.groundAnswers();
  ok = rig.fromGround(kDest, 10, armOf(command::kSwitch, 11));
  g = rig.groundAnswers();
  report(ok && hasAnswer(g, 10, answer::kDone, 0),
    "used sequences: the memory is the last 64 (bench choice): after 64 newer ones the oldest is forgotten");
  UsedSequences ring;
  for (uint32_t i = 0; i < 100; i++) {
    const uint8_t byte = static_cast<uint8_t>(i);
    ok = ring.add(i, &byte, 1) && ok;
  }
  const uint8_t b35 = 35;
  const uint8_t b36 = 36;
  const uint8_t b99 = 99;
  const uint8_t b100 = 100;
  using Repeat = UsedSequences::Repeat;
  report(ok && ring.check(35, &b35, 1) == Repeat::kNew && ring.check(36, &b36, 1) == Repeat::kSamePending &&
         ring.check(99, &b99, 1) == Repeat::kSamePending && ring.check(100, &b100, 1) == Repeat::kNew,
    "used sequences: ring keeps exactly the last 64 of 100");
  const std::array<uint8_t, config::kUsedPayloadMax + 1> too_long{};
  report(!ring.add(500, too_long.data(), static_cast<uint16_t>(too_long.size())) &&
         ring.check(500, too_long.data(), 1) == Repeat::kNew,
    "used sequences: a payload longer than the ring keeps is not stored");
}

/**
 * @brief REQ-031: an ARM or dangerous command sent again after a lost answer runs once.
 *
 * The ground console resends a command with the same sequence number and the same bytes when
 * its answer is lost (Interfaces Sec. 4.2). Cases: (1) a finished ARM and SWITCH sent again are
 * answered ALREADY DONE and nothing reaches the controller a second time; (2) a SWITCH sent
 * again while still queued, while its ARM is outstanding and while it is outstanding is answered
 * RECEIVED only, and the controller's one final answer follows; (3) the same number with other
 * bytes is REFUSED 0x04 and changes nothing; (4) a first one that ended REFUSED is not turned
 * into ALREADY DONE by a resend (REFUSED 0x04), after the controller refused the command and
 * after it refused the ARM.
 *
 * @param[in]     scratch  scratch directory
 * @return nothing
 */
void testResendAfterLostAnswer(const std::string & scratch)
{
  const uint32_t kDest = frame::kDestController;
  auto count = [](const std::vector<GroundAnswer> & v, uint32_t answered, uint8_t kind) {
    size_t n = 0;
    for (const GroundAnswer & a : v) {
      n += (a.answered == answered && a.kind == kind) ? 1U : 0U;
    }
    return n;
  };
  auto finals = [&count](const std::vector<GroundAnswer> & v, uint32_t answered) {
    return count(v, answered, answer::kDone) + count(v, answered, answer::kRefused) +
           count(v, answered, answer::kAlreadyDone);
  };

  // 1. ARM and SWITCH answered, the answers "lost", both sent again.
  std::string dir = scratch + "/resend1";
  removeTree(dir);
  {
    UpdaterRig rig(dir, 0);
    if (!rig.open()) { report(false, "resend: rig did not open"); return; }
    std::vector<SentFrame> sent;
    SentFrame f{};
    bool ok = rig.fromGround(kDest, 10, armOf(command::kSwitch, 11)) &&
              rig.fromGround(kDest, 11, {command::kSwitch}) && rig.tick(f);
    sent.push_back(f);
    rig.answer(f.sequence, answer::kDone, 0);
    ok = ok && rig.tick(f);
    sent.push_back(f);
    rig.answer(f.sequence, answer::kDone, 0);
    (void)rig.groundAnswers();   // the answers the ground never saw
    ok = ok && rig.fromGround(kDest, 10, armOf(command::kSwitch, 11));
    std::vector<GroundAnswer> g = rig.groundAnswers();
    report(ok && g.size() == 1 && hasAnswer(g, 10, answer::kAlreadyDone, 0) && quiet(rig),
      "resend: the finished ARM sent again: ALREADY DONE only, nothing sent to the controller");
    ok = rig.fromGround(kDest, 11, {command::kSwitch});
    g = rig.groundAnswers();
    report(ok && g.size() == 1 && hasAnswer(g, 11, answer::kAlreadyDone, 0) && quiet(rig),
      "resend: the finished SWITCH sent again: ALREADY DONE only, nothing sent to the controller");
    size_t arms = 0;
    size_t switches = 0;
    for (const SentFrame & s : sent) {
      arms += (s.payload[0] == command::kArm) ? 1U : 0U;
      switches += (s.payload[0] == command::kSwitch) ? 1U : 0U;
    }
    report(sent.size() == 2 && arms == 1 && switches == 1,
      "resend: the controller got the ARM and the SWITCH exactly once");
  }

  // 2. SWITCH sent again while it is queued, while its ARM is out, and while it is out.
  dir = scratch + "/resend2";
  removeTree(dir);
  {
    UpdaterRig rig(dir, 0);
    if (!rig.open()) { report(false, "resend: rig 2 did not open"); return; }
    std::vector<SentFrame> sent;
    SentFrame f{};
    // GET STATUS from the ground keeps data link A busy, so the SWITCH waits in the queue.
    bool ok = rig.fromGround(kDest, 5, {command::kGetStatus}) && rig.tick(f);
    SentFrame status = f;
    ok = ok && rig.fromGround(kDest, 20, armOf(command::kSwitch, 21)) &&
         rig.fromGround(kDest, 21, {command::kSwitch});
    (void)rig.groundAnswers();
    ok = ok && quiet(rig);
    ok = ok && rig.fromGround(kDest, 21, {command::kSwitch});
    std::vector<GroundAnswer> g = rig.groundAnswers();
    report(ok && g.size() == 1 && hasAnswer(g, 21, answer::kReceived, 0) && quiet(rig),
      "resend: SWITCH sent again while queued: RECEIVED only, nothing sent");
    rig.answer(status.sequence, answer::kDone, 0);
    ok = rig.tick(f) && f.payload[0] == command::kArm;
    sent.push_back(f);
    SentFrame arm = f;
    (void)rig.groundAnswers();
    ok = ok && rig.fromGround(kDest, 21, {command::kSwitch});
    g = rig.groundAnswers();
    report(ok && g.size() == 1 && hasAnswer(g, 21, answer::kReceived, 0) && quiet(rig),
      "resend: SWITCH sent again while its ARM is outstanding: RECEIVED only, nothing sent");
    rig.answer(arm.sequence, answer::kDone, 0);
    ok = rig.tick(f) && f.payload[0] == command::kSwitch;
    sent.push_back(f);
    SentFrame sw = f;
    ok = ok && rig.fromGround(kDest, 21, {command::kSwitch});
    g = rig.groundAnswers();
    report(ok && g.size() == 1 && hasAnswer(g, 21, answer::kReceived, 0) && quiet(rig),
      "resend: SWITCH sent again while it is outstanding: RECEIVED only, nothing sent");
    rig.answer(sw.sequence, answer::kReceived, 0);
    rig.answer(sw.sequence, answer::kDone, 0);
    g = rig.groundAnswers();
    report(finals(g, 21) == 1 && hasAnswer(g, 21, answer::kDone, 0),
      "resend: exactly one final answer, DONE, follows for SWITCH 21");
    size_t switches = 0;
    for (const SentFrame & s : sent) {
      switches += (s.payload[0] == command::kSwitch) ? 1U : 0U;
    }
    ok = rig.fromGround(kDest, 21, {command::kSwitch});
    g = rig.groundAnswers();
    report(switches == 1 && ok && g.size() == 1 && hasAnswer(g, 21, answer::kAlreadyDone, 0) && quiet(rig),
      "resend: the controller got SWITCH 21 once; sent again after DONE: ALREADY DONE");
  }

  // 3. The same number with other bytes.
  dir = scratch + "/resend3";
  removeTree(dir);
  {
    UpdaterRig rig(dir, 0);
    if (!rig.open()) { report(false, "resend: rig 3 did not open"); return; }
    SentFrame f{};
    bool ok = rig.fromGround(kDest, 30, armOf(command::kSwitch, 31));
    (void)rig.groundAnswers();
    ok = ok && rig.fromGround(kDest, 30, armOf(command::kSwitch, 32));
    std::vector<GroundAnswer> g = rig.groundAnswers();
    report(ok && g.size() == 1 && hasAnswer(g, 30, answer::kRefused, answer::kOldCommand) && quiet(rig),
      "resend: ARM 30 again naming another sequence number: REFUSED 0x04");
    ok = rig.fromGround(kDest, 31, {command::kSwitch}) && rig.tick(f) && f.payload[0] == command::kArm;
    SentFrame arm = f;
    (void)rig.groundAnswers();
    report(ok, "resend: the refused ARM replaced nothing: SWITCH 31 still goes out with its ARM");
    ok = rig.fromGround(kDest, 31, {command::kReturnToPrevious});
    g = rig.groundAnswers();
    report(ok && g.size() == 1 && hasAnswer(g, 31, answer::kRefused, answer::kOldCommand) && quiet(rig),
      "resend: number 31 again with another command: REFUSED 0x04");
    rig.answer(arm.sequence, answer::kDone, 0);
    ok = rig.tick(f) && f.payload[0] == command::kSwitch;
    rig.answer(f.sequence, answer::kDone, 0);
    (void)rig.groundAnswers();
    ok = ok && rig.fromGround(kDest, 31, {command::kSwitch});
    g = rig.groundAnswers();
    report(ok && g.size() == 1 && hasAnswer(g, 31, answer::kAlreadyDone, 0) && quiet(rig),
      "resend: the refused repeat did not change the first one: SWITCH 31 again is ALREADY DONE");
  }

  // 4. A first one that ended REFUSED is never ALREADY DONE.
  dir = scratch + "/resend4";
  removeTree(dir);
  {
    UpdaterRig rig(dir, 0);
    if (!rig.open()) { report(false, "resend: rig 4 did not open"); return; }
    SentFrame f{};
    bool ok = rig.fromGround(kDest, 40, armOf(command::kSwitch, 41)) &&
              rig.fromGround(kDest, 41, {command::kSwitch}) && rig.tick(f);
    rig.answer(f.sequence, answer::kDone, 0);
    ok = ok && rig.tick(f);
    rig.answer(f.sequence, answer::kRefused, answer::kNotInThisMode);
    (void)rig.groundAnswers();
    ok = ok && rig.fromGround(kDest, 41, {command::kSwitch});
    std::vector<GroundAnswer> g = rig.groundAnswers();
    report(ok && g.size() == 1 && hasAnswer(g, 41, answer::kRefused, answer::kOldCommand) && quiet(rig),
      "resend: SWITCH refused by the controller, sent again: REFUSED 0x04, not ALREADY DONE");
    ok = rig.fromGround(kDest, 50, armOf(command::kSwitch, 51)) &&
         rig.fromGround(kDest, 51, {command::kSwitch}) && rig.tick(f);
    rig.answer(f.sequence, answer::kRefused, answer::kNotInThisMode);
    (void)rig.groundAnswers();
    ok = ok && rig.fromGround(kDest, 51, {command::kSwitch});
    g = rig.groundAnswers();
    report(ok && g.size() == 1 && hasAnswer(g, 51, answer::kRefused, answer::kOldCommand) && quiet(rig),
      "resend: SWITCH whose ARM the controller refused, sent again: REFUSED 0x04, not RECEIVED");
  }
}

/**
 * @brief Decision 6: a restarted trial (a second EVENT 0x0006) gets the test image again; the first does not make it twice.
 *
 * @param[in]     scratch  scratch directory
 * @return nothing
 */
void testTestImageOnEvent(const std::string & scratch)
{
  TestImageTrigger t;
  t.onHealthMode(report::kModeTrial);
  t.markStarted();
  t.onTrialStartedEvent();
  report(!t.due(), "trial event: the first EVENT 0x0006 after the HEALTH that began the trial: not due again");
  t.onTrialStartedEvent();
  report(t.due(), "trial event: a second EVENT 0x0006 in the same trial (restarted): due");
  t.markStarted();
  t.onHealthMode(report::kModeOperate);
  t.onTrialStartedEvent();
  t.onHealthMode(report::kModeTrial);
  t.markStarted();
  t.onTrialStartedEvent();
  report(t.due(), "trial event: EVENT before HEALTH in one trial, then a restart EVENT: due");

  std::string dir = scratch + "/audit-event";
  removeTree(dir);
  UpdaterRig rig(dir, 0);
  if (!rig.open()) { report(false, "trial event: rig did not open"); return; }
  auto event = [&rig](uint16_t code) {
    std::vector<uint8_t> p(report::kEventMinBytes, 0);
    p[report::kOffsetKind] = report::kEvent;
    bytes::writeU16(&p[report::kOffsetEventCode], code);
    rig.fromController(frame::kTypeReport, p);
  };
  SentFrame f{};
  rig.health(report::kModeTrial);
  bool ok = rig.tick(f) && f.payload[0] == command::kUploadBegin;
  for (int i = 0; i < 6 && ok; i++) {   // BEGIN, 4 CHUNKs, END
    rig.answer(f.sequence, answer::kDone, 0);
    if (i < 5) { ok = rig.tick(f); }
  }
  event(report::kEventTrialStarted);
  report(ok && quiet(rig), "trial event: image sent for the trial; the first EVENT 0x0006 sends nothing more");
  event(report::kEventTrialStarted);
  report(rig.tick(f) && f.payload[0] == command::kUploadBegin,
    "trial event: a restarted trial (second EVENT 0x0006): the test image is sent again");
}

/**
 * @brief Decision H (accepted by the owner, 13 Oct 2026): the trial watch's timing rules.
 *
 * The trial time (120 s) and margin (30 s); one overrun reported once; HEALTH reports that keep
 * showing TRIAL do not restart the timing; EVENT 0x0007 and 0x0008 end it; EVENT 0x0006 starts it.
 *
 * @return nothing
 */
void testTrialWatch()
{
  TrialWatch w(config::kTrialMs, config::kTrialMarginMs);
  w.onHealthMode(report::kModeTrial, 1000);
  report(!w.expired(1000 + 150000), "trial watch: exactly trial time + margin (150 s): not expired");
  report(w.expired(1000 + 150001) && !w.expired(1000 + 160000), "trial watch: 150.001 s: expired, once");
  w.onHealthMode(report::kModeTrial, 200000);
  report(!w.expired(200000 + 999999), "trial watch: HEALTH reports still showing TRIAL do not start a new timing");
  w.onHealthMode(report::kModeOperate, 300000);
  w.onHealthMode(report::kModeTrial, 300000);
  w.onEvent(report::kEventFirmwareConfirmed, 310000);
  report(!w.expired(300000 + 400000), "trial watch: EVENT 0x0007 ends the timing");
  w.onEvent(report::kEventTrialStarted, 1000000);
  w.onEvent(report::kEventFirmwareGivenUp, 1010000);
  report(!w.expired(1000000 + 400000), "trial watch: EVENT 0x0008 ends the timing");
  w.onEvent(report::kEventTrialStarted, 2000000);
  report(w.expired(2000000 + 150001), "trial watch: EVENT 0x0006 starts the timing");
}

/**
 * @brief Writes an executable sh script.
 *
 * @param[in]     path  where to write it
 * @param[in]     body  the script's text after the "#!/bin/sh" line
 * @param[in]     mode  file mode, e.g. 0755; 0644 makes a script that cannot be executed
 * @return true: written; false: the file could not be written
 */
bool writeScript(const std::string & path, const std::string & body, mode_t mode)
{
  std::FILE * f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    return false;
  }
  std::string text = "#!/bin/sh\n" + body;
  bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  ok = (std::fclose(f) == 0) && ok;
  return ok && ::chmod(path.c_str(), mode) == 0;
}

/**
 * @brief Tells whether a file's first line equals text.
 *
 * @param[in]     path  file to read
 * @param[in]     text  expected first line, without the newline
 * @return true: the file exists and its first line is text
 */
bool firstLineIs(const std::string & path, const std::string & text)
{
  std::vector<uint8_t> data = readFile(path);
  std::string content(data.begin(), data.end());
  return content.substr(0, content.find('\n')) == text && !data.empty();
}

/**
 * @brief Polls a RestartRunner on the real clock until it ends or the limit passes.
 *
 * Sleeps 5 ms between polls: a bounded wait of the test, not of the runner.
 *
 * @param[in,out] runner    the runner, started
 * @param[in]     limit_ms  most real time to wait, ms
 * @param[out]    outcome   the outcome when true is returned
 * @return true: the child ended or was given up in time; false: still running at the limit
 */
bool runUntilEnded(RestartRunner & runner, uint64_t limit_ms, RestartRunner::Outcome & outcome)
{
  uint64_t start = clocks::nowMs();
  while (clocks::nowMs() - start <= limit_ms) {
    if (runner.poll(clocks::nowMs(), outcome)) {
      return true;
    }
    ::usleep(5000);
  }
  return false;
}

/**
 * @brief Tells whether a process is gone, waiting up to 2 s for it to disappear.
 *
 * @param[in]     pid  process id read from a file; 0 or negative counts as gone
 * @return true: kill(pid, 0) says no such process; false: still there after 2 s
 */
bool processGone(long pid)
{
  if (pid <= 0) {
    return true;
  }
  for (int i = 0; i < 400; i++) {
    if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
      return true;
    }
    ::usleep(5000);
  }
  return false;
}

/**
 * @brief Reads a process id written by a mock script into a file.
 *
 * @param[in]     path  the file
 * @return the id; 0 if the file is missing or empty
 */
long readPid(const std::string & path)
{
  std::vector<uint8_t> data = readFile(path);
  std::string text(data.begin(), data.end());
  return text.empty() ? 0 : std::atol(text.c_str());
}

/**
 * @brief RestartRunner (decision H): the result lines, the exit status, start errors, the deadline.
 *
 * Mock scripts in the scratch directory stand for restore.sh. Real child processes, the real
 * clock, deadlines of 150 ms. Passes when each result line gives its Result, a script that cannot
 * start gives its errno, start() and poll() return at once while the child runs, TERM comes after
 * the deadline, KILL reaches a script that ignores TERM and the sleep it started.
 *
 * @param[in]     scratch  scratch directory
 * @return nothing
 */
void testRestartRunner(const std::string & scratch)
{
  std::string dir = scratch + "/restart-runner";
  removeTree(dir);
  (void)::mkdir(dir.c_str(), 0755);
  using R = RestartRunner::Result;
  RestartRunner::Outcome o{};
  int error = 0;

  report(!RestartRunner(nullptr, 1000, 1000).configured() && !RestartRunner("", 1000, 1000).configured() &&
         RestartRunner("x", 1000, 1000).configured(), "restart runner: configured only with a non-empty path");

  // The script is called with the one argument "reset".
  std::string s = dir + "/done.sh";
  bool ok = writeScript(s, "echo \"$#:$1\" > " + dir + "/args\necho 'RESTORE DONE: reset'\n", 0755);
  RestartRunner a(s.c_str(), 5000, 150);
  ok = ok && a.start(clocks::nowMs(), error) && a.running() && runUntilEnded(a, 3000, o);
  report(ok && !a.running() && o.result == R::kDone && o.exit_code == 0 && o.term_signal == 0 &&
         std::strcmp(o.line.data(), "RESTORE DONE: reset") == 0 && firstLineIs(dir + "/args", "1:reset"),
    "restart runner: 'RESTORE DONE: reset', exit 0: kDone; called as 'script reset'");

  struct Case { const char * name; const char * body; R expected; };
  const Case cases[] = {
    {"done line but exit 1", "echo 'RESTORE DONE: reset'\nexit 1\n", R::kFailed},
    {"after reset", "echo 'RESTORE FAILED: after reset (time-out after 2 s)'\nexit 1\n", R::kFailedAfterReset},
    {"failed in connect", "echo 'RESTORE FAILED: connect (log: x)'\nexit 1\n", R::kFailed},
    {"failed in reset and run", "echo 'RESTORE FAILED: reset and run (log: x)'\nexit 1\n", R::kFailed},
    {"no line, exit 0", "exit 0\n", R::kFailed},
    {"DONE line of another word", "echo 'RESTORE DONE: resetting'\n", R::kFailed},
    {"a done line of another mode", "echo 'RESTORE DONE: restore: x'\n", R::kFailed},
    {"after-reset word prefix", "echo 'RESTORE FAILED: after resetting'\nexit 1\n", R::kFailed},
    {"line among noise", "echo noise\necho 'RESTORE DONE: reset'\n", R::kDone},
  };
  for (const Case & c : cases) {
    std::string path = dir + "/case.sh";
    RestartRunner r(path.c_str(), 5000, 150);
    bool good = writeScript(path, c.body, 0755) && r.start(clocks::nowMs(), error) &&
                runUntilEnded(r, 3000, o);
    report(good && o.result == c.expected && !o.deadline_hit, std::string("restart runner: ") + c.name);
  }

  // A chatty script: the newest bytes are kept, so the last line is still read.
  s = dir + "/chatty.sh";
  ok = writeScript(s, "i=0\nwhile [ $i -lt 400 ]; do echo 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'; i=$((i+1)); done\necho 'RESTORE DONE: reset'\n", 0755);
  RestartRunner chatty(s.c_str(), 5000, 150);
  ok = ok && chatty.start(clocks::nowMs(), error) && runUntilEnded(chatty, 3000, o);
  report(ok && o.result == R::kDone, "restart runner: 28 KB of output before the result line: the line is still read");

  // A script that kills itself: a signal, no line.
  s = dir + "/killed.sh";
  ok = writeScript(s, "kill -9 $$\n", 0755);
  RestartRunner killed(s.c_str(), 5000, 150);
  ok = ok && killed.start(clocks::nowMs(), error) && runUntilEnded(killed, 3000, o);
  report(ok && o.result == R::kFailed && o.exit_code == -1 && o.term_signal == SIGKILL,
    "restart runner: a script ended by a signal: kFailed, the signal reported");

  // Scripts that cannot start.
  RestartRunner missing((dir + "/no-such-script.sh").c_str(), 5000, 150);
  report(!missing.start(clocks::nowMs(), error) && error == ENOENT && !missing.running(),
    "restart runner: a missing script: start() false, ENOENT, nothing running");
  s = dir + "/not-executable.sh";
  ok = writeScript(s, "echo 'RESTORE DONE: reset'\n", 0644);
  RestartRunner noexec(s.c_str(), 5000, 150);
  report(ok && !noexec.start(clocks::nowMs(), error) && error == EACCES && !noexec.running(),
    "restart runner: a script without the execute bit: start() false, EACCES");
  RestartRunner none(nullptr, 5000, 150);
  report(!none.start(clocks::nowMs(), error) && !none.running(), "restart runner: no script configured: start() false");

  // Never blocks: start and poll return at once while the child sleeps.
  s = dir + "/slow.sh";
  ok = writeScript(s, "echo $$ > " + dir + "/slow.pid\nsleep 1\necho 'RESTORE DONE: reset'\n", 0755);
  RestartRunner slow(s.c_str(), 5000, 150);
  uint64_t t0 = clocks::nowMs();
  ok = ok && slow.start(t0, error);
  bool quick_poll = true;
  for (int i = 0; i < 20; i++) {
    quick_poll = quick_poll && !slow.poll(clocks::nowMs(), o);
  }
  uint64_t took = clocks::nowMs() - t0;
  bool second = slow.start(clocks::nowMs(), error);
  report(ok && quick_poll && took < 250 && slow.running() && !second && error == EBUSY,
    "restart runner: start() and 20 polls of a 1 s script took under 250 ms; a second start() is refused (EBUSY)");
  ok = runUntilEnded(slow, 3000, o);
  report(ok && o.result == R::kDone && clocks::nowMs() - t0 >= 900, "restart runner: and the result comes when the script ends");

  // Deadline: TERM to a script that obeys it.
  s = dir + "/obeys.sh";
  ok = writeScript(s, "echo $$ > " + dir + "/obeys.pid\nexec sleep 30\n", 0755);
  RestartRunner obeys(s.c_str(), 150, 150);
  t0 = clocks::nowMs();
  ok = ok && obeys.start(t0, error) && runUntilEnded(obeys, 4000, o);
  report(ok && o.deadline_hit && !o.killed && o.term_signal == SIGTERM && o.result == R::kFailed &&
         clocks::nowMs() - t0 < 2000 && processGone(readPid(dir + "/obeys.pid")),
    "restart runner: past the deadline the script gets TERM, is reaped, and the result is kFailed");

  // Deadline: KILL to a script that ignores TERM, and to the sleep it started.
  s = dir + "/stubborn.sh";
  ok = writeScript(s, "trap '' TERM\necho $$ > " + dir + "/stubborn.pid\nsleep 1000 &\necho $! > " + dir +
                      "/stubborn-child.pid\nwhile :; do sleep 1; done\n", 0755);
  RestartRunner stubborn(s.c_str(), 2500, 300);   // 2.5 s: a slow first start of sh (seen: over 600 ms) must still reach its trap
  t0 = clocks::nowMs();
  ok = ok && stubborn.start(t0, error) && runUntilEnded(stubborn, 8000, o);
  report(ok && o.deadline_hit && o.killed && !o.abandoned && o.term_signal == SIGKILL && o.result == R::kFailed &&
         processGone(readPid(dir + "/stubborn.pid")),
    "restart runner: a script that ignores TERM gets KILL after the grace");
  report(processGone(readPid(dir + "/stubborn-child.pid")),
    "restart runner: KILL goes to the whole process group: the sleep the script started is gone too");
  report(!stubborn.running() && !stubborn.poll(clocks::nowMs(), o), "restart runner: after the end, poll() is false and nothing runs");

  // The runner can be used again after an end.
  RestartRunner again((dir + "/done.sh").c_str(), 5000, 150);
  ok = again.start(clocks::nowMs(), error) && runUntilEnded(again, 3000, o) &&
       again.start(clocks::nowMs(), error) && runUntilEnded(again, 3000, o);
  report(ok && o.result == R::kDone, "restart runner: used twice in a row");
}

/**
 * @brief Waits for a mock restart script to finish, serving the Updater meanwhile.
 *
 * Calls tick and collects the ground's reports until the file named by done_file exists and then
 * for 150 ms more (reports from the end of the script), or until 5 s of real time pass.
 *
 * @param[in,out] rig        the updater under test
 * @param[in]     done_file  a file the mock script creates as its last act; "" to wait 300 ms only
 * @param[out]    reports    every report the ground received meanwhile
 * @return nothing
 */
void serveUntilScriptEnds(UpdaterRig & rig, const std::string & done_file,
  std::vector<std::vector<uint8_t>> & reports)
{
  SentFrame f{};
  uint64_t start = clocks::nowMs();
  uint64_t done_at = 0;
  while (clocks::nowMs() - start < 5000) {
    (void)rig.tick(f);
    for (std::vector<uint8_t> & r : rig.groundReports()) {
      reports.push_back(r);
    }
    if (done_at == 0 && (done_file.empty() ? clocks::nowMs() - start > 300 : exists(done_file))) {
      done_at = clocks::nowMs();
    }
    if (done_at != 0 && clocks::nowMs() - done_at > 150) {
      return;
    }
  }
}

/**
 * @brief Counts the reports that are EVENT reports with this code.
 *
 * @param[in]     reports  report payloads
 * @param[in]     code     event code
 * @return the count
 */
size_t countEvents(const std::vector<std::vector<uint8_t>> & reports, uint16_t code)
{
  size_t n = 0;
  for (const std::vector<uint8_t> & r : reports) {
    if (r.size() >= report::kEventMinBytes && r[report::kOffsetKind] == report::kEvent &&
        bytes::readU16(&r[report::kOffsetEventCode]) == code) {
      n++;
    }
  }
  return n;
}

/**
 * @brief Tells whether none of the reports is EVENT 0x0106 or the old EVENT 0x0009.
 *
 * @param[in]     reports  report payloads the ground received
 * @return true: neither event is among them (HEALTH reports and other events do not count)
 */
bool noRestartEvent(const std::vector<std::vector<uint8_t>> & reports)
{
  return countEvents(reports, report::kEventControllerRestarted) == 0 && countEvents(reports, 0x0009) == 0;
}

/**
 * @brief Decision H: the trial overruns, the Updater runs the restart script, and tells the ground.
 *
 * Mock scripts stand for "restore.sh reset"; the rig's clock is moved by the test, the script runs
 * in real time. Passes when: nothing at 150 s exactly; at 150.001 s the script starts, once, and
 * EVENT 0x0106 with detail 0x01 (and no EVENT 0x0009) reaches the ground only after the script
 * says the controller was reset ("RESTORE DONE: reset", or "RESTORE FAILED: after reset"); a script
 * that fails before the reset, is missing, is not configured or is already running sends nothing;
 * the main loop is not blocked while the script runs; the deadline stops a script that hangs.
 *
 * @param[in]     scratch  scratch directory
 * @return nothing
 */
void testTrialOverrunRestart(const std::string & scratch)
{
  std::string base = scratch + "/overrun";
  removeTree(base);
  (void)::mkdir(base.c_str(), 0755);

  // One run: a trial begins, the ground has spoken, the clock goes to trial time + margin (+1 ms),
  // the mock script runs. Returns the reports the ground got; marks are written into the result.
  struct Run
  {
    std::vector<std::vector<uint8_t>> reports;
    bool silent_at_150s;
    bool started;   // the script was called (its marker file exists)
    uint64_t overrun_tick_ms;   // real time the tick that started the script took
  };
  auto run = [&](const std::string & name, const std::string & script_body, bool make_script,
                 bool configured) -> Run {
    Run result{{}, false, false, 0};
    std::string dir = base + "/" + name;
    (void)::mkdir(dir.c_str(), 0755);
    removeTree(dir);   // files of an earlier run (the store's data directory is a subdirectory and stays)
    (void)::mkdir(dir.c_str(), 0755);
    std::string script = dir + "/restore.sh";
    if (make_script) {
      (void)writeScript(script, "echo called > " + dir + "/called\n" + script_body + "touch " + dir + "/finished\n", 0755);
    }
    std::string data = dir + "/data";   // a named string: the store keeps the pointer
    UpdaterRig rig(data, 0, configured ? script : "");
    if (!rig.open()) {
      return result;
    }
    SentFrame f{};
    (void)rig.fromGround(frame::kDestController, 1, {command::kGetStatus});   // a ground has spoken
    (void)rig.tick(f);
    rig.answer(f.sequence, answer::kDone, 0);
    rig.health(report::kModeTrial);
    (void)rig.groundReports();
    rig.advance(config::kTrialMs + config::kTrialMarginMs);
    (void)rig.tick(f);
    result.silent_at_150s = rig.groundReports().empty() && !exists(dir + "/called");
    rig.advance(2);
    uint64_t t0 = clocks::nowMs();
    (void)rig.tick(f);
    result.overrun_tick_ms = clocks::nowMs() - t0;
    serveUntilScriptEnds(rig, make_script && configured ? dir + "/finished" : "", result.reports);
    result.started = exists(dir + "/called");
    return result;
  };

  Run r = run("done", "echo 'RESTORE DONE: reset'\n", true, true);
  bool detail_ok = false;
  for (const std::vector<uint8_t> & p : r.reports) {
    if (p.size() == report::kEventMinBytes + 1 && p[report::kOffsetKind] == report::kEvent &&
        bytes::readU16(&p[report::kOffsetEventCode]) == report::kEventControllerRestarted &&
        p[report::kEventDetailOffset] == report::kRestartReasonTrialPastTime) {
      detail_ok = true;
    }
  }
  report(r.silent_at_150s, "trial overrun: at exactly 150 s nothing is run and nothing is sent");
  report(r.started && r.reports.size() == 1 && countEvents(r.reports, 0x0106) == 1 && detail_ok,
    "trial overrun: the script RESTORE DONE: reset -> exactly one report to the ground, EVENT 0x0106, 16 bytes, detail 0x01");
  report(countEvents(r.reports, 0x0009) == 0, "trial overrun: and no EVENT 0x0009 any more");
  report(r.overrun_tick_ms < 250, "trial overrun: the tick that starts the script returns at once (" + std::to_string(r.overrun_tick_ms) + " ms)");

  r = run("after", "echo 'RESTORE FAILED: after reset (time-out after 300 s, OpenOCD stopped; log: x)'\nexit 1\n", true, true);
  report(r.started && countEvents(r.reports, 0x0106) == 1 && r.reports.size() == 1,
    "trial overrun: the script RESTORE FAILED: after reset -> the reset happened: EVENT 0x0106 is sent");

  r = run("before", "echo 'RESTORE FAILED: connect (log: x)'\nexit 1\n", true, true);
  report(r.started && noRestartEvent(r.reports),
    "trial overrun: the script RESTORE FAILED: connect -> the reset is not shown: nothing is sent");
  r = run("silent", "exit 1\n", true, true);
  report(r.started && noRestartEvent(r.reports), "trial overrun: a script that fails with no line: nothing is sent");

  r = run("missing", "", false, true);
  report(r.silent_at_150s && !r.started && noRestartEvent(r.reports),
    "trial overrun: the script path does not exist: logged, nothing is sent, the Updater goes on");
  r = run("unconfigured", "", false, false);
  report(!r.started && noRestartEvent(r.reports), "trial overrun: no script configured: logged, nothing is sent");

  // The main loop is not blocked while the script runs: a ground command goes through.
  {
    std::string dir = base + "/loop";
    (void)::mkdir(dir.c_str(), 0755);
    removeTree(dir);
    (void)::mkdir(dir.c_str(), 0755);
    bool made = writeScript(dir + "/restore.sh", "sleep 1\necho 'RESTORE DONE: reset'\ntouch " + dir + "/finished\n", 0755);
    std::string data = dir + "/data";   // a named string: the store keeps the pointer
    std::string script = dir + "/restore.sh";
    UpdaterRig rig(data, 0, script);
    if (!made || !rig.open()) {
      report(false, "trial overrun: rig or script did not open");
      return;
    }
    SentFrame f{};
    (void)rig.fromGround(frame::kDestController, 1, {command::kGetStatus});
    (void)rig.tick(f);
    rig.answer(f.sequence, answer::kDone, 0);
    rig.health(report::kModeTrial);
    rig.advance(config::kTrialMs + config::kTrialMarginMs + 1);
    (void)rig.tick(f);
    rig.advance(config::kBeginTimeoutMs + 1);   // the test image of this trial times out and frees data link A
    (void)rig.tick(f);
    uint64_t t0 = clocks::nowMs();
    bool ok = rig.fromGround(frame::kDestController, 2, {command::kGetStatus});
    SentFrame g{};
    ok = ok && rig.tick(g) && isCommand(g, {command::kGetStatus});
    uint64_t took = clocks::nowMs() - t0;
    report(ok && took < 800 && !exists(dir + "/finished"),
      "trial overrun: while the script runs a ground command is passed on to the controller (" + std::to_string(took) + " ms, script still running)");
    rig.answer(g.sequence, answer::kDone, 0);
    std::vector<std::vector<uint8_t>> reports;
    serveUntilScriptEnds(rig, dir + "/finished", reports);
    report(countEvents(reports, 0x0106) == 1, "trial overrun: and the event comes when the script has ended");
  }

  // A second overrun while the script still runs does not start a second one.
  {
    std::string dir = base + "/twice";
    (void)::mkdir(dir.c_str(), 0755);
    removeTree(dir);
    (void)::mkdir(dir.c_str(), 0755);
    bool made = writeScript(dir + "/restore.sh", "echo x >> " + dir + "/calls\nsleep 1\necho 'RESTORE DONE: reset'\ntouch " + dir + "/finished\n", 0755);
    std::string data = dir + "/data";   // a named string: the store keeps the pointer
    std::string script = dir + "/restore.sh";
    UpdaterRig rig(data, 0, script);
    if (!made || !rig.open()) {
      report(false, "trial overrun twice: rig or script did not open");
      return;
    }
    SentFrame f{};
    (void)rig.fromGround(frame::kDestController, 1, {command::kGetStatus});
    (void)rig.tick(f);
    rig.answer(f.sequence, answer::kDone, 0);
    rig.health(report::kModeTrial);
    rig.advance(config::kTrialMs + config::kTrialMarginMs + 1);
    (void)rig.tick(f);
    rig.health(report::kModeOperate);
    rig.health(report::kModeTrial);   // another trial begins at once
    rig.advance(config::kTrialMs + config::kTrialMarginMs + 1);
    (void)rig.tick(f);
    std::vector<std::vector<uint8_t>> reports;
    serveUntilScriptEnds(rig, dir + "/finished", reports);
    std::vector<uint8_t> calls = readFile(dir + "/calls");
    report(calls.size() == 2 && countEvents(reports, 0x0106) == 1,
      "trial overrun: a second overrun while the script runs does not start it again; one event");
  }

  // The deadline stops a script that hangs; nothing is sent.
  {
    std::string dir = base + "/hang";
    (void)::mkdir(dir.c_str(), 0755);
    removeTree(dir);
    (void)::mkdir(dir.c_str(), 0755);
    bool made = writeScript(dir + "/restore.sh", "echo $$ > " + dir + "/pid\nexec sleep 100\n", 0755);
    std::string data = dir + "/data";   // a named string: the store keeps the pointer
    std::string script = dir + "/restore.sh";
    UpdaterRig rig(data, 0, script);
    if (!made || !rig.open()) {
      report(false, "trial overrun hang: rig or script did not open");
      return;
    }
    SentFrame f{};
    (void)rig.fromGround(frame::kDestController, 1, {command::kGetStatus});
    (void)rig.tick(f);
    rig.answer(f.sequence, answer::kDone, 0);
    rig.health(report::kModeTrial);
    rig.advance(config::kTrialMs + config::kTrialMarginMs + 1);
    (void)rig.tick(f);
    std::vector<std::vector<uint8_t>> reports;
    serveUntilScriptEnds(rig, "", reports);
    long pid = readPid(dir + "/pid");
    bool alive = pid > 0 && ::kill(static_cast<pid_t>(pid), 0) == 0;   // still sleeping before the deadline
    rig.advance(config::kRestartScriptMs + 1);   // past the updater's deadline: TERM
    serveUntilScriptEnds(rig, "", reports);
    report(alive && processGone(readPid(dir + "/pid")) && noRestartEvent(reports),
      "trial overrun: a script that hangs is stopped at the updater's deadline; nothing is sent");
  }
}

/**
 * @brief Decision S: the controller does not answer an ARM; the ground's dangerous command is refused 0xFF.
 *
 * ARM held, dangerous command sent, the ARM goes to the controller, no answer within the answer
 * time: the ground gets REFUSED 0xFF "controller did not answer" for its command, the command
 * itself is never sent, and a later answer to the ARM changes nothing. Before the decision the
 * ground got no answer.
 *
 * @param[in]     scratch  scratch directory
 * @return nothing
 */
void testArmNoAnswer(const std::string & scratch)
{
  std::string dir = scratch + "/arm-no-answer";
  removeTree(dir);
  UpdaterRig rig(dir, 0);
  if (!rig.open()) { report(false, "arm no answer: rig did not open"); return; }
  const uint32_t kDest = frame::kDestController;
  SentFrame arm{};
  bool ok = rig.fromGround(kDest, 1, armOf(command::kSwitch, 2));
  (void)rig.groundAnswers();
  ok = ok && rig.fromGround(kDest, 2, {command::kSwitch}) && rig.tick(arm) &&
       arm.payload.size() == 6 && arm.payload[0] == command::kArm;
  report(ok && hasAnswer(rig.groundAnswers(), 2, answer::kRefused, 0xFF) == false,
    "arm no answer: before the answer time passes the ground has no answer for the SWITCH");
  rig.advance(config::kAnswerTimeoutMs - 1);
  SentFrame f{};
  bool early = rig.tick(f);
  report(!early && rig.groundAnswers().empty(), "arm no answer: 1 ms before the answer time (2 s): still waiting");
  rig.advance(1);
  SentFrame after{};
  bool sent = rig.tick(after);
  std::vector<GroundAnswer> g = rig.groundAnswers();
  report(hasAnswer(g, 2, answer::kRefused, answer::kNoAnswerBench) && g.size() == 1,
    "arm no answer: past the answer time the ground's SWITCH is answered REFUSED 0xFF, once");
  report(!sent, "arm no answer: and the SWITCH itself is not sent");
  rig.answer(arm.sequence, answer::kDone, 0);   // a late ARM answer
  report(!rig.tick(f) && rig.groundAnswers().empty(), "arm no answer: a late answer to the ARM sends nothing more");
}

/**
 * @brief Decision S names and the decoded detail: refusals 0x10 and 0x11, event 0x0106, check results, events 0x0002 and 0x0003.
 *
 * Passes when each name is the Interfaces Sec. 4.2 / Sec. 5.4 wording, unknown values are
 * "unknown", the constants have the table's values, and describeEvent writes the refused copy and
 * the check result of events 0x0002 and 0x0003 (and says so when the detail is short or absent).
 *
 * @return nothing
 */
void testNamesAndFallbackDetail()
{
  report(answer::kWrongKind == 0x10 && answer::kBootStateNotStored == 0x11 &&
         report::kEventControllerRestarted == 0x0106 && report::kRestartReasonTrialPastTime == 0x01,
    "names: refusals 0x10 and 0x11, event 0x0106 and reason 0x01 have the table's values");
  report(std::strcmp(refusalName(0x10), "image of the wrong kind for its place") == 0 &&
         std::strcmp(refusalName(0x11), "boot state could not be stored") == 0 &&
         std::strcmp(refusalName(0x0F), "trial tests not passed") == 0 &&
         std::strcmp(refusalName(0x05), "not allowed in the current mode") == 0 &&
         std::strcmp(refusalName(0xFF), "controller did not answer (bench code)") == 0 &&
         std::strcmp(refusalName(0x12), "unknown") == 0 && std::strcmp(refusalName(0x00), "unknown") == 0,
    "names: refusal 0x10 'image of the wrong kind for its place', 0x11 'boot state could not be stored', 0xFF, unknown");
  bool all_named = true;
  for (unsigned code = 0x01; code <= 0x11; code++) {
    all_named = all_named && std::strcmp(refusalName(static_cast<uint8_t>(code)), "unknown") != 0;
  }
  report(all_named, "names: every refusal 0x01 to 0x11 of the Sec. 4.2 table has a name");
  report(std::strcmp(eventName(0x0106), "controller restarted by the onboard computer") == 0 &&
         std::strcmp(eventName(0x0009), "restart") == 0 && std::strcmp(eventName(0x0107), "unknown") == 0,
    "names: event 0x0106 'controller restarted by the onboard computer'; 0x0009 stays the controller's 'restart'");
  const char * expected[] = {"none: the copy passed", "the copy is empty", "proprietary string", "format", "kind",
    "target", "length", "start address outside the region", "body check value", "signature A", "signature B"};
  bool results = true;
  for (unsigned i = 0; i <= 0x0A; i++) {
    results = results && std::strcmp(checkResultName(static_cast<uint8_t>(i)), expected[i]) == 0;
  }
  report(results && std::strcmp(checkResultName(0x0B), "unknown") == 0,
    "names: check results 0x00 to 0x0A as in the Sec. 5.4 table; 0x0B unknown");

  std::array<char, 192> text{};
  const uint8_t previous[] = {0x01, 0x09};
  describeEvent(0x0002, previous, 2, text.data(), text.size());
  report(std::strcmp(text.data(), "EVENT 0x0002 (fall-back to the previous copy), refused copy 0x01 (A), check result 0x09 (signature A)") == 0,
    "event detail: 0x0002 {0x01, 0x09}: refused copy A, check result signature A");
  const uint8_t last_resort[] = {0x02, 0x0A};
  describeEvent(0x0003, last_resort, 2, text.data(), text.size());
  report(std::strcmp(text.data(), "EVENT 0x0003 (fall-back to the last-resort firmware), refused copy 0x02 (B), check result 0x0a (signature B)") == 0,
    "event detail: 0x0003 {0x02, 0x0A}: refused copy B, check result signature B");
  const uint8_t odd[] = {0x03, 0x7F, 0x55};
  describeEvent(0x0002, odd, 3, text.data(), text.size());
  report(std::strstr(text.data(), "refused copy 0x03 (last-resort), check result 0x7f (unknown)") != nullptr,
    "event detail: copy 0x03 is last-resort; a check result outside the table is 'unknown'; a third byte is ignored");
  describeEvent(0x0002, previous, 1, text.data(), text.size());
  report(std::strstr(text.data(), "detail of 1 byte(s) instead of the 2 of Sec. 5.4: refused copy only, no check result") != nullptr,
    "event detail: 0x0002 with a 1-byte detail says so");
  describeEvent(0x0003, nullptr, 0, text.data(), text.size());
  report(std::strstr(text.data(), "detail of 0 byte(s) instead of the 2 of Sec. 5.4") != nullptr,
    "event detail: 0x0003 with no detail says so");
  const uint8_t reason[] = {0x01};
  describeEvent(0x0106, reason, 1, text.data(), text.size());
  report(std::strcmp(text.data(), "EVENT 0x0106 (controller restarted by the onboard computer), detail 0x01") == 0,
    "event detail: 0x0106 shows its reason byte");
  describeEvent(0x000B, reason, 1, text.data(), text.size());
  report(std::strcmp(text.data(), "EVENT 0x000b (trial tests failed), detail 0x01") == 0,
    "event detail: 0x000B shows which test (unchanged)");
  describeEvent(0x0004, previous, 2, text.data(), text.size());
  report(std::strcmp(text.data(), "EVENT 0x0004 (self-test failed)") == 0, "event detail: other events show no detail");
  std::array<char, 16> small{};
  describeEvent(0x0002, previous, 2, small.data(), small.size());
  report(small[small.size() - 1] == '\0' && std::strlen(small.data()) == small.size() - 1,
    "event detail: a too-small buffer gets cut text, terminated");

  // Through the report store: a 17-byte event 0x0002 is accepted and numbered, as before.
  ReportStore store;
  std::vector<uint8_t> event(report::kEventMinBytes + 2, 0);
  event[report::kOffsetKind] = report::kEvent;
  bytes::writeU16(&event[report::kOffsetEventCode], 0x0002);
  event[report::kEventDetailOffset] = 0x01;
  event[report::kEventDetailOffset + 1] = 0x09;
  std::array<uint8_t, report::kEventMaxBytes> out{};
  report(store.accept(event.data(), event.size(), 7, 100, out.data(), out.size()) &&
         bytes::readU32(&out[report::kOffsetNumber]) == 1 && out[report::kEventDetailOffset + 1] == 0x09,
    "event detail: the report store passes the 2-byte detail of event 0x0002 on unchanged");
}

/**
 * @brief Decision H options: --restore-script, and the default path next to the program.
 *
 * @return nothing
 */
void testRestoreScriptOption()
{
  Options o{};
  const char * none[] = {"fsir-updater"};
  report(parseOptions(1, none, o) && o.restore_script == nullptr, "options: --restore-script not given: unset (default next to the program)");
  const char * given[] = {"x", "--restore-script", "/opt/fsir/restore.sh", "--beat-s", "2"};
  report(parseOptions(5, given, o) && std::strcmp(o.restore_script, "/opt/fsir/restore.sh") == 0 && o.beat_s == 2,
    "options: --restore-script PATH read");
  const char * missing[] = {"x", "--restore-script"};
  report(!parseOptions(2, missing, o), "options: --restore-script without a value refused");

  std::array<char, 128> path{};
  bool made = defaultRestoreScript("/opt/fsir/fsir-updater", path.data(), path.size());
  std::string text(path.data());
#ifdef __linux__
  // On Linux the program file is the test program, from /proc/self/exe, not argv[0].
  report(made && text.size() > 11 && text.substr(text.size() - 11) == "/restore.sh",
    "options: default restart script is restore.sh in the directory of the program file");
#else
  report(made && text == "/opt/fsir/restore.sh", "options: default restart script is restore.sh in the directory of the program");
#endif
  std::array<char, 8> tiny{};
  report(!defaultRestoreScript("/opt/fsir/fsir-updater", tiny.data(), tiny.size()),
    "options: a path that does not fit gives false");
}

/**
 * @brief Decision 3b and the checks of the check-only mode (image_file_check.hpp).
 *
 * @param[in]     test_images  directory with the signed test images
 * @param[in]     flipped      path of a damaged image
 * @param[in]     scratch      scratch directory
 * @return nothing
 */
void testCheckImageFile(const std::string & test_images, const std::string & flipped,
  const std::string & scratch)
{
  ImageChecker checker(PUBLIC_KEY_A, PUBLIC_KEY_B);
  std::array<char, 384> line{};
  uint8_t kind = 0;
  report(parseKindName("bootloader", kind) && kind == 0x01 && parseKindName("main", kind) &&
         kind == 0x02 && parseKindName("last-resort", kind) && kind == 0x03 &&
         !parseKindName("slot", kind) && !parseKindName(nullptr, kind),
    "check-image: the --kind words bootloader, main, last-resort; anything else not understood");
  bool ok = checkImageFile((test_images + "/slotA-v1.img").c_str(), 0x02, checker, line.data(), line.size());
  report(ok && std::strstr(line.data(), "passed") != nullptr, "check-image: a good main image passes");
  ok = checkImageFile((test_images + "/last-resort-v1.img").c_str(), 0x03, checker, line.data(), line.size());
  report(ok, "check-image: a good last-resort image passes as last-resort");
  ok = checkImageFile((test_images + "/last-resort-v1.img").c_str(), 0x02, checker, line.data(), line.size());
  report(!ok && std::strstr(line.data(), "kind is last-resort, expected main") != nullptr,
    "check-image: a last-resort image given as main: refused, line says the kinds");
  ok = checkImageFile((test_images + "/slotA-v1-wrong-key-a.img").c_str(), 0x02, checker, line.data(), line.size());
  report(!ok && std::strstr(line.data(), "signature A") != nullptr, "check-image: wrong key A: refused, 'signature A'");
  ok = checkImageFile((test_images + "/slotB-v2-wrong-key-b.img").c_str(), 0x02, checker, line.data(), line.size());
  report(!ok && std::strstr(line.data(), "signature B") != nullptr, "check-image: wrong key B: refused, 'signature B'");
  ok = checkImageFile(flipped.c_str(), 0x02, checker, line.data(), line.size());
  report(!ok && std::strstr(line.data(), "refusal 0x") != nullptr, "check-image: a damaged image: refused with its code");
  ok = checkImageFile((scratch + "/no-such-file").c_str(), 0x02, checker, line.data(), line.size());
  report(!ok && std::strstr(line.data(), "cannot open") != nullptr, "check-image: a missing file: refused");
  // Exactly the overhead (192 bytes): an image with an empty body must not pass.
  std::string tiny = scratch + "/tiny.img";
  std::vector<uint8_t> image = readFile(test_images + "/slotA-v1.img");
  image.resize(config::kImageOverheadBytes);
  std::FILE * out = std::fopen(tiny.c_str(), "wb");
  if (out != nullptr) {
    (void)std::fwrite(image.data(), 1, image.size(), out);
    (void)std::fclose(out);
  }
  ok = checkImageFile(tiny.c_str(), 0x02, checker, line.data(), line.size());
  report(!ok, "check-image: an image of 192 bytes (no body) is refused");
  Options o{};
  const char * a1[] = {"x", "--check-image", "f"};
  const char * a2[] = {"x", "--check-image", "f", "--kind", "main"};
  report(!parseOptions(3, a1, o) && parseOptions(5, a2, o) && o.check_image != nullptr &&
         std::strcmp(o.check_kind, "main") == 0,
    "check-image: options: --check-image needs --kind");
}

/**
 * @brief Severity-3 fixes: image 0xFFFF, a missing device, the bench codes, the socket flags.
 *
 * @param[in]     controller_images  directory with firmware-B.img
 * @param[in]     scratch            scratch directory
 * @return nothing
 */
void testSeverity3(const std::string & controller_images, const std::string & scratch)
{
  {
    std::string dir = scratch + "/audit-s3a";
    removeTree(dir);
    UpdaterRig rig(dir, 0);
    if (!rig.open()) { report(false, "image 0xFFFF: rig did not open"); return; }
    const std::vector<std::vector<uint8_t>> cmds = {
      {command::kUploadBegin, 0xFF, 0xFF, 0, 4, 0, 0}, {command::kUploadChunk, 0xFF, 0xFF, 0, 0, 0, 0, 1},
      {command::kUploadEnd, 0xFF, 0xFF}, {command::kUploadCancel, 0xFF, 0xFF}, {command::kDeliver, 0xFF, 0xFF}};
    bool ok = true;
    uint32_t seq = 1;
    for (const std::vector<uint8_t> & c : cmds) {
      ok = rig.fromGround(frame::kDestOnboard, seq, c) && ok;
      std::vector<GroundAnswer> g = rig.groundAnswers();
      ok = ok && g.size() == 1 && hasAnswer(g, seq, answer::kRefused, 0x06);
      seq++;
    }
    report(ok, "image 0xFFFF: every command naming it is REFUSED 0x06 only (no RECEIVED)");
  }
  {
    std::string dir = scratch + "/audit-s3b";
    removeTree(dir);
    UpdaterRig rig(dir, 0);
    if (!rig.open()) { report(false, "cannot send: rig did not open"); return; }
    rig.failSending(true);
    bool ok = rig.fromGround(frame::kDestController, 1, {command::kGetStatus});
    report(ok && hasAnswer(rig.groundAnswers(), 1, answer::kRefused, answer::kCannotSendBench),
      "cannot send: a ground command with data link A's device missing: REFUSED 0xFD (not silence)");
    ok = rig.fromGround(frame::kDestController, 2, armOf(command::kSwitch, 3));
    (void)rig.groundAnswers();
    ok = ok && rig.fromGround(frame::kDestController, 3, {command::kSwitch});
    report(ok && hasAnswer(rig.groundAnswers(), 3, answer::kRefused, answer::kCannotSendBench),
      "cannot send: an ARM/command pair with the device missing: REFUSED 0xFD");
    std::vector<uint8_t> image = readFile(controller_images + "/firmware-B.img");
    ok = storeThrough(rig, 3, image);
    rig.health(report::kModeOperate);
    ok = ok && rig.fromGround(frame::kDestOnboard, 4, {command::kDeliver, 3, 0});
    report(ok && hasAnswer(rig.groundAnswers(), 4, answer::kRefused, answer::kCannotSendBench),
      "cannot send: a delivery with the device missing: REFUSED 0xFD, not the 'no answer' code 0xFF");
  }
  {
    std::string dir = scratch + "/audit-s3c";
    removeTree(dir);
    UploadStore store(dir.c_str());
    bool ready = store.ready();
    removeTree(dir);   // the storage goes away under the store
    uint32_t resume = 0;
    report(ready && store.begin(1, 1000, resume) == answer::kStorageBench &&
           answer::kStorageBench != answer::kNoAnswerBench && answer::kCannotSendBench != answer::kNoAnswerBench,
      "bench codes: a storage failure is 0xFE, apart from 'no answer' 0xFF and 'cannot send' 0xFD");
  }
  {
    CommandGate gate(47934);
    bool open = gate.open();
    int flags = open ? ::fcntl(gate.fd(), F_GETFL) : 0;
    int fdflags = open ? ::fcntl(gate.fd(), F_GETFD) : 0;
    report(open && (flags & O_NONBLOCK) != 0 && (fdflags & FD_CLOEXEC) != 0,
      "socket: the ground socket is non-blocking and close-on-exec");
  }
  {
    // Every descriptor the store opens is close-on-exec.
    std::string dir = scratch + "/audit-s3d";
    removeTree(dir);
    UploadStore store(dir.c_str());
    bool ok = store.ready();
    std::vector<int> before;
    for (int fd = 3; fd < 256; fd++) {
      if (::fcntl(fd, F_GETFD) >= 0) { before.push_back(fd); }
    }
    uint32_t resume = 0;
    ok = ok && store.begin(1, 1000, resume) == 0;
    bool all = true;
    int fresh = 0;
    for (int fd = 3; fd < 256; fd++) {
      int fl = ::fcntl(fd, F_GETFD);
      if (fl >= 0 && std::find(before.begin(), before.end(), fd) == before.end()) {
        fresh++;
        all = all && (fl & FD_CLOEXEC) != 0;
      }
    }
    report(ok && fresh >= 1 && all, "O_CLOEXEC: the store's open .part file is close-on-exec");
    int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    bool pty = master >= 0 && ::grantpt(master) == 0 && ::unlockpt(master) == 0;
    const char * slave = pty ? ::ptsname(master) : nullptr;
    if (slave != nullptr) {
      SerialPort port(slave);
      bool opened = port.open();
      int fl = opened ? ::fcntl(port.pollFd(), F_GETFD) : 0;
      report(opened && (fl & FD_CLOEXEC) != 0, "O_CLOEXEC: the serial device is close-on-exec");
    } else {
      report(false, "O_CLOEXEC: could not make a pseudo-terminal");
    }
    if (master >= 0) { (void)::close(master); }
  }
}

/**
 * @brief Decision 8: the dropped-datagram log line is rate limited.
 *
 * @return nothing
 */
void testDropLogLimit()
{
  CommandGate gate(47935);
  int client = ::socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(47935);
  to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bool ok = gate.open() && client >= 0;
  const std::array<uint8_t, 8> junk = {1, 2, 3, 4, 5, 6, 7, 8};
  for (int i = 0; ok && i < 200; i++) {
    ok = ::sendto(client, junk.data(), junk.size(), 0, reinterpret_cast<const sockaddr *>(&to),
           sizeof to) == static_cast<ssize_t>(junk.size());
  }
  frame::Frame f{};
  pollfd p{};
  p.fd = gate.fd();
  p.events = POLLIN;
  while (ok && ::poll(&p, 1, 50) == 1) {
    (void)gate.receive(f);
  }
  report(ok && gate.droppedCount() == 200 && gate.droppedLogged() >= 1 && gate.droppedLogged() <= 3,
    "drop log: 200 bad datagrams in a burst: all counted, at most 3 log lines");
  if (client >= 0) { (void)::close(client); }
}

/**
 * @brief Audit findings 15: the header hash in the .part file.
 *
 * @param[in]     scratch  scratch directory
 * @return nothing
 */
void testStoreHardening(const std::string & scratch)
{
  std::string dir = scratch + "/audit-hash";
  removeTree(dir);
  const size_t kLength = 1000;
  std::vector<uint8_t> a(kLength);
  std::vector<uint8_t> b(kLength);
  for (size_t i = 0; i < kLength; i++) {
    a[i] = static_cast<uint8_t>(i * 3);
    b[i] = static_cast<uint8_t>(i * 5 + 1);
  }
  {
    UploadStore store(dir.c_str());
    uint32_t resume = 9;
    bool ok = store.ready() && store.begin(5, kLength, resume) == 0 &&
              sendChunks(store, 5, a, 0, 512);
    report(ok && store.stored() == 512, "header hash: 512 bytes of image A stored");
  }
  {
    UploadStore store(dir.c_str());
    uint32_t resume = 0;
    bool ok = store.begin(5, kLength, resume) == 0 && resume == 512;
    bool written = true;
    // The same image A starts again from offset 0: a resend, nothing dropped.
    ok = ok && store.chunk(5, 0, a.data(), 256, written) == 0 && !written && store.stored() == 512;
    report(ok, "header hash: the same image's first chunk again: a resend, the 512 stored bytes kept");
    // A different image B with the same number and length sends its first chunk.
    ok = store.chunk(5, 0, b.data(), 256, written) == 0 && written && store.stored() == 256;
    report(ok, "header hash: another image, same number and length, first chunk: stored bytes dropped, B starts afresh");
    ok = sendChunks(store, 5, b, 256, kLength) && store.stored() == kLength;
    report(ok, "header hash: and B then goes on to its full length");
  }
  {
    // A .part whose bytes no longer match its hash is not resumed.
    UploadStore store(dir.c_str());
    uint32_t resume = 0;
    (void)store.begin(6, kLength, resume);
    (void)sendChunks(store, 6, a, 0, 512);
    std::string part = dir + "/image-00006.part";
    int fd = ::open(part.c_str(), O_RDWR | O_CLOEXEC);
    const uint8_t flip = 0xEE;
    bool ok = fd >= 0 && ::pwrite(fd, &flip, 1, 40 + 10) == 1;
    if (fd >= 0) { (void)::close(fd); }
    UploadStore again(dir.c_str());
    ok = ok && again.begin(6, kLength, resume) == 0 && resume == 0;
    report(ok, "header hash: a .part whose first 64 bytes changed on disk is not resumed");
  }
  {
    // A .part of the older form (no tag) is not resumed either.
    std::string part = dir + "/image-00007.part";
    std::FILE * out = std::fopen(part.c_str(), "wb");
    std::array<uint8_t, 4 + 300> old{};
    bytes::writeU32(old.data(), static_cast<uint32_t>(kLength));
    if (out != nullptr) {
      (void)std::fwrite(old.data(), 1, old.size(), out);
      (void)std::fclose(out);
    }
    UploadStore store(dir.c_str());
    uint32_t resume = 5;
    report(out != nullptr && store.begin(7, kLength, resume) == 0 && resume == 0,
      "header hash: a .part of the earlier form (4-byte head) is not resumed");
  }
}

// The flush hook of testFlushOrder: notes whether the .part file still existed at the second
// call (the first flush after the rename), and can be made to fail.
std::string g_hook_part;
int g_hook_calls = 0;
int g_hook_part_existed_at_call = -1;
bool g_hook_fail_from = false;
int g_hook_fail_at = 0;

/**
 * @brief The test's replacement for the directory flush.
 *
 * Counts the calls, notes if the .part file exists at call number 2 (the flush that must come
 * before the .part file is deleted), and fails from call number g_hook_fail_at when
 * g_hook_fail_from is set.
 *
 * @param[in]     dir  the directory (unused)
 * @return true: pretend the flush worked; false: pretend it failed
 */
bool flushHook(const char * dir)
{
  (void)dir;
  g_hook_calls++;
  struct stat info{};
  bool exists = ::stat(g_hook_part.c_str(), &info) == 0;
  if (g_hook_calls == 1) {
    g_hook_part_existed_at_call = exists ? 1 : 0;
  }
  return !(g_hook_fail_from && g_hook_calls >= g_hook_fail_at);
}

/**
 * @brief Audit finding 16: the directory is flushed between the rename and the delete of the .part file; a failed flush keeps the .part file.
 *
 * @param[in]     controller_images  directory with firmware-B.img
 * @param[in]     scratch            scratch directory
 * @return nothing
 */
void testFlushOrder(const std::string & controller_images, const std::string & scratch)
{
  std::string dir = scratch + "/audit-flush";
  removeTree(dir);
  std::vector<uint8_t> image = readFile(controller_images + "/firmware-B.img");
  ImageChecker checker(PUBLIC_KEY_A, PUBLIC_KEY_B);
  UploadStore store(dir.c_str());
  bool ok = store.ready();
  store.setDirectorySync(flushHook);
  g_hook_part = dir + "/image-00003.part";
  g_hook_fail_from = false;
  uint32_t resume = 0;
  ImageVerdict verdict{};
  ok = ok && store.begin(3, static_cast<uint32_t>(image.size()), resume) == 0 &&
       sendChunks(store, 3, image, 0, image.size());
  g_hook_calls = 0;   // from here on: the flushes of UPLOAD END
  g_hook_part_existed_at_call = -1;
  ok = ok && store.end(3, checker, verdict) == 0;
  report(ok && g_hook_calls == 2 && g_hook_part_existed_at_call == 1,
    "flush order: UPLOAD END flushes the folder after the rename while the .part file still exists, then again after deleting it");

  std::string dir2 = scratch + "/audit-flush2";
  removeTree(dir2);
  UploadStore store2(dir2.c_str());
  ok = store2.ready();
  store2.setDirectorySync(flushHook);
  g_hook_part = dir2 + "/image-00003.part";
  g_hook_fail_from = false;
  ok = ok && store2.begin(3, static_cast<uint32_t>(image.size()), resume) == 0 &&
       sendChunks(store2, 3, image, 0, image.size());
  g_hook_calls = 0;
  g_hook_fail_from = true;
  g_hook_fail_at = 1;
  uint8_t code = store2.end(3, checker, verdict);
  g_hook_fail_from = false;
  struct stat info{};
  report(ok && code == answer::kStorageBench && ::stat(g_hook_part.c_str(), &info) == 0,
    "flush order: a failed flush after the rename: UPLOAD END fails 0xFE and the .part file is kept");
  report(syncDirectory(scratch.c_str()) && !syncDirectory((scratch + "/no-such-dir").c_str()),
    "flush: syncDirectory is true for a folder and false for one that cannot be opened");
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
  testArmAge(argv[1], argv[4]);
  testControllerAllowedList(argv[4]);
  testAlreadyDone(argv[1], argv[4]);
  testUsedSequences(argv[4]);
  testResendAfterLostAnswer(argv[4]);
  testTestImageOnEvent(argv[4]);
  testTrialWatch();
  testRestartRunner(argv[4]);
  testTrialOverrunRestart(argv[4]);
  testArmNoAnswer(argv[4]);
  testNamesAndFallbackDetail();
  testRestoreScriptOption();
  testCheckImageFile(argv[2], argv[3], argv[4]);
  testSeverity3(argv[1], argv[4]);
  testDropLogLimit();
  testStoreHardening(argv[4]);
  testFlushOrder(argv[1], argv[4]);
  std::printf("unit tests: %d of %d passed\n", g_tests - g_failures, g_tests);
  return g_failures == 0 ? 0 : 1;
}
