/*
 * fuzz_updater.cpp — fuzz target 3 (fuzz_updater.hpp).
 *
 * A session is a fresh Updater in a fresh data directory, driven by about kOpsPerSession injected
 * frames (ground datagrams and controller frames), all derived from (seed, session). The
 * controller is played: it answers the updater's commands RECEIVED then DONE most of the time,
 * REFUSED or not at all otherwise, so that uploads, deliveries, ARM pairs and the trial path are
 * reached.
 *
 * Oracles (task brief; sources in each check):
 *   O1 a dangerous command goes to the controller only directly after its ARM frame naming it
 *      (Interfaces Sec. 5.2, REQ-029, REQ-112), and never more often than ARMs were answered DONE;
 *   O2 a dangerous onboard command (SET LIMITS, SET ONBOARD TIME) ends DONE only after an ARM
 *      naming it and its ground sequence number was answered DONE;
 *   O3 a command that must be refused (unknown identifier, wrong length, Expiry 0 or in the past
 *      with the clock unchanged) is never answered RECEIVED, DONE or ALREADY DONE (Sec. 4.2);
 *   O4 only the controller's own commands (0x20 to 0x26, the ARM) leave on data link A on a ground
 *      command; the UPLOAD commands 0x01 to 0x04 only as part of a delivery or the test image;
 *   O5 an answer goes to the ground only for a sequence number the ground sent, with a kind of
 *      Sec. 4.2; every frame to the ground decodes;
 *   plus crash, UBSan, hang (the watchdog) and memory growth (resident size per session).
 */
#include "fuzz_updater.hpp"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <memory>
#include <mach/mach.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "command_gate.hpp"
#include "config.hpp"
#include "controller_watch.hpp"
#include "data_link.hpp"
#include "frame.hpp"
#include "image_checker.hpp"
#include "keys.h"
#include "protocol.hpp"
#include "report_store.hpp"
#include "transport.hpp"
#include "update_manager.hpp"
#include "updater.hpp"

namespace fuzz
{

namespace
{

// Injected frames per session. Bench value: 500 sessions of 2000 give 10^6 inputs, and a session
// is long enough to upload, deliver, arm and trial.
constexpr uint64_t kOpsPerSession = 2000;
// First UDP port tried; the next free one is taken. Bench choice, away from 47001 (bench test),
// 47931 to 47935 (unit tests).
constexpr uint16_t kPortFirst = 47990;
constexpr int kPortTries = 50;
// Longest wait for a loopback datagram to be readable, ms. Bench value: far above the normal
// microseconds, for a Mac under load.
constexpr int kDeliverWaitMs = 2000;
// Receive buffer of the fake ground, so a SEND REPORTS burst is not lost (as in the unit tests).
constexpr int kClientBuffer = 8 * 1024 * 1024;
// The largest data directory a session may reach before it is called unbounded growth, bytes:
// the report store 1 MB (config::kReportStoreBytes) twice (rewrite), uploads of 4 images of 40 KB,
// the used-sequence files; bench value.
constexpr uint64_t kDirMaxBytes = 8ULL * 1024ULL * 1024ULL;
// Resident-size growth between the first and the last session that is called a leak, bytes.
// Bench value: the allocator's own settling is a few MB; 64 MB is far above it.
constexpr uint64_t kRssGrowthMax = 64ULL * 1024ULL * 1024ULL;
// A system time that is certainly in the past of now (Sep 2001) and below any real clock: a
// command with an Expiry under it must be refused 0x03 while the clock offset is unchanged.
constexpr uint32_t kCertainlyPast = 1000000000U;
// Controller commands the updater may put on data link A for a ground command (Sec. 5.1 rows 0x20
// to 0x26) and the ARM.
constexpr uint8_t kCtrlIdFirst = 0x20;
constexpr uint8_t kCtrlIdLast = 0x26;
// The simulated time steps of a tick op, ms: around the timers (answer 2 s, 3 s, ARM 30 s,
// silence 5 s, trial 120 s + 30 s, reset gap 60 s).
constexpr uint64_t kTickSteps[12] = {0, 1, 10, 100, 1000, 2001, 3001, 5001, 29999, 30001, 60001,
                                     150001};

// The fake data link A, as in test_updater.cpp.
class FakeTransport final : public Transport
{
public:
  std::vector<uint8_t> sent;
  std::vector<uint8_t> incoming;

  /**
   * @brief Keeps the bytes the updater sends.
   *
   * @param[in]     data  the bytes
   * @param[in]     size  number of bytes
   * @return true, always
   */
  bool sendBytes(const uint8_t * data, size_t size) override
  {
    sent.insert(sent.end(), data, data + size);
    return true;
  }

  /**
   * @brief Hands the updater the bytes the fuzzer put in incoming.
   *
   * @param[out]    buffer    where the bytes go
   * @param[in]     capacity  size of buffer, bytes
   * @return number of bytes written to buffer
   */
  long receiveBytes(uint8_t * buffer, size_t capacity) override
  {
    const size_t n = std::min(capacity, incoming.size());
    std::memcpy(buffer, incoming.data(), n);
    incoming.erase(incoming.begin(), incoming.begin() + static_cast<long>(n));
    return static_cast<long>(n);
  }

  /**
   * @brief The fake has no descriptor to poll.
   *
   * @return -1
   */
  int pollFd() const override
  {
    return -1;
  }
};

// One frame the updater sent on data link A.
struct SentFrame
{
  uint8_t type;
  uint8_t destination;
  uint32_t sequence;
  std::vector<uint8_t> payload;
};

// What was sent to the ground under one sequence number.
struct GroundSent
{
  uint8_t dest;
  uint8_t id;
  uint16_t length;
  bool must_refuse;     // by the tables: the updater has no right to run it
  bool arm_valid;       // an ARM naming a dangerous command of its destination, 6 bytes
  bool onboard_danger;  // SET LIMITS or SET ONBOARD TIME with the right length, to the onboard
};

// Everything the oracles remember in a session.
struct Model
{
  std::map<uint32_t, std::vector<GroundSent>> by_seq;
  int tokens = 0;                 // ARMs answered DONE minus dangerous commands executed
  bool clock_pristine = true;     // no SET ONBOARD TIME answered DONE yet
  bool delivery_possible = false; // a DELIVER or a HEALTH in TRIAL was injected
  bool have_prev_frame = false;
  SentFrame prev_frame{};
  std::vector<SentFrame> unanswered;
  // ground cmds, raw, ctrl frames, answers, reports, dangerous sent, arms done, deliveries
  uint64_t counts[8] = {};
  std::map<uint32_t, uint64_t> answer_hist;   // kind << 8 | code
  std::map<uint32_t, uint64_t> done_by_id;
};

// The fuzzer's whole rig for one session.
struct Rig
{
  FakeTransport transport;
  DataLink link;
  CommandGate gate;
  UploadStore store;
  ImageChecker checker;
  ReportStore reports;
  ControllerWatch watch;
  std::string script;
  Updater updater;
  int client;
  uint64_t now_ms;
  uint64_t listen_ms;
  uint32_t ctrl_seq;
  uint32_t ground_seq;
  uint16_t port;
  frame::Parser parser;

  /**
   * @brief Builds the rig on a data directory and a UDP port.
   *
   * @param[in]     dir        data directory
   * @param[in]     udp_port   ground port
   */
  Rig(const std::string & dir, uint16_t udp_port)
  : transport(), link(transport), gate(udp_port), store(dir.c_str()),
    checker(PUBLIC_KEY_A, PUBLIC_KEY_B), reports(dir.c_str(), config::kReportStoreBytes),
    watch(5000, 0), script(), updater(gate, link, store, checker, reports, watch, 0U,
    script.c_str(), config::kRestartScriptMs), client(-1), now_ms(1000), listen_ms(0),
    ctrl_seq(1), ground_seq(1), port(udp_port), parser()
  {
  }

  /**
   * @brief Closes the fuzzer's own socket.
   */
  ~Rig()
  {
    if (client >= 0) {
      (void)::close(client);
    }
  }

  Rig(const Rig &) = delete;
  Rig & operator=(const Rig &) = delete;
};

// A seed image for uploads.
struct ImageSeed
{
  std::vector<uint8_t> bytes;
};

/**
 * @brief Tells whether a command to the onboard computer is known with this length (Sec. 5.1).
 *
 * Written from the Interfaces Sec. 5.1 table as summarised in protocol.hpp (sizes), not from
 * updater.cpp.
 *
 * @param[in]     id      identifier
 * @param[in]     length  payload length
 * @return true: a command the onboard computer runs
 */
bool validOnboard(uint8_t id, uint16_t length)
{
  switch (id) {
    case 0x01: return length == 7;
    case 0x02: return length >= 8 && length <= 263;
    case 0x03: case 0x04: case 0x05: return length == 3;
    case 0x06: return length == 7;
    case 0x07: return length == 1;
    case 0x08: return length == 4;
    case 0x09: return length == 5;
    case 0x0B: return length == 5;
    case 0x0C: return length == 34;
    case 0x25: return length == 1;
    case 0x0D: return length == 6;
    default: return false;   // input from the link: not a command of the onboard computer
  }
}

/**
 * @brief Tells whether a command to the controller is one the ground may send, with its length.
 *
 * @param[in]     id      identifier
 * @param[in]     length  payload length
 * @return true: a controller command (0x20 to 0x26) or the ARM with the right length
 */
bool validController(uint8_t id, uint16_t length)
{
  switch (id) {
    case 0x20: case 0x21: case 0x23: case 0x25: case 0x26: return length == 1;
    case 0x22: return length == 2;
    case 0x24: return length == 5;
    case 0x0D: return length == 6;
    default: return false;   // input from the link: not on the controller's list
  }
}

/**
 * @brief Tells whether an identifier is a dangerous command of the controller (Sec. 5.1).
 *
 * @param[in]     id  identifier
 * @return true: SWITCH, RETURN TO PREVIOUS, START FAILED, SET CONTROLLER TIME or CONFIRM
 */
bool dangerousController(uint8_t id)
{
  return id == 0x20 || id == 0x21 || id == 0x22 || id == 0x24 || id == 0x26;
}

/**
 * @brief Tells whether an identifier is a dangerous command of the onboard computer (Sec. 5.1).
 *
 * @param[in]     id  identifier
 * @return true: SET LIMITS, RESTORE, REPROGRAM, SET ONBOARD TIME or REPLACE KEY
 */
bool dangerousOnboard(uint8_t id)
{
  return id == 0x06 || id == 0x07 || id == 0x08 || id == 0x09 || id == 0x0C;
}

/**
 * @brief Reads the resident size of this process.
 *
 * @return bytes of resident memory, 0 if unknown
 */
uint64_t residentBytes()
{
  mach_task_basic_info info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
        &count) != KERN_SUCCESS) {
    return 0;
  }
  return info.resident_size;
}

/**
 * @brief Adds up the sizes of the files in a directory (not recursive; the store is flat).
 *
 * @param[in]     dir  directory
 * @return total bytes
 */
uint64_t dirBytes(const std::string & dir)
{
  uint64_t total = 0;
  DIR * d = ::opendir(dir.c_str());
  if (d == nullptr) {
    return 0;
  }
  for (size_t i = 0; i < 100000U; i++) {   // bounded
    struct dirent * e = ::readdir(d);
    if (e == nullptr) {
      break;
    }
    struct stat st{};
    if (::stat((dir + "/" + e->d_name).c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
      total += static_cast<uint64_t>(st.st_size);
    }
  }
  (void)::closedir(d);
  return total;
}

/**
 * @brief Reads one image file.
 *
 * @param[in]     path  file name
 * @param[out]    out   the bytes
 * @return true: read
 */
bool readAll(const std::string & path, std::vector<uint8_t> & out)
{
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  out.clear();
  std::array<uint8_t, 4096> buf{};
  for (size_t i = 0; i < 100U; i++) {   // bounded: 400 KB
    ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n <= 0) {
      break;
    }
    out.insert(out.end(), buf.begin(), buf.begin() + n);
  }
  (void)::close(fd);
  return true;
}

/**
 * @brief Loads the .img files of a directory as upload seeds.
 *
 * @param[in]     dir    directory
 * @param[in,out] seeds  seeds to add to
 * @return nothing
 */
void loadImages(const std::string & dir, std::vector<ImageSeed> & seeds)
{
  DIR * d = ::opendir(dir.c_str());
  if (d == nullptr) {
    return;
  }
  for (size_t i = 0; i < 1000U; i++) {   // bounded
    struct dirent * e = ::readdir(d);
    if (e == nullptr) {
      break;
    }
    const std::string n = e->d_name;
    ImageSeed s;
    if (n.size() > 4U && n.compare(n.size() - 4U, 4U, ".img") == 0 &&
        readAll(dir + "/" + n, s.bytes)) {
      seeds.push_back(s);
    }
  }
  (void)::closedir(d);
}

/**
 * @brief Records the command a datagram carries, if it is a valid command frame for the updater.
 *
 * Uses the fuzzer's own decoder (own CRC-32). A datagram that is no good command frame to
 * Destination 1 or 2 is dropped by the updater without an answer and is not recorded.
 *
 * @param[in,out] m   the model
 * @param[in]     d   the datagram
 * @return nothing
 */
void recordGround(Model & m, const std::vector<uint8_t> & d)
{
  if (d.size() < 20U || d[0] != 0xA5U || d[1] != 0x5AU || d[2] != 1U) {
    return;
  }
  const uint16_t len = readLe16(&d[6]);
  if (len > 1024U || 20U + static_cast<size_t>(len) != d.size() ||
      readLe32(&d[16U + len]) != refCrc32(d.data(), 16U + len)) {
    return;
  }
  const uint8_t type = d[3];
  const uint8_t dest = d[4];
  if (type != 1U || (dest != 1U && dest != 2U)) {
    return;
  }
  const uint32_t seq = readLe32(&d[8]);
  const uint32_t expiry = readLe32(&d[12]);
  GroundSent g{};
  g.dest = dest;
  g.id = (len > 0U) ? d[16] : 0U;
  g.length = len;
  const bool valid = (len > 0U) && (dest == 1U ? validOnboard(g.id, len)
                                    : validController(g.id, len));
  if (g.id == 0x0D && valid) {
    const uint8_t named = d[17];
    g.arm_valid = (dest == 1U) ? dangerousOnboard(named) : dangerousController(named);
  }
  const bool arm_ok = (g.id != 0x0D) || g.arm_valid;
  const bool exempt = dest == 1U && len > 0U &&
    (g.id == 0x09 || (g.id == 0x0D && len == 6U && d[17] == 0x09));
  const bool expired = m.clock_pristine && !exempt && (expiry == 0U || expiry < kCertainlyPast);
  g.must_refuse = !valid || !arm_ok || expired;
  if (dest == 1U && g.id == 0x05U) {
    m.delivery_possible = true;   // a DELIVER in a good command frame, however it was made
  }
  g.onboard_danger = dest == 1U && valid && (g.id == 0x06 || g.id == 0x09);
  m.by_seq[seq].push_back(g);
}

/**
 * @brief Builds a frame to the updater with the updater's own encoder.
 *
 * @param[in]     type     Type byte
 * @param[in]     dest     Destination byte
 * @param[in]     flags    Flags byte
 * @param[in]     seq      sequence number
 * @param[in]     expiry   expiry
 * @param[in]     payload  payload
 * @return the frame bytes
 */
std::vector<uint8_t> build(uint8_t type, uint8_t dest, uint8_t flags, uint32_t seq,
  uint32_t expiry, const std::vector<uint8_t> & payload)
{
  frame::Frame f{type, dest, flags, static_cast<uint16_t>(payload.size()), seq, expiry,
                 payload.data()};
  std::vector<uint8_t> out(frame::kMaxBytes);
  const size_t n = frame::encode(f, out.data(), out.size());
  out.resize(n);
  return out;
}

/**
 * @brief Moves the bytes the updater sent on data link A into whole frames, in order.
 *
 * Applies oracles O1 and O4 to each command frame.
 *
 * @param[in,out] r      the report
 * @param[in,out] rig    the rig
 * @param[in,out] m      the model
 * @param[in]     session  session number
 * @return nothing
 */
void collectSent(Report & r, Rig & rig, Model & m, uint64_t session)
{
  for (uint8_t byte : rig.transport.sent) {
    frame::Frame f{};
    if (!rig.parser.feed(byte, 0, f)) {
      continue;
    }
    SentFrame s{f.type, f.destination, f.sequence,
                std::vector<uint8_t>(f.payload, f.payload + f.length)};
    if (s.type == frame::kTypeCommand && s.destination == frame::kDestController &&
        !s.payload.empty()) {
      const uint8_t id = s.payload[0];
      m.counts[5] += dangerousController(id) ? 1U : 0U;
      if (dangerousController(id)) {
        const SentFrame & p = m.prev_frame;
        const bool paired = m.have_prev_frame && p.type == frame::kTypeCommand &&
          p.destination == frame::kDestController && p.payload.size() == 6U &&
          p.payload[0] == 0x0DU && p.payload[1] == id && readLe32(&p.payload[2]) == s.sequence;
        m.tokens--;
        if (!paired) {
          finding(r, "dangerous command sent to the controller without its ARM frame directly "
            "before it", session, s.payload.data(), s.payload.size(),
            "Interfaces Sec. 5.2, REQ-029, REQ-112: ARM naming it, then the command");
        }
        if (m.tokens < 0) {
          finding(r, "more dangerous commands reached the controller than ARMs were accepted",
            session, s.payload.data(), s.payload.size(), "REQ-029: one ARM, one command");
          m.tokens = 0;
        }
      }
      const bool ctrl_own = (id >= kCtrlIdFirst && id <= kCtrlIdLast) || id == 0x0D;
      const bool upload = id >= 0x01 && id <= 0x04;
      // The test image (image number 0xFFFF, Sec. 5.5 step 7) goes out on its own after a TRIAL
      // HEALTH report, which a mutated controller frame can also make.
      const bool test_image = upload && s.payload.size() >= 3U && s.payload[1] == 0xFFU &&
                              s.payload[2] == 0xFFU && m.counts[2] > 0U;
      if (!ctrl_own && !(upload && m.delivery_possible) && !test_image) {
        finding(r, "a command identifier outside the controller's list was sent on data link A",
          session, s.payload.data(), s.payload.size(),
          "Sec. 5.1 rows 0x20 to 0x26, the ARM, and UPLOAD only in a delivery or the test image");
      }
      m.unanswered.push_back(s);
    }
    m.prev_frame = s;
    m.have_prev_frame = true;
  }
  rig.transport.sent.clear();
}

/**
 * @brief Reads every datagram the updater sent to the ground and applies O2, O3 and O5.
 *
 * @param[in,out] r        the report
 * @param[in,out] rig      the rig
 * @param[in,out] m        the model
 * @param[in]     session  session number
 * @return nothing
 */
void drainGround(Report & r, Rig & rig, Model & m, uint64_t session)
{
  std::array<uint8_t, 2048> buf{};
  for (size_t guard = 0; guard < 100000U; guard++) {   // bounded
    const ssize_t n = ::recv(rig.client, buf.data(), buf.size(), MSG_DONTWAIT);
    if (n <= 0) {
      return;
    }
    frame::Frame f{};
    if (!frame::decode(buf.data(), static_cast<size_t>(n), f) ||
        f.destination != frame::kDestGround) {
      finding(r, "the updater sent the ground a datagram that is no good frame to Destination 3",
        session, buf.data(), static_cast<size_t>(n), "Interfaces Sec. 4.0");
      continue;
    }
    if (f.type == frame::kTypeReport) {
      m.counts[4]++;
      continue;
    }
    frame::Answer a{};
    if (f.type != frame::kTypeAnswer || !frame::decodeAnswer(f, a) || a.kind < 1U || a.kind > 4U) {
      finding(r, "a frame to the ground is neither a report nor a well-formed answer", session,
        buf.data(), static_cast<size_t>(n), "Interfaces Sec. 4.2: kind 1 to 4, 6 bytes");
      continue;
    }
    m.counts[3]++;
    m.answer_hist[(static_cast<uint32_t>(a.kind) << 8) | a.code]++;
    auto it = m.by_seq.find(a.answered);
    if (it == m.by_seq.end()) {
      finding(r, "answer to a sequence number the ground never sent in a good command frame",
        session, buf.data(), static_cast<size_t>(n), "Sec. 4.2: a dropped frame gets no answer");
      continue;
    }
    bool all_refuse = true;
    bool any_arm = false;
    bool all_danger = true;   // no other command shared the number: the answer is unambiguous
    uint8_t any_id = 0;
    for (const GroundSent & g : it->second) {
      all_refuse = all_refuse && g.must_refuse;
      any_arm = any_arm || (g.arm_valid && g.id == 0x0D);
      all_danger = all_danger && g.onboard_danger;
      any_id = g.id;
    }
    if (all_refuse && a.kind != answer::kRefused) {
      std::string why = "answer kind " + std::to_string(a.kind) + " to seq " +
        std::to_string(a.answered) + ", sent as:";
      for (const GroundSent & g : it->second) {
        why += " [dest " + std::to_string(g.dest) + " id " + std::to_string(g.id) + " len " +
          std::to_string(g.length) + (g.must_refuse ? " refuse" : "") + "]";
      }
      finding(r, "a command that must be refused was answered RECEIVED, DONE or ALREADY DONE",
        session, buf.data(), static_cast<size_t>(n), why.c_str());
    }
    if (a.kind == answer::kDone && it->second.size() == 1U && it->second[0].dest == 1U &&
        (any_id == 0x07U || any_id == 0x08U || any_id == 0x0CU)) {
      finding(r, "RESTORE, REPROGRAM or REPLACE KEY ended DONE although no handler exists",
        session, buf.data(), static_cast<size_t>(n),
        "updater.hpp: after the ARM check they are REFUSED 0x06 not built");
    }
    if (a.kind == answer::kDone) {
      m.done_by_id[any_id]++;
      if (any_arm) {
        m.tokens++;
        m.counts[6]++;
      } else if (all_danger) {
        m.tokens--;
        m.clock_pristine = m.clock_pristine && any_id != 0x09;
        if (m.tokens < 0) {
          std::string why = "answer seq " + std::to_string(a.answered) + " history:";
          for (const GroundSent & g : it->second) {
            why += " [dest " + std::to_string(g.dest) + " id " + std::to_string(g.id) + " len " +
              std::to_string(g.length) + "]";
          }
          finding(r, "a dangerous onboard command ended DONE without an ARM that named it",
            session, buf.data(), static_cast<size_t>(n), why.c_str());
          m.tokens = 0;
        }
      }
    }
  }
}

/**
 * @brief Delivers one frame from the controller to the updater, in one read.
 *
 * @param[in,out] r        the report
 * @param[in,out] rig      the rig
 * @param[in,out] m        the model
 * @param[in]     session  session number
 * @param[in]     bytes    the frame bytes
 * @param[in]     chunks   how many reads to split it into (1 to 3), with a gap of 1 to 15 ms
 * @param[in,out] rng      the generator
 * @return nothing
 */
void toController(Report & r, Rig & rig, Model & m, uint64_t session,
  const std::vector<uint8_t> & bytes, uint32_t chunks, Rng & rng)
{
  inputBegin(r, session, bytes.data(), bytes.size());
  size_t from = 0;
  for (uint32_t c = 0; c < chunks; c++) {
    const size_t to = (c + 1U == chunks) ? bytes.size() : from + bytes.size() / chunks;
    rig.transport.incoming.assign(bytes.begin() + static_cast<long>(from),
      bytes.begin() + static_cast<long>(to));
    rig.listen_ms += (chunks == 1U) ? 100U : 1U + below(rng, 15U);
    (void)rig.updater.onLinkReadable(rig.listen_ms, rig.now_ms);
    from = to;
  }
  inputEnd(r, session);
  m.counts[2]++;
  collectSent(r, rig, m, session);
  drainGround(r, rig, m, session);
}

/**
 * @brief A random payload for a controller report or answer frame.
 *
 * @param[in,out] rng   the generator
 * @param[in]     type  frame type it travels in
 * @return the payload
 */
std::vector<uint8_t> controllerPayload(Rng & rng, uint8_t type)
{
  if (type == frame::kTypeAnswer) {
    std::vector<uint8_t> p(chance(rng, 90U) ? 6U : below(rng, 12U));
    for (auto & x : p) {
      x = static_cast<uint8_t>(nextU64(rng));
    }
    return p;
  }
  const bool health = chance(rng, 50U);
  size_t len = health ? 24U : 15U + below(rng, 33U);
  if (chance(rng, 8U)) {
    len = below(rng, 60U);
  }
  std::vector<uint8_t> p = randomBytes(rng, len);
  if (p.size() > 14U) {
    p[12] = chance(rng, 92U) ? (health ? 1U : 2U) : static_cast<uint8_t>(nextU64(rng));
    if (health) {
      p[13] = chance(rng, 85U) ? static_cast<uint8_t>(1U + below(rng, 3U))
                               : static_cast<uint8_t>(nextU64(rng));
    } else {
      static const uint16_t codes[16] = {1, 2, 3, 5, 6, 7, 8, 9, 0xA, 0xB, 0x101, 0x102, 0x104,
                                         0x105, 0x106, 0xFFFF};
      writeLe16(&p[13], chance(rng, 85U) ? codes[below(rng, 16U)]
                                         : static_cast<uint16_t>(nextU64(rng)));
    }
    std::memset(p.data(), 0, 8);
  }
  return p;
}

/**
 * @brief The played controller: answers the updater's commands, or sends reports.
 *
 * Takes each command frame the updater sent that is not answered yet and answers it RECEIVED then
 * DONE (80 per cent), REFUSED with a random code (10), or not at all (10). At most 16 per call
 * (B2: a bound).
 *
 * @param[in,out] r        the report
 * @param[in,out] rig      the rig
 * @param[in,out] m        the model
 * @param[in]     session  session number
 * @param[in,out] rng      the generator
 * @return nothing
 */
void controllerAnswers(Report & r, Rig & rig, Model & m, uint64_t session, Rng & rng)
{
  for (size_t round = 0; round < 16U && !m.unanswered.empty(); round++) {
    const SentFrame s = m.unanswered.front();
    m.unanswered.erase(m.unanswered.begin());
    const uint32_t mode = below(rng, 100U);
    if (mode >= 90U) {
      continue;   // no answer
    }
    const uint32_t wrong = chance(rng, 3U) ? static_cast<uint32_t>(nextU64(rng)) : s.sequence;
    if (mode < 80U) {
      std::vector<uint8_t> p(6);
      frame::encodeAnswer(frame::Answer{wrong, answer::kReceived, 0}, p.data());
      toController(r, rig, m, session, build(frame::kTypeAnswer, frame::kDestOnboard, 0,
        rig.ctrl_seq++, 0, p), 1, rng);
      frame::encodeAnswer(frame::Answer{wrong, answer::kDone, 0}, p.data());
      toController(r, rig, m, session, build(frame::kTypeAnswer, frame::kDestOnboard, 0,
        rig.ctrl_seq++, 0, p), 1, rng);
    } else {
      std::vector<uint8_t> p(6);
      frame::encodeAnswer(frame::Answer{wrong, answer::kRefused,
        static_cast<uint8_t>(chance(rng, 70U) ? 1U + below(rng, 0x13U) : nextU64(rng))}, p.data());
      toController(r, rig, m, session, build(frame::kTypeAnswer, frame::kDestOnboard, 0,
        rig.ctrl_seq++, 0, p), 1, rng);
    }
  }
}

/**
 * @brief Sends one datagram from the fake ground to the updater and lets it read it.
 *
 * @param[in,out] r        the report
 * @param[in,out] rig      the rig
 * @param[in,out] m        the model
 * @param[in]     session  session number
 * @param[in]     d        the datagram
 * @return nothing
 */
void toGround(Report & r, Rig & rig, Model & m, uint64_t session, const std::vector<uint8_t> & d)
{
  recordGround(m, d);
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(rig.port);
  to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  (void)::sendto(rig.client, d.data(), d.size(), 0, reinterpret_cast<const sockaddr *>(&to),
    sizeof to);
  pollfd p{};
  p.fd = rig.gate.fd();
  p.events = POLLIN;
  inputBegin(r, session, d.data(), d.size());
  // Wait up to 2 s for the datagram (a loaded Mac can delay a loopback datagram), then read
  // every datagram that is queued, so that the updater has seen all that was sent.
  if (::poll(&p, 1, kDeliverWaitMs) == 1) {
    for (size_t k = 0; k < 64U; k++) {   // bounded
      rig.updater.onGroundReadable(rig.now_ms);
      if (::poll(&p, 1, 0) != 1) {
        break;
      }
    }
  }
  inputEnd(r, session);
  m.counts[0]++;
  collectSent(r, rig, m, session);
  drainGround(r, rig, m, session);
}

/**
 * @brief Chooses a ground sequence number: mostly fresh, sometimes a repeat or a boundary.
 *
 * @param[in,out] rig  the rig (its counter)
 * @param[in,out] rng  the generator
 * @return the sequence number
 */
uint32_t pickSequence(Rig & rig, Rng & rng)
{
  const uint32_t k = below(rng, 100U);
  if (k < 85U) {
    return rig.ground_seq++;
  }
  if (k < 95U) {
    return rig.ground_seq > 8U ? rig.ground_seq - 1U - below(rng, 8U) : 1U;
  }
  if (k < 97U) {
    return static_cast<uint32_t>(boundaryValue(rng, 4U));
  }
  return static_cast<uint32_t>(nextU64(rng));
}

/**
 * @brief Chooses an Expiry: mostly the latest time, sometimes 0, in the past, or random.
 *
 * @param[in,out] rng  the generator
 * @return the Expiry
 */
uint32_t pickExpiry(Rng & rng)
{
  const uint32_t k = below(rng, 100U);
  if (k < 85U) {
    return 0xFFFFFFFFU;
  }
  if (k < 90U) {
    return 0U;
  }
  if (k < 95U) {
    return 1U + below(rng, kCertainlyPast - 1U);
  }
  return static_cast<uint32_t>(nextU64(rng));
}

/**
 * @brief A command payload for a destination: mostly a known identifier with the right length.
 *
 * @param[in,out] rng   the generator
 * @param[in]     dest  Destination byte
 * @param[in]     seeds the image seeds (for the lengths of UPLOAD BEGIN)
 * @return the payload
 */
std::vector<uint8_t> commandPayload(Rng & rng, uint8_t dest, const std::vector<ImageSeed> & seeds)
{
  static const uint8_t onboard_ids[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0x0B, 0x0C, 0x0D, 0x25, 0x0A,
                                          0x0E, 0x40};
  static const uint8_t ctrl_ids[12] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x0D, 1, 2, 3,
                                       0x27};
  uint8_t id = (dest == 1U) ? onboard_ids[below(rng, 16U)] : ctrl_ids[below(rng, 12U)];
  if (chance(rng, 8U)) {
    id = static_cast<uint8_t>(nextU64(rng));
  }
  size_t want = 1;
  switch (id) {
    case 1: want = 7; break;
    case 2: want = 7 + 1U + below(rng, 256U); break;
    case 3: case 4: case 5: want = 3; break;
    case 6: want = 7; break;
    case 8: want = 4; break;
    case 9: case 0x0B: case 0x24: want = 5; break;
    case 0x0C: want = 34; break;
    case 0x0D: want = 6; break;
    case 0x22: want = 2; break;
    default: want = 1; break;   // input from the link: other identifiers take no fields
  }
  size_t len = want;
  if (chance(rng, 25U)) {
    const uint32_t k = below(rng, 6U);
    len = (k == 0U) ? 0U : (k == 1U) ? want + 1U : (k == 2U) ? (want > 0U ? want - 1U : 0U)
        : (k == 3U) ? 1024U : (k == 4U) ? 1025U : below(rng, 300U);
  }
  std::vector<uint8_t> p = randomBytes(rng, len);
  if (len > 0U) {
    p[0] = id;
  }
  const uint16_t numbers[6] = {0, 1, 2, 3, 0xFFFF, static_cast<uint16_t>(nextU64(rng))};
  if ((id <= 5U) && len >= 3U) {
    writeLe16(&p[1], numbers[below(rng, 6U)]);
  }
  if (id == 1U && len >= 7U) {
    const uint32_t lens[6] = {0, 192, 193, 3032, 40960, 40961};
    writeLe32(&p[3], chance(rng, 50U) && !seeds.empty()
      ? static_cast<uint32_t>(seeds[below(rng, static_cast<uint32_t>(seeds.size()))].bytes.size())
      : lens[below(rng, 6U)]);
  }
  if (id == 2U && len >= 7U) {
    writeLe32(&p[3], chance(rng, 60U) ? 256U * below(rng, 4U)
                                      : static_cast<uint32_t>(boundaryValue(rng, 4U)));
  }
  if (id == 6U && len >= 7U) {
    writeLe32(&p[1], chance(rng, 50U) ? 1U + below(rng, 3U)
                                      : static_cast<uint32_t>(boundaryValue(rng, 4U)));
    writeLe16(&p[5], chance(rng, 70U) ? 0x0431U : static_cast<uint16_t>(nextU64(rng)));
  }
  if (id == 9U && len >= 5U) {
    writeLe32(&p[1], chance(rng, 70U) ? 1790000000U + below(rng, 100000U)
                                      : static_cast<uint32_t>(boundaryValue(rng, 4U)));
  }
  if (id == 0x0BU && len >= 5U) {
    writeLe32(&p[1], chance(rng, 70U) ? below(rng, 20U)
                                      : static_cast<uint32_t>(boundaryValue(rng, 4U)));
  }
  if (id == 0x0DU && len >= 6U) {
    const uint8_t named_on[5] = {6, 7, 8, 9, 0x0C};
    const uint8_t named_ct[5] = {0x20, 0x21, 0x22, 0x24, 0x26};
    p[1] = chance(rng, 85U) ? ((dest == 1U) ? named_on[below(rng, 5U)] : named_ct[below(rng, 5U)])
                            : static_cast<uint8_t>(nextU64(rng));
  }
  return p;
}

/**
 * @brief One ground command op: a structured frame, or a mutated or random datagram.
 *
 * @param[in,out] r        the report
 * @param[in,out] rig      the rig
 * @param[in,out] m        the model
 * @param[in]     session  session number
 * @param[in,out] rng      the generator
 * @param[in]     seeds    image seeds
 * @return nothing
 */
void groundOp(Report & r, Rig & rig, Model & m, uint64_t session, Rng & rng,
  const std::vector<ImageSeed> & seeds)
{
  const uint8_t dest = chance(rng, 55U) ? 1U : (chance(rng, 85U) ? 2U
                                                : static_cast<uint8_t>(nextU64(rng)));
  std::vector<uint8_t> payload = commandPayload(rng, dest, seeds);
  const uint8_t type = chance(rng, 94U) ? 1U : static_cast<uint8_t>(nextU64(rng));
  const uint8_t flags = chance(rng, 95U) ? 0U : static_cast<uint8_t>(nextU64(rng));
  std::vector<uint8_t> d = build(type, dest, flags, pickSequence(rig, rng), pickExpiry(rng),
    payload);
  if (chance(rng, 10U)) {
    const std::vector<Field> fields = {{0, 2}, {2, 1}, {3, 1}, {4, 1}, {6, 2}, {8, 4}, {12, 4}};
    mutate(rng, d, fields, 1100U, nullptr);
    if (chance(rng, 60U) && d.size() >= 20U) {
      writeLe32(&d[d.size() - 4U], refCrc32(d.data(), d.size() - 4U));
      if (d.size() >= 8U) {
        writeLe16(&d[6], static_cast<uint16_t>(d.size() - 20U));
      }
      writeLe32(&d[d.size() - 4U], refCrc32(d.data(), d.size() - 4U));
    }
    m.counts[1]++;
  } else if (chance(rng, 3U)) {
    d = randomBytes(rng, below(rng, 3000U));
    m.counts[1]++;
  }
  toGround(r, rig, m, session, d);
}

/**
 * @brief An ARM and then the dangerous command it names; sometimes out of order, late or twice.
 *
 * @param[in,out] r        the report
 * @param[in,out] rig      the rig
 * @param[in,out] m        the model
 * @param[in]     session  session number
 * @param[in,out] rng      the generator
 * @return nothing
 */
void pairOp(Report & r, Rig & rig, Model & m, uint64_t session, Rng & rng)
{
  const bool onboard = chance(rng, 50U);
  static const uint8_t named_on[5] = {6, 7, 8, 9, 0x0C};
  static const uint8_t named_ct[5] = {0x20, 0x21, 0x22, 0x24, 0x26};
  const uint8_t id = onboard ? named_on[below(rng, 5U)] : named_ct[below(rng, 5U)];
  const uint8_t dest = onboard ? 1U : 2U;
  std::vector<uint8_t> cmd = commandPayload(rng, dest, std::vector<ImageSeed>{});
  if (!cmd.empty()) {
    cmd[0] = id;
  }
  if (chance(rng, 85U)) {
    const size_t want = (id == 0x06U) ? 7U : (id == 0x07U) ? 1U : (id == 0x08U) ? 4U
      : (id == 0x09U) ? 5U : (id == 0x0CU) ? 34U : (id == 0x22U) ? 2U : (id == 0x24U) ? 5U : 1U;
    cmd.resize(want, 0x31U);
    cmd[0] = id;
  }
  const uint32_t arm_seq = rig.ground_seq++;
  const uint32_t cmd_seq = rig.ground_seq++;
  std::vector<uint8_t> arm = {0x0DU, id, 0, 0, 0, 0};
  writeLe32(&arm[2], chance(rng, 90U) ? cmd_seq : static_cast<uint32_t>(nextU64(rng)));
  if (chance(rng, 5U)) {
    arm[1] = static_cast<uint8_t>(nextU64(rng));
  }
  const uint32_t expiry = 0xFFFFFFFFU;
  const bool backwards = chance(rng, 6U);
  if (!backwards) {
    toGround(r, rig, m, session, build(1, dest, 0, arm_seq, expiry, arm));
    controllerAnswers(r, rig, m, session, rng);
  }
  if (chance(rng, 8U)) {
    rig.now_ms += 30001U;   // the ARM is too old by the time the command comes
  }
  toGround(r, rig, m, session, build(1, dest, 0, cmd_seq, expiry, cmd));
  controllerAnswers(r, rig, m, session, rng);
  if (backwards) {
    toGround(r, rig, m, session, build(1, dest, 0, arm_seq, expiry, arm));
    controllerAnswers(r, rig, m, session, rng);
  }
  if (chance(rng, 20U)) {
    toGround(r, rig, m, session, build(1, dest, 0, cmd_seq, expiry, cmd));   // a replay
    controllerAnswers(r, rig, m, session, rng);
  }
  if (chance(rng, 10U)) {
    toGround(r, rig, m, session, build(1, dest, 0, arm_seq, expiry, arm));   // ARM replay
    controllerAnswers(r, rig, m, session, rng);
  }
}

/**
 * @brief An upload of an image seed (sometimes damaged or incomplete), then maybe a DELIVER.
 *
 * @param[in,out] r        the report
 * @param[in,out] rig      the rig
 * @param[in,out] m        the model
 * @param[in]     session  session number
 * @param[in,out] rng      the generator
 * @param[in]     seeds    image seeds
 * @return nothing
 */
void uploadOp(Report & r, Rig & rig, Model & m, uint64_t session, Rng & rng,
  const std::vector<ImageSeed> & seeds)
{
  if (seeds.empty()) {
    return;
  }
  std::vector<uint8_t> img = seeds[below(rng, static_cast<uint32_t>(seeds.size()))].bytes;
  if (img.size() > 5000U && chance(rng, 80U)) {
    img = seeds[0].bytes;
  }
  if (chance(rng, 20U)) {
    mutate(rng, img, {{0, 4}, {12, 4}, {16, 4}}, 6000U, nullptr);
  }
  const uint16_t number = static_cast<uint16_t>(1U + below(rng, 3U));
  std::vector<uint8_t> begin = {0x01U, 0, 0, 0, 0, 0, 0};
  writeLe16(&begin[1], number);
  writeLe32(&begin[3], static_cast<uint32_t>(img.size()));
  toGround(r, rig, m, session, build(1, 1, 0, rig.ground_seq++, 0xFFFFFFFFU, begin));
  controllerAnswers(r, rig, m, session, rng);
  const size_t chunk = config::kChunkBytes;
  for (size_t off = 0; off < img.size(); off += chunk) {
    if (chance(rng, 4U)) {
      continue;   // a skipped chunk
    }
    const size_t n = std::min(chunk, img.size() - off);
    std::vector<uint8_t> c = {0x02U, 0, 0, 0, 0, 0, 0};
    writeLe16(&c[1], number);
    writeLe32(&c[3], static_cast<uint32_t>(off));
    c.insert(c.end(), img.begin() + static_cast<long>(off),
             img.begin() + static_cast<long>(off + n));
    toGround(r, rig, m, session, build(1, 1, 0, rig.ground_seq++, 0xFFFFFFFFU, c));
    if (chance(rng, 5U)) {
      toGround(r, rig, m, session, build(1, 1, 0, rig.ground_seq - 1U, 0xFFFFFFFFU, c));
    }
  }
  std::vector<uint8_t> end = {0x03U, 0, 0};
  writeLe16(&end[1], number);
  toGround(r, rig, m, session, build(1, 1, 0, rig.ground_seq++, 0xFFFFFFFFU, end));
  if (chance(rng, 60U)) {
    std::vector<uint8_t> health = controllerPayload(rng, frame::kTypeReport);
    health.assign(24U, 0U);
    health[12] = 1U;
    health[13] = chance(rng, 90U) ? 1U : static_cast<uint8_t>(1U + below(rng, 3U));
    toController(r, rig, m, session, build(frame::kTypeReport, frame::kDestOnboard, 0,
      rig.ctrl_seq++, 0, health), 1, rng);
    m.delivery_possible = true;
    std::vector<uint8_t> dv = {0x05U, 0, 0};
    writeLe16(&dv[1], number);
    toGround(r, rig, m, session, build(1, 1, 0, rig.ground_seq++, 0xFFFFFFFFU, dv));
    for (size_t step = 0; step < 100U && !m.unanswered.empty(); step++) {   // bounded
      controllerAnswers(r, rig, m, session, rng);
      rig.now_ms += 10U;
      rig.updater.tick(rig.now_ms);
      collectSent(r, rig, m, session);
      drainGround(r, rig, m, session);
    }
  }
}

/**
 * @brief One controller op: a report or answer, or raw noise, split over reads.
 *
 * @param[in,out] r        the report
 * @param[in,out] rig      the rig
 * @param[in,out] m        the model
 * @param[in]     session  session number
 * @param[in,out] rng      the generator
 * @return nothing
 */
void controllerOp(Report & r, Rig & rig, Model & m, uint64_t session, Rng & rng)
{
  const uint32_t k = below(rng, 100U);
  uint8_t type = (k < 70U) ? frame::kTypeReport : frame::kTypeAnswer;
  if (chance(rng, 5U)) {
    type = static_cast<uint8_t>(nextU64(rng));
  }
  const uint8_t dest = chance(rng, 92U) ? frame::kDestOnboard : static_cast<uint8_t>(nextU64(rng));
  const std::vector<uint8_t> payload = controllerPayload(rng, type);
  if (type == frame::kTypeReport && payload.size() > 14U && payload[12] == 1U &&
      payload[13] == report::kModeTrial) {
    m.delivery_possible = true;
  }
  std::vector<uint8_t> d = build(type, dest, 0, rig.ctrl_seq++, 0, payload);
  if (chance(rng, 12U)) {
    mutate(rng, d, {{0, 2}, {6, 2}, {3, 1}, {4, 1}}, 1100U, nullptr);
  }
  if (chance(rng, 3U)) {
    d = randomBytes(rng, below(rng, 100U));
  }
  toController(r, rig, m, session, d, 1U + below(rng, 3U), rng);
  controllerAnswers(r, rig, m, session, rng);
}

/**
 * @brief Moves the clock on and runs the timers.
 *
 * @param[in,out] r        the report
 * @param[in,out] rig      the rig
 * @param[in,out] m        the model
 * @param[in]     session  session number
 * @param[in,out] rng      the generator
 * @return nothing
 */
void tickOp(Report & r, Rig & rig, Model & m, uint64_t session, Rng & rng)
{
  rig.now_ms += kTickSteps[below(rng, 12U)];
  inputBegin(r, session, nullptr, 0);
  rig.updater.tick(rig.now_ms);
  inputEnd(r, session);
  collectSent(r, rig, m, session);
  drainGround(r, rig, m, session);
  controllerAnswers(r, rig, m, session, rng);
}

/**
 * @brief Runs one session on a fresh rig.
 *
 * @param[in,out] r        the report
 * @param[in]     session  session number (the index of the input)
 * @param[in]     port     UDP port
 * @param[in]     seeds    image seeds
 * @param[in,out] hist     the totals over all sessions (counts[8], answer hist, done by id)
 * @param[out]    max_dir  the largest data directory seen, bytes
 * @return true: the rig came up; false: it did not
 */
bool runSession(Report & r, uint64_t session, uint16_t port, const std::vector<ImageSeed> & seeds,
  Model & hist, uint64_t & max_dir)
{
  const std::string dir = r.opt.work_dir + "/updater";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  std::unique_ptr<Rig> rig(new Rig(dir, port));
  Model m;
  rig->client = ::socket(AF_INET, SOCK_DGRAM, 0);
  (void)::setsockopt(rig->client, SOL_SOCKET, SO_RCVBUF, &kClientBuffer, sizeof kClientBuffer);
  if (rig->client < 0 || !rig->gate.open() || !rig->store.ready() || !rig->reports.load() ||
      !rig->checker.ready()) {
    return false;
  }
  Rng rng = rngFor(r.opt.seed, session);
  for (uint64_t guard = 0; guard < 8U * kOpsPerSession
       && m.counts[0] + m.counts[2] < kOpsPerSession;
       guard++) {   // bounded
    const uint32_t k = below(rng, 100U);
    if (k < 38U) {
      groundOp(r, *rig, m, session, rng, seeds);
    } else if (k < 46U) {
      pairOp(r, *rig, m, session, rng);
    } else if (k < 49U) {
      uploadOp(r, *rig, m, session, rng, seeds);
    } else if (k < 82U) {
      controllerOp(r, *rig, m, session, rng);
    } else {
      tickOp(r, *rig, m, session, rng);
    }
  }
  max_dir = std::max(max_dir, dirBytes(dir));
  for (int i = 0; i < 8; i++) {
    hist.counts[i] += m.counts[i];
  }
  for (const auto & kv : m.answer_hist) {
    hist.answer_hist[kv.first] += kv.second;
  }
  for (const auto & kv : m.done_by_id) {
    hist.done_by_id[kv.first] += kv.second;
  }
  return true;
}

}  // namespace

/**
 * @brief Target 3: sessions against a fresh Updater, with the oracles O1 to O5.
 *
 * The number of sessions is count / kOpsPerSession, rounded up; with --only the one session
 * asked for. Resident size is logged at the first, the middle and the last session and every
 * tenth; growth over the run above kRssGrowthMax is a finding. The largest data directory is
 * logged; one above kDirMaxBytes is a finding.
 *
 * @param[in,out] r  the report (options, log)
 * @return number of findings
 */
int runUpdater(Report & r)
{
  std::vector<ImageSeed> seeds;
  loadImages(r.opt.test_images, seeds);
  loadImages(r.opt.ctrl_images, seeds);
  std::sort(seeds.begin(), seeds.end(), [](const ImageSeed & a, const ImageSeed & b) {
    return a.bytes.size() < b.bytes.size();
  });
  uint16_t port = kPortFirst;
  bool bound = false;
  for (int t = 0; t < kPortTries && !bound; t++) {
    CommandGate probe(static_cast<uint16_t>(kPortFirst + t));
    bound = probe.open();
    port = static_cast<uint16_t>(kPortFirst + t);
  }
  if (!bound) {
    (void)std::fprintf(r.log, "FATAL: no free UDP port from %u\n", kPortFirst);
    r.findings++;
    return 1;
  }
  const uint64_t sessions = (r.opt.count + kOpsPerSession - 1U) / kOpsPerSession;
  const uint64_t lo = r.opt.has_only ? r.opt.only : 0U;
  const uint64_t hi = r.opt.has_only ? r.opt.only + 1U : sessions;
  Model hist;
  uint64_t max_dir = 0;
  uint64_t rss_first = 0;
  uint64_t rss_last = 0;
  (void)std::fprintf(r.log, "updater: %llu sessions of about %llu injected frames, UDP port %u, "
    "%zu image seeds\n", static_cast<unsigned long long>(hi - lo),
    static_cast<unsigned long long>(kOpsPerSession), port, seeds.size());
  for (uint64_t s = lo; s < hi; s++) {
    if (!runSession(r, s, port, seeds, hist, max_dir)) {
      (void)std::fprintf(r.log, "FATAL: session %llu could not start its rig\n",
        static_cast<unsigned long long>(s));
      r.findings++;
      break;
    }
    const uint64_t rss = residentBytes();
    rss_first = (s == lo) ? rss : rss_first;
    rss_last = rss;
    if (s == lo || s + 1U == hi || (s - lo) % 10U == 0U) {
      (void)std::fprintf(r.log, "session %llu: injected so far %llu, resident %.1f MB\n",
        static_cast<unsigned long long>(s), static_cast<unsigned long long>(r.inputs),
        static_cast<double>(rss) / 1048576.0);
    }
  }
  (void)std::fprintf(r.log, "updater totals: %llu ground datagrams (of which %llu raw or mutated), "
    "%llu controller frames, %llu answers and %llu reports seen at the ground, %llu dangerous "
    "commands sent to the controller, %llu ARMs answered DONE\n",
    static_cast<unsigned long long>(hist.counts[0]),
        static_cast<unsigned long long>(hist.counts[1]),
    static_cast<unsigned long long>(hist.counts[2]),
        static_cast<unsigned long long>(hist.counts[3]),
    static_cast<unsigned long long>(hist.counts[4]),
        static_cast<unsigned long long>(hist.counts[5]),
    static_cast<unsigned long long>(hist.counts[6]));
  (void)std::fprintf(r.log, "answers by kind<<8|code:");
  for (const auto & kv : hist.answer_hist) {
    (void)std::fprintf(r.log, " %04x=%llu", kv.first, static_cast<unsigned long long>(kv.second));
  }
  (void)std::fprintf(r.log, "\nDONE answers by command identifier:");
  for (const auto & kv : hist.done_by_id) {
    (void)std::fprintf(r.log, " %02x=%llu", kv.first, static_cast<unsigned long long>(kv.second));
  }
  (void)std::fprintf(r.log,
                     "\nlargest data directory %.1f KB; resident size first session %.1f MB, "
    "last %.1f MB\n", static_cast<double>(max_dir) / 1024.0,
    static_cast<double>(rss_first) / 1048576.0, static_cast<double>(rss_last) / 1048576.0);
  if (max_dir > kDirMaxBytes) {
    finding(r, "data directory grew beyond its bound", 0, nullptr, 0, "bounded storage expected");
  }
  if (rss_last > rss_first + kRssGrowthMax) {
    finding(r, "resident memory grew across the run", 0, nullptr, 0, "more than 64 MB over run");
  }
  return static_cast<int>(r.findings);
}

}  // namespace fuzz
