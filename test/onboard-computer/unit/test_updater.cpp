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
 *   reports       report number and onboard time filled in; lengths and kinds checked
 *   data link     stop-and-wait, sequence numbers, timeout, the 10 ms gap
 *   options       the command line
 *
 * Usage: test_updater CONTROLLER_IMAGES TEST_IMAGES FLIPPED_IMAGE SCRATCH_DIR
 * Prints PASS or FAIL per test; exit status 1 on a failure.
 */
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "bytes.hpp"
#include "command_gate.hpp"
#include "config.hpp"
#include "crc32.hpp"
#include "data_link.hpp"
#include "frame.hpp"
#include "image_checker.hpp"
#include "keys.h"
#include "log.hpp"
#include "options.hpp"
#include "protocol.hpp"
#include "report_store.hpp"
#include "update_manager.hpp"

// The controller's frame layer and CRC-32, in C, as reference.
extern "C" {
#include "crc32.h"
#include "frame.h"
}

namespace
{

int g_tests = 0;
int g_failures = 0;

void report(bool ok, const std::string & name)
{
  g_tests++;
  if (!ok) {
    g_failures++;
  }
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name.c_str());
}

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
uint32_t nextRandom()
{
  g_seed = g_seed * 1103515245U + 12345U;
  return g_seed >> 8;
}

// ---- CRC-32 and frames ---------------------------------------------------------------------

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

void flip(std::vector<uint8_t> & image, size_t offset)
{
  image[offset] = static_cast<uint8_t>(image[offset] ^ 0xFFU);   // as tools/flip_byte.py
}

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

bool exists(const std::string & path)
{
  struct stat info{};
  return stat(path.c_str(), &info) == 0;
}

// Uploads image[from, to) in 256-byte chunks; false at the first refusal.
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

  bool sendBytes(const uint8_t * data, size_t size) override
  {
    sent.insert(sent.end(), data, data + size);
    return true;
  }

  long receiveBytes(uint8_t * buffer, size_t capacity) override
  {
    size_t n = std::min(capacity, incoming.size());
    std::memcpy(buffer, incoming.data(), n);
    incoming.erase(incoming.begin(), incoming.begin() + static_cast<long>(n));
    return static_cast<long>(n);
  }

  int pollFd() const override
  {
    return -1;
  }
};

class Collector final : public ControllerFrameHandler
{
public:
  std::vector<frame::Answer> answers;
  void onControllerFrame(const frame::Frame & f) override
  {
    frame::Answer a{};
    if (frame::decodeAnswer(f, a)) {
      answers.push_back(a);
    }
  }
};

std::vector<uint8_t> answerBytes(uint32_t sequence, uint32_t answered, uint8_t kind)
{
  std::array<uint8_t, 6> payload{};
  frame::encodeAnswer(frame::Answer{answered, kind, 0}, payload.data());
  frame::Frame f{frame::kTypeAnswer, frame::kDestOnboard, 0, 6, sequence, 0, payload.data()};
  std::array<uint8_t, 64> out{};
  size_t n = frame::encode(f, out.data(), out.size());
  return std::vector<uint8_t>(out.begin(), out.begin() + static_cast<long>(n));
}

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
    pushed = pushed && queue.push(c);
  }
  report(pushed && queue.full() && !queue.push(c), "forward queue: holds 8, refuses a ninth");
  report(queue.front().ground_sequence == 77 && queue.front().payload[0] == 0x20 &&
         queue.front().expiry == 5, "forward queue: oldest first, fields kept");
  queue.pop();
  report(queue.front().ground_sequence == 78, "forward queue: pop gives the next");
}

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

}  // namespace

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
  testReports();
  testDataLink();
  testTables();
  testOptions();
  testKeyFile(argv[4]);
  std::printf("unit tests: %d of %d passed\n", g_tests - g_failures, g_tests);
  return g_failures == 0 ? 0 : 1;
}
