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
constexpr uint8_t kArm = 0x0D;            // command identifier (1), its sequence number (4)
constexpr uint8_t kSwitch = 0x20;         // none; dangerous
constexpr uint8_t kReturnToPrevious = 0x21;   // none; dangerous
constexpr uint8_t kStartFailed = 0x22;    // which copy (1); dangerous
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
constexpr uint8_t kNotInThisMode = 0x05;      // REQ-027
constexpr uint8_t kImpossible = 0x06;         // impossible values, or an unknown command; REQ-028
constexpr uint8_t kTrialRunning = 0x07;       // no new firmware during a trial; REQ-050
constexpr uint8_t kImageDamaged = 0x08;       // REQ-005, REQ-044
constexpr uint8_t kNotFromOperator = 0x09;    // REQ-006, REQ-043
constexpr uint8_t kOtherController = 0x0B;    // REQ-046
constexpr uint8_t kTrialTestsNotPassed = 0x0F;   // "Trial tests not passed"; REQ-012

// BENCH ONLY, not in the Interfaces table: the controller did not answer a command of a
// delivery within its timeout. Sec. 4.2 has no reason for "no answer", yet DELIVER must end
// with DONE or REFUSED (REQ-033). 0xFF lies outside the table on purpose, so it cannot be
// read as a real reason. Open item for the owner: add a reason to Sec. 4.2.
constexpr uint8_t kNoAnswerBench = 0xFF;

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
// controller sends the first group; the onboard computer's own events (0x0101 to 0x0105) are
// not sent on the bench.
constexpr uint16_t kEventTrialStarted = 0x0006;
constexpr uint16_t kEventFirmwareConfirmed = 0x0007;
constexpr uint16_t kEventFirmwareGivenUp = 0x0008;
constexpr uint16_t kEventTrialTestsPassed = 0x000A;   // REQ-139
constexpr uint16_t kEventTrialTestsFailed = 0x000B;   // REQ-139, REQ-140; detail (1): which test
constexpr size_t kEventDetailOffset = 15;
constexpr size_t kEventMinBytes = 15;
constexpr size_t kEventMaxBytes = 15 + 32;

}  // namespace report

#endif  // FSIR_UPDATER_PROTOCOL_HPP
