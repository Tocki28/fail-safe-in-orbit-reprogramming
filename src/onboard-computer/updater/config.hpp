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
// (Interfaces Sec. 5.2, REQ-030). Proposal (TBR), awaiting the owner's acceptance: 30 s,
// standing for TBD-arm; the controller uses the same 30 s. A bench value.
constexpr uint64_t kArmExpiryMs = 30000;

// ---- Trial time: decision H, Proposal (TBR), awaiting the owner's acceptance -----------------------------------------------------------------------------
// The controller enforces the trial time itself; the onboard computer checks that it did
// (decision H of the controller audit). Trial time 120 s: bench value (TBD-trial in the
// requirements). Margin 30 s: Proposal (TBR), awaiting the owner's acceptance. No EVENT 0x0007
// (confirmed) or 0x0008 (given up) within trial time + margin after the trial began means the
// controller's own limit did not work (trial_watch.hpp).
// Proposal (TBR), awaiting the owner's acceptance.
constexpr uint64_t kTrialMs = 120000;
// Proposal (TBR), awaiting the owner's acceptance.
constexpr uint64_t kTrialMarginMs = 30000;

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
// Ground sequence numbers already used by an ARM or a dangerous command since start; a repeat
// is refused 0x04 (REQ-065). Bench choice: a ring of the last 64; the oldest is forgotten, so
// a replay older than 64 such commands is not caught (open: REQ-065 keeps the counter over
// restarts).
constexpr size_t kUsedSequenceCount = 64;
// The "dropped datagram" log lines on the ground link: at most one per this many ms, so a
// flood of bad datagrams cannot fill the log (audit finding #8).
constexpr uint64_t kDropLogEveryMs = 1000;

// ---- Main loop -----------------------------------------------------------------------------
// poll() wakes at least this often to run the timers (answer timeouts, controller watch).
constexpr int kPollMs = 50;

}  // namespace config

#endif  // FSIR_UPDATER_CONFIG_HPP
