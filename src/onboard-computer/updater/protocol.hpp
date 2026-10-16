/*
 * protocol.hpp — command identifiers (Interfaces Sec. 5.1), answer kinds and refusal reasons
 * (Sec. 4.2), report fields (Sec. 5.4).
 *
 * REQ-100 asks the ground and the onboard software to use one shared list. Bench limit: the
 * ground console is a separate program written from the same Interfaces tables; this file is
 * the onboard computer's copy, not a list both programs read.
 */
#ifndef FSIR_UPDATER_PROTOCOL_HPP
#define FSIR_UPDATER_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>

namespace command
{

// Identifiers 0x01-0x1F: commands to the onboard computer; 0x20-0x3F: to the controller.
// UPLOAD BEGIN to UPLOAD CANCEL also go to the controller, with Destination 0x02.
constexpr uint8_t kUploadBegin = 0x01;    // image number (2), image length (4)
constexpr uint8_t kUploadChunk = 0x02;    // image number (2), offset (4), data (up to 256)
constexpr uint8_t kUploadEnd = 0x03;      // image number (2)
constexpr uint8_t kUploadCancel = 0x04;   // image number (2)
constexpr uint8_t kDeliver = 0x05;        // image number (2)
// The onboard computer's own dangerous commands (Interfaces Sec. 5.1 table, "Dangerous: YES").
// SET LIMITS (0x06) has a handler (owner decision 24, 15 Oct 2026), and so has SET ONBOARD TIME
// (0x09, decision 25); the others have none yet:
// after the arming check they are refused 0x06 "not built" (Updater::runOnboardDangerous).
constexpr uint8_t kSetLimits = 0x06;      // minimum version (4), controller identity (2); dangerous
constexpr uint8_t kRestore = 0x07;        // none; dangerous
constexpr uint8_t kReprogram = 0x08;      // image number (2), region (1); dangerous
constexpr uint8_t kSetOnboardTime = 0x09;   // ground time (4), unsigned seconds; dangerous
constexpr uint8_t kReplaceKey = 0x0C;     // which key (1), new key (32); dangerous
constexpr uint8_t kArm = 0x0D;            // command identifier (1), its sequence number (4)
constexpr uint8_t kSwitch = 0x20;         // none; dangerous
constexpr uint8_t kReturnToPrevious = 0x21;   // none; dangerous
constexpr uint8_t kStartFailed = 0x22;    // which copy (1); dangerous
constexpr uint8_t kLeaveFallback = 0x23;   // none; not dangerous
constexpr uint8_t kSetControllerTime = 0x24;   // ground time (4); dangerous
constexpr uint8_t kGetStatus = 0x25;      // none
// CONFIRM (TRIAL only, dangerous, REQ-012, REQ-020) is a ground command for the controller:
// the onboard computer passes it on unchanged in meaning, like SWITCH, and never sends it
// itself. The operator sends it, after ARM, once the trial tests have passed (Sec. 5.5).
constexpr uint8_t kConfirm = 0x26;        // none; dangerous

// The test image of the controller's trial tests (Interfaces Sec. 5.1): UPLOAD BEGIN to
// UPLOAD END with this image number is accepted by the controller during a trial, for the test
// image only. The controller keeps it in working memory and writes nothing into a slot.
constexpr uint16_t kTestImageNumber = 0xFFFF;

// Payload sizes: the identifier byte, then the fields above.
constexpr size_t kIdBytes = 1;
constexpr size_t kUploadBeginBytes = kIdBytes + 2 + 4;
constexpr size_t kUploadChunkHeadBytes = kIdBytes + 2 + 4;
constexpr size_t kImageNumberOnlyBytes = kIdBytes + 2;   // UPLOAD END, UPLOAD CANCEL, DELIVER
constexpr size_t kGetStatusBytes = kIdBytes;
constexpr size_t kArmBytes = kIdBytes + 1 + 4;   // ARM: command identifier (1), sequence (4)
// Controller commands (Interfaces Sec. 5.1 table, rows 0x20 to 0x26), identifier then fields.
constexpr size_t kStartFailedBytes = kIdBytes + 1;          // which copy (1)
constexpr size_t kSetControllerTimeBytes = kIdBytes + 4;    // ground time (4)
// The onboard computer's dangerous commands (Interfaces Sec. 5.1 table, rows 0x06 to 0x0C).
constexpr size_t kSetLimitsBytes = kIdBytes + 4 + 2;        // minimum version (4), identity (2)
constexpr size_t kSetLimitsVersionAt = kIdBytes;            // offset of the minimum version (4)
constexpr size_t kSetLimitsIdentityAt = kIdBytes + 4;       // offset of the controller identity (2)
constexpr size_t kRestoreBytes = kIdBytes;                  // none
constexpr size_t kReprogramBytes = kIdBytes + 2 + 1;        // image number (2), region (1)
constexpr size_t kSetOnboardTimeBytes = kIdBytes + 4;       // ground time (4)
constexpr size_t kSetOnboardTimeAt = kIdBytes;              // offset of the ground time (4)
constexpr size_t kNewKeyBytes = 32;                         // new key (32), same row
constexpr size_t kReplaceKeyBytes = kIdBytes + 1 + kNewKeyBytes;   // which key (1), new key (32)

}  // namespace command

namespace answer
{

// Answer kinds (Interfaces Sec. 4.2).
constexpr uint8_t kReceived = 0x01;
constexpr uint8_t kDone = 0x02;
constexpr uint8_t kRefused = 0x03;
constexpr uint8_t kAlreadyDone = 0x04;

// Code in DONE on success (Sec. 4.2). Sec. 4.2 gives no code for RECEIVED; 0x00 as the
// controller sends it (src/controller/firmware/main.c at commit 5c0b5f8, a placeholder
// awaiting the owner).
constexpr uint8_t kSuccess = 0x00;
constexpr uint8_t kReceivedCode = 0x00;

// Refusal reasons used here (Sec. 4.2 table).
constexpr uint8_t kNotArmed = 0x01;           // dangerous command without a valid ARM; REQ-029, REQ-112
constexpr uint8_t kArmExpired = 0x02;         // REQ-030
constexpr uint8_t kPastExpiry = 0x03;         // "Command past its expiry"; REQ-066, decision 25
constexpr uint8_t kOldCommand = 0x04;         // "Old command sent again"; REQ-065
constexpr uint8_t kNotInThisMode = 0x05;      // REQ-027
constexpr uint8_t kImpossible = 0x06;         // impossible values, or an unknown command; REQ-028
constexpr uint8_t kTrialRunning = 0x07;       // no new firmware during a trial; REQ-050
constexpr uint8_t kImageDamaged = 0x08;       // REQ-005, REQ-044
constexpr uint8_t kNotFromOperator = 0x09;    // REQ-006, REQ-043
constexpr uint8_t kOlderThanMinimum = 0x0A;   // "Image older than the minimum version"; REQ-045
constexpr uint8_t kOtherController = 0x0B;    // REQ-046
constexpr uint8_t kTrialTestsNotPassed = 0x0F;   // "Trial tests not passed"; REQ-012
constexpr uint8_t kWrongKind = 0x10;          // "Image of the wrong kind for its place"; REQ-005, REQ-008
constexpr uint8_t kBootStateNotStored = 0x11; // "Boot state could not be stored"; REQ-012, REQ-021
// "Delivery cancelled by the operator": the answer to a DELIVER that an UPLOAD CANCEL for the same
// image stopped (REQ-051). Owner decision 21, 15 Oct 2026; Interfaces Sec. 4.2 row to be added
// (the table ends at 0x11 today, so 0x12 is the next free code).
constexpr uint8_t kCancelled = 0x12;
// "The image was given up; START FAILED starts it again": the controller's refusal of a command
// for an image it gave up after a failed trial or check (FALLBACK). Owner decision 30, 15 Oct
// 2026; Interfaces Sec. 4.2 row to be added (TBR). The controller sends it; the onboard computer
// only passes it to the ground and names it. 0x13 is the next free code after 0x12.
constexpr uint8_t kImageGivenUp = 0x13;

// BENCH ONLY, not in the Interfaces table. Sec. 4.2 has no reason for "the onboard computer
// could not do it", yet a delivery must end with DONE or REFUSED (REQ-033) and the ground
// should be able to tell the causes apart. The three values lie outside the table on purpose
// (0xFD to 0xFF), so none can be read as a real reason, and each names one cause. Open item
// for the owner: add reasons to Sec. 4.2.
//   0xFF  "controller did not answer": the controller did not answer a command of a delivery
//         within its timeout, or did not answer the ARM of a ground dangerous command within the
//         answer time (decision S, accepted by the owner, 13 Oct 2026; Interfaces Sec. 5.2);
//   0xFE  the onboard computer's own storage failed (cannot open, write, flush or read back);
//   0xFD  the onboard computer could not send on data link A (device missing or failed).
constexpr uint8_t kNoAnswerBench = 0xFF;
constexpr uint8_t kStorageBench = 0xFE;
constexpr uint8_t kCannotSendBench = 0xFD;

}  // namespace answer

namespace report
{

// Report kinds and HEALTH values (Interfaces Sec. 5.4).
constexpr uint8_t kHealth = 0x01;
constexpr uint8_t kEvent = 0x02;
constexpr uint8_t kModeOperate = 0x01;
constexpr uint8_t kModeTrial = 0x02;
constexpr uint8_t kModeFallback = 0x03;

// Report payload, fields in the row order of the Sec. 5.4 table, on data link A and on the
// ground link alike (see report_store.hpp for why):
//   offset  bytes  field                                    set by
//        0      4  report number                            onboard computer (0 from the controller)
//        4      4  onboard computer clock time              onboard computer (0 from the controller)
//        8      4  controller clock time                    controller
//       12      1  report kind                              controller
//   HEALTH:
//       13      1  mode
//       14      1  running firmware kind
//       15      4  running firmware version
//       19      1  running slot: 0x01 A, 0x02 B, 0x03 last-resort
//       20      1  failed starts of the new copy
//       21      2  trial time left, in seconds
//       23      1  last self-test                           = 24 bytes
//   EVENT:
//       13      2  event code
//       15  0..32  detail                                   = 15 to 47 bytes
// The same offsets as the controller's src/controller/firmware/health-monitor/health_monitor.c
// (AT_REPORT_NUMBER to AT_LAST_SELF_TEST), which sends the first two fields as 0 (commit
// 5c0b5f8; decoded on the board by test/controller/upload_probe.py).
constexpr size_t kOffsetNumber = 0;
constexpr size_t kOffsetOnboardTime = 4;
constexpr size_t kOffsetControllerTime = 8;
constexpr size_t kOffsetKind = 12;
constexpr size_t kHealthBytes = 24;
constexpr size_t kOffsetMode = 13;
constexpr size_t kOffsetFirmwareKind = 14;
constexpr size_t kOffsetFirmwareVersion = 15;
constexpr size_t kOffsetSlot = 19;
constexpr size_t kOffsetFailedStarts = 20;
constexpr size_t kOffsetTrialLeft = 21;
constexpr size_t kOffsetSelfTest = 23;
// Event codes (Sec. 5.4 table), the ones the updater acts on or names in its log. The
// controller sends the first group; the onboard computer's own events 0x0101 and 0x0102
// (decision 27), 0x0105 (SET ONBOARD TIME, below; decision 25) and 0x0106 are sent; 0x0103 and
// 0x0104 are not sent on the bench.
// 0x0001 "fault detected" carries a 1-byte detail, the fault kind; 0x0005 "change in memory
// detected" carries a 2-byte detail, where (1) and later faults (1) (owner's decisions 2 and 3,
// 13 Oct 2026, and the second round of that day; the code lists are
// src/controller/common/fault_codes.h, copied into report_store.cpp).
constexpr uint16_t kEventFaultDetected = 0x0001;
constexpr uint16_t kEventMemoryChange = 0x0005;
// Event 0x0005's detail: where (1) and later faults (1). Bit 7 of where is set when the
// bootloader found the fault at the last start; the value is a copy of FAULT_FOUND_BY_BOOTLOADER
// (src/controller/common/fault_codes.h), which the unit test compares with the header.
constexpr size_t kMemoryChangeDetailBytes = 2;
constexpr uint8_t kFoundByBootloader = 0x80U;
// The later-faults byte stops counting at 255; a copy of BACKUP_FAULT_LATER_MAX
// (src/controller/common/backup_layout.h), which the unit test compares with the header.
constexpr uint8_t kLaterFaultsMax = 0xFFU;
constexpr uint16_t kEventTrialStarted = 0x0006;
constexpr uint16_t kEventFirmwareConfirmed = 0x0007;
constexpr uint16_t kEventFirmwareGivenUp = 0x0008;
// 0x0009 "restart" is the controller's own event (Sec. 5.4 table); the updater only names it.
// 0x0106 "controller restarted by the onboard computer" is the onboard computer's own event
// (Sec. 5.4 table, REQ-013); detail: reason (1). The updater sends it to the ground after it
// has run the restart script and the controller was reset (decision H, accepted by the owner,
// 13 Oct 2026; updater.cpp, onTrialOverrun and onRestartFinished).
constexpr uint16_t kEventControllerRestarted = 0x0106;
// 0x0105 "onboard computer clock differs from ground time" is the onboard computer's own event
// (Sec. 5.4 table, REQ-072). Detail (4): the difference, signed, little-endian, in seconds, onboard
// ground time minus the ground time of SET ONBOARD TIME. Layout: Proposal (TBR), awaiting the
// owner's acceptance; the table gives the code but no detail layout. Sent by the onboard computer
// when SET ONBOARD TIME finds a difference above config::kClockDifferenceMaxS (decision 25).
constexpr uint16_t kEventClockDiffers = 0x0105;
constexpr size_t kClockDifferenceDetailBytes = 4;
constexpr uint8_t kRestartReasonTrialPastTime = 0x01;   // Sec. 5.4 table: "0x01 trial past its time"
// Reason 0x02 "controller silent": the onboard computer reset a controller that sent no good
// frame for the silence limit (owner decision 20, 15 Oct 2026; REQ-061). Interfaces Sec. 5.4
// row 0x0106 gets this reason; Proposal (TBR), awaiting the owner's acceptance: 0x02 is the next
// free code after 0x01.
constexpr uint8_t kRestartReasonControllerSilent = 0x02;
// 0x0101 "data link lost" and 0x0102 "data link working again" (Sec. 5.4 table, REQ-081,
// REQ-082; owner decision 27, 15 Oct 2026): the onboard computer's own events about its link to
// the controller. Detail: link id (1).
constexpr uint16_t kEventDataLinkLost = 0x0101;
constexpr uint16_t kEventDataLinkWorking = 0x0102;
// Link id 0x01 is data link A, the link to the controller (owner decision 27, 15 Oct 2026).
constexpr uint8_t kLinkIdDataLinkA = 0x01;
constexpr uint16_t kEventTrialTestsPassed = 0x000A;   // REQ-139
constexpr uint16_t kEventTrialTestsFailed = 0x000B;   // REQ-139, REQ-140; detail (1): which test
// Events 0x0002 and 0x0003 carry a 2-byte detail: refused copy (1), check result (1) (Sec. 5.4
// table and the check-result table below it).
constexpr uint16_t kEventFallbackPrevious = 0x0002;
constexpr uint16_t kEventFallbackLastResort = 0x0003;
constexpr size_t kFallbackDetailBytes = 2;
constexpr size_t kOffsetEventCode = 13;
constexpr size_t kEventDetailOffset = 15;
constexpr size_t kEventMinBytes = 15;
constexpr size_t kEventMaxBytes = 15 + 32;

}  // namespace report

#endif  // FSIR_UPDATER_PROTOCOL_HPP
