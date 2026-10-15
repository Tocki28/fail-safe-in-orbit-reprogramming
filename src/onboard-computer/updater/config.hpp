/*
 * config.hpp — every fixed setting of the updater, each with its source or the reason for it.
 *
 * Image sizes and header offsets come from the controller's own headers
 * (src/controller/common/layout.h and image_format.h), so the onboard computer, the bootloader
 * and the image tool read one definition. Only data is shared with the controller, never
 * checking code (REQ-130).
 */
#ifndef FSIR_UPDATER_CONFIG_HPP
#define FSIR_UPDATER_CONFIG_HPP

#include <cstddef>
#include <cstdint>

#include "image_format.h"
#include "layout.h"

namespace config
{

// ---- Ground link (bench) -----------------------------------------------------------------
// Bench stand-in for the radio link (Interfaces Sec. 6.1): UDP, one frame per datagram, on all
// interfaces. Port 47001 is a bench choice shared with the ground console; the Interfaces
// document has no number for it because the radio equipment lies outside the System.
constexpr uint16_t kDefaultUdpPort = 47001;

// ---- Data link A (bench) -----------------------------------------------------------------
// The controller board's ST-LINK virtual COM port, plugged into the onboard computer's USB.
constexpr const char * kDefaultDevice = "/dev/ttyACM0";
// A frame with a gap of more than 10 ms inside it is dropped (Interfaces Sec. 6.2).
constexpr uint64_t kFrameGapMaxMs = 10;
// How long to wait before trying to open a missing serial device again. The ST-LINK leaves
// the onboard computer's USB whenever its cable is moved to the Mac to write the controller.
constexpr uint64_t kReopenEveryMs = 1000;

// Stop-and-wait: the next frame goes to the controller only after the controller's last answer
// (DONE, REFUSED or ALREADY DONE) to the previous one, because the controller cannot receive
// while it writes its flash. These are the longest waits for that last answer.
//
// Most commands: 2 s, a bench value from the build brief. The longest frame the onboard
// computer sends, an UPLOAD CHUNK, is 16 + 263 + 4 = 283 bytes, 24.6 ms at 115200 baud and 10
// bits per byte; the two answers, 2 x 26 bytes, take 4.5 ms. The controller's time to check an
// image at UPLOAD END (CRC-32 and two signatures) is not measured yet (TBD).
constexpr uint32_t kAnswerTimeoutMs = 2000;
// UPLOAD BEGIN: the controller erases the slot before it answers. A slot is 40 KB of 2 KB pages
// (layout.h: SLOT_BYTES 0xA000 / FLASH_PAGE_BYTES 0x800 = 20 pages); one page erase takes up to
// 24.47 ms [DS12589 Rev 6, p. 107, as cited in docs/hardware-trade-study], so up to
// 20 x 24.47 = 489.4 ms, plus 7 ms on the line. 3 s keeps a margin of about six times.
constexpr uint32_t kBeginTimeoutMs = 3000;

// GET STATUS to the controller when the link is idle, to show the link works (Interfaces
// Sec. 6.2: "every TBD-beat"). 0 = off, the default; set with --beat-s.
constexpr uint32_t kDefaultBeatS = 0;
constexpr uint32_t kMaxBeatS = 3600;

// ---- Arming ----------------------------------------------------------------------------------
// How long the onboard computer holds a ground ARM before a dangerous command must use it
// (Interfaces Sec. 5.2, REQ-030). Accepted by the owner, 13 Oct 2026 (decision Q): 30 s, a bench
// value standing for TBD-arm; the controller uses the same 30 s.
constexpr uint64_t kArmExpiryMs = 30000;

// ---- Trial time and restart: decision H, accepted by the owner, 13 Oct 2026 ------------------
// The controller enforces the trial time itself; the onboard computer checks that it did
// (decision H of the controller audit). Trial time 120 s: bench value standing for TBD-trial
// in the requirements. Margin 30 s: accepted by the owner, 13 Oct 2026. No EVENT 0x0007
// (confirmed) or 0x0008 (given up) within trial time + margin after the trial began means the
// controller's own limit did not work (trial_watch.hpp); the updater then runs the restart
// script (restart_runner.hpp) and tells the ground with EVENT 0x0106.
constexpr uint64_t kTrialMs = 120000;
constexpr uint64_t kTrialMarginMs = 30000;
// The restart script: restore.sh in mode "reset" (restore.sh header), looked for next to the
// updater's own program file unless --restore-script names another (options.hpp).
constexpr const char * kRestoreScriptName = "restore.sh";
// The longest the updater lets the restart script run before it sends it TERM: the script's own
// limit for OpenOCD (OPENOCD_TIMEOUT_S, 30 s, set from a full restore measured at 4.0 s on the
// bench and accepted by the owner, 13 Oct 2026; restore.sh) plus its wait between TERM and KILL
// (KILL_GRACE_S 5 s) plus 5 s, so that the script's own time-out line arrives before the updater
// steps in.
constexpr uint64_t kRestartScriptMs = 40000;
// With these values a second trial overrun cannot arrive while a restart script still runs: the
// updater stops the script at kRestartScriptMs, before a new trial plus its margin can pass. The
// guard "no second restart script while one runs" (updater.cpp) covers a clock jump or a later
// change of these values; this check says when the guard is the only thing left in the way.
static_assert(kRestartScriptMs < kTrialMs + kTrialMarginMs,
  "a restart script must be stopped before a second overrun can be due");
// After TERM, the wait before the updater sends KILL to the script's whole process group. TBD,
// a PLACEHOLDER (the script's KILL_GRACE_S default is 5 s); not measured.
constexpr uint64_t kRestartKillGraceMs = 5000;

// ---- Controller watch ----------------------------------------------------------------------
// REQ-061: notice a silent controller. Bench value 5 s from the build brief; the requirements
// document has no value yet (TBD).
constexpr uint64_t kSilenceMs = 5000;

// ---- Firmware images -----------------------------------------------------------------------
constexpr const char * kDefaultDataDir = "/var/lib/fsir/images";
// Images move in chunks of 256 bytes (Interfaces Sec. 5.5).
constexpr size_t kChunkBytes = 256;
// The largest region an image is written to: a 40 KB slot (layout.h, SLOT_BYTES). Bootloader
// (24 KB) and last-resort (16 KB) regions are smaller.
constexpr size_t kMaxImageBytes = static_cast<size_t>(SLOT_BYTES);
// Header 64 bytes, then the body, then Signature A and Signature B of 64 bytes each
// (Interfaces Sec. 5.5, Figures 5 and 6; layout.h).
constexpr size_t kImageHeaderBytes = static_cast<size_t>(IMAGE_HEADER_BYTES);
constexpr size_t kSignatureBytes = static_cast<size_t>(IMAGE_SIGNATURE_BYTES);
constexpr size_t kImageOverheadBytes = kImageHeaderBytes + 2 * kSignatureBytes;
// Ed25519 public keys are 32 bytes [RFC 8032, Sec. 5.1.5].
constexpr size_t kPublicKeyBytes = 32;

// ---- Fixed tables --------------------------------------------------------------------------
// Ground commands for the controller waiting for data link A while it is busy, e.g. during a
// delivery. A person at the ground console sends one or two at a time; 8 is ample.
constexpr size_t kForwardQueueSize = 8;
// Data-link sequence number -> ground sequence number, for answers to forwarded commands.
// Kept after a timeout so that a late answer still reaches the ground; the oldest is replaced.
constexpr size_t kForwardTableSize = 16;
// Ground sequence numbers already used by an ARM or a dangerous command since start, with the
// command's payload and whether it has finished. A repeat with other bytes is refused 0x04
// (REQ-065); a repeat with the same bytes is answered RECEIVED or ALREADY DONE (REQ-031, Interfaces
// Sec. 4.2). Bench choice: a ring of the last 64; the oldest is forgotten, so a replay older than
// 64 such commands is not caught (open: REQ-065 keeps the counter over restarts).
constexpr size_t kUsedSequenceCount = 64;
// The most payload bytes the ring keeps per entry: the longest ARM or dangerous command the
// updater accepts is REPLACE KEY, command::kReplaceKeyBytes = 34 (identifier 1, which key 1, new
// key 32; Interfaces Sec. 5.1 table row 0x0C). The ARM has 6 (command::kArmBytes) and SET
// CONTROLLER TIME 5. Raised from 6 to 34 so that the onboard computer's own dangerous commands
// (0x06 to 0x09, 0x0C) get the same resend behaviour as the controller's (REQ-031, REQ-065);
// command_gate.cpp has a static_assert against the protocol.hpp sizes. The bytes are kept as
// they are, not as a digest: that needs no crypto library in the command gate and cannot
// collide, so "same bytes" is exact. Open (TBR, awaiting the
// owner's acceptance): the ring then holds a replacement key's bytes in RAM for as long as the
// entry lives (64 entries); a digest would not.
constexpr size_t kUsedPayloadMax = 34;
// Ground sequence numbers already used by every OTHER ground command (not an ARM or a dangerous
// one) since start, with the command's exact payload and whether it has finished (owner decision
// 26, 15 Oct 2026; REQ-065, REQ-031). A separate memory from the 64 above, so that upload traffic
// (an image is up to 40 KB in 256-byte chunks, 160 UPLOAD CHUNKs at least) cannot push the
// dangerous commands out of theirs. 256: the owner's number, decision 26. Lost at a restart
// (accepted risk, decision 26).
constexpr size_t kRepeatCount = 256;
// The most payload bytes that memory keeps per entry: the longest ground command is UPLOAD CHUNK,
// identifier 1 + image number 2 + offset 4 + data up to 256 = 263 bytes (Interfaces Sec. 5.1
// table, row 0x02; command::kUploadChunkHeadBytes + kChunkBytes, checked by a static_assert in
// command_gate.cpp). Bytes are kept exactly (decision 26). RAM taken by the 256 entries: see the
// static_assert on sizeof(RepeatMemory) in command_gate.cpp (about 70 KB, measured, not guessed).
constexpr size_t kRepeatPayloadMax = 263;

// ---- Image limits (owner decision 24, 15 Oct 2026; REQ-045, REQ-046, REQ-053) -----------------
// The limits are kept in the data directory as a DurableRecord (durable_record.hpp) of this name,
// 6 bytes: minimum version (4, little-endian), controller identity (2, little-endian), the field
// order of SET LIMITS (Interfaces Sec. 5.1 table, row 0x06).
constexpr const char * kLimitsRecordName = "limits";
constexpr size_t kLimitsRecordBytes = 6;
// With no record the onboard computer starts with minimum version 1 (owner decision 24, 15 Oct
// 2026).
constexpr uint32_t kDefaultMinimumVersion = 1U;
// With no record the controller identity is 0x0431. Proposal (TBR), awaiting the owner's
// acceptance: 0x0431 is the target the image checker demands today (image_checker.cpp), copied
// from IMAGE_TARGET_CONTROLLER (image_format.h).
constexpr uint16_t kDefaultControllerIdentity = static_cast<uint16_t>(IMAGE_TARGET_CONTROLLER);
static_assert(kDefaultControllerIdentity == 0x0431U,
  "the proposed default identity is the target the image checker demanded before decision 24");

// The "dropped datagram" log lines on the ground link: at most one per this many ms, so a
// flood of bad datagrams cannot fill the log (audit finding #8).
constexpr uint64_t kDropLogEveryMs = 1000;

// ---- Main loop -----------------------------------------------------------------------------
// poll() wakes at least this often to run the timers (answer timeouts, controller watch).
constexpr int kPollMs = 50;

}  // namespace config

#endif  // FSIR_UPDATER_CONFIG_HPP
