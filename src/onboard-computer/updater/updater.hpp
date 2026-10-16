/*
 * updater.hpp — the program's wiring: takes frames from the ground link and from data link A
 * and hands each to the component that acts on it (Interfaces Sec. 4.2, Figure 4; Sec. 5.5,
 * Figure 7).
 *
 * From the ground:
 *   Destination 0x01 (onboard computer): UPLOAD BEGIN, CHUNK, END, CANCEL, DELIVER, GET STATUS.
 *     Checks on arrival: a known command, with the payload length its fields need. A command
 *     that fails them is answered REFUSED 0x06 only, as the controller does
 *     (src/controller/firmware/main.c at commit 5c0b5f8); every other command is answered
 *     RECEIVED, then DONE or
 *     REFUSED (Interfaces Sec. 4.2, REQ-033). The ARM (0x0D) and the dangerous commands 0x06,
 *     0x07, 0x08, 0x09 and 0x0C are checked against an ARM as described below. SET LIMITS (0x06)
 *     has a handler (owner decision 24, 15 Oct 2026, below) and so has SET ONBOARD TIME (0x09,
 *     owner decision 25, below); 0x07, 0x08 and 0x0C have none yet: REFUSED 0x06 after the arming
 *     check. 0x0A and 0x0B are not built: REFUSED 0x06.
 *   Expiry (owner decision 25, 15 Oct 2026; REQ-066, below) is checked first, on arrival.
 *   Repeats (owner decision 26, 15 Oct 2026; REQ-065, REQ-031): every command that passed the
 *     arrival checks is remembered by its ground sequence number and its exact bytes. The ARMs and
 *     dangerous commands are kept in UsedSequences (the last 64), all others in RepeatMemory (the
 *     last 256), so that upload traffic cannot push the dangerous ones out. The same number and
 *     bytes again: ALREADY DONE if the first one ended DONE, the first one's refusal code if it
 *     was refused, RECEIVED while it still runs; nothing is run twice. The same number with other
 *     bytes, or a number held by the other memory: REFUSED 0x04. A command refused on arrival
 *     (unknown, wrong length, image number 0xFFFF, not on the controller's list, queue full) is
 *     not remembered. The memories are lost at a restart (accepted risk, decision 26).
 *   Destination 0x02 (controller): only the controller's own commands, 0x20 to 0x26 with the
 *     payload length each needs (Interfaces Sec. 5.1 table), plus the ARM above; every other
 *     identifier, including UPLOAD BEGIN, CHUNK, END and CANCEL (0x01 to 0x04), is refused 0x06:
 *     only DELIVER produces UPLOAD commands to the controller, after the image checks (REQ-047).
 *     Passed on over data link A with the onboard computer's own
 *     sequence number; the controller's answers come back to the ground with the ground's
 *     sequence number (why: data_link.hpp). No answer from the controller in time to an ordinary
 *     command: nothing is made up for the ground, which sees an unanswered command; a late answer
 *     is still passed on. (No answer to the ARM of a dangerous command is different: see below.)
 *     While data link A is busy (a delivery), they wait in a queue of 8; when it is full,
 *     REFUSED 0x05 (bench choice: Sec. 4.2 has no reason for "busy").
 * From the controller:
 *   answers: to the command outstanding on data link A; reports: numbered, stamped and sent
 *   to the ground (report_store.hpp). Any good frame tells the controller watch it is alive.
 *
 * IMAGE LIMITS (owner decision 24, 15 Oct 2026; REQ-045, REQ-046, REQ-053): the minimum version
 *   and the controller identity are kept in the "limits" record of the data directory
 *   (limits_record.hpp) and read when the Updater is made. No record: minimum version 1, identity
 *   0x0431 (Proposal, TBR). SET LIMITS (ARM first; lowering needs nothing more) writes the record,
 *   and answers DONE only after it is written (REQ-034); a failed write is REFUSED 0xFE and the
 *   limits stay. UPLOAD END and DELIVER's check (REQ-047) refuse an image below the minimum with
 *   0x0A and one for another identity with 0x0B. A damaged limits record fails closed (TBR): both
 *   refuse every image 0x0A, and the log says why, until a SET LIMITS writes a good record.
 *
 * ONBOARD CLOCK AND EXPIRY (owner decisions 25 and 42, 15 Oct 2026; REQ-066, REQ-072, REQ-073):
 *   Onboard ground time = the system clock (seconds, Unix time) + the offset kept in the
 *   "clock-offset" record (clock_record.hpp; 0 with no record). It stamps the onboard time of
 *   every report sent to the ground and is the clock for expiry checks.
 *   - Expiry, checked first in onGroundCommand (expiryCheck), before the arrival checks, the repeat
 *     memories and the ARM: Expiry 0, or Expiry lower than the onboard ground time -> REFUSED
 *     0x03; the command is not remembered, so a resend with a valid Expiry runs. Exempt: SET
 *     ONBOARD TIME (0x09) and an ARM (0x0D) naming 0x09, both to the onboard computer, so that a
 *     wrong clock can always be corrected. The order is a Proposal (TBR), awaiting the owner's
 *     acceptance. The updater's own commands to the controller carry Expiry "never" (kNoExpiry).
 *   - SET ONBOARD TIME (ARM first, as SET LIMITS; payload 0x09, ground time (4), unsigned seconds):
 *     difference = onboard ground time - ground time; above kClockDifferenceMaxS in either
 *     direction the onboard computer sends its own EVENT 0x0105 first; then it stores offset =
 *     ground time - system clock in the record and answers DONE after the write (REQ-034). A
 *     failed write is REFUSED 0xFE and the offset in force is unchanged.
 *   - At start the record is read; a damaged or unreadable one gives offset 0 and a log line
 *     (Proposal (TBR), awaiting the owner's acceptance: fail-closed is not useful for a clock).
 *
 * AFTER A DELIVERY (owner decision 22, 15 Oct 2026; REQ-052): when the controller reports EVENT
 *   0x0007 (firmware confirmed) or 0x0008 (firmware given up), the image of the last ground
 *   DELIVER that ended DONE is deleted (image-n.img) and the deletion is logged. A .part file of
 *   an upload in progress is never deleted. An image refused at UPLOAD END is removed there.
 *
 * CANCEL OF A DELIVERY (owner decision 21, 15 Oct 2026; REQ-051): UPLOAD CANCEL for the image
 *   being delivered stops the delivery after the step in flight (stop-and-wait), sends UPLOAD
 *   CANCEL to the controller, answers the running DELIVER REFUSED 0x12 (answer::kCancelled) and
 *   then answers the UPLOAD CANCEL (DONE if the controller accepted its UPLOAD CANCEL; else the
 *   controller's code, 0xFF or 0xFD: TBR). The image's stored bytes are removed.
 *
 * DELIVER (REQ-047, REQ-048, REQ-050):
 *   1. a delivery already running: REFUSED 0x05 (bench choice, as above);
 *   2. the controller's mode, from the last HEALTH report: TRIAL -> REFUSED 0x07 (REQ-050).
 *      DELIVER is allowed in OPERATE and FALLBACK only (Interfaces Sec. 5.1); no HEALTH report
 *      in the last 5 s, or another mode -> REFUSED 0x05 (bench choice: the onboard computer
 *      cannot show that no trial runs, so it does not deliver);
 *   3. no stored image of that number -> REFUSED 0x06;
 *   4. the image checked again, just before delivery (REQ-047), against the limits -> its
 *      refusal reason; only
 *      main firmware (kind 0x02) goes through an update -> REFUSED 0x06 otherwise;
 *   5. UPLOAD BEGIN, CHUNKs, END to the controller (update_manager.hpp); DONE when the
 *      controller's END is DONE; REFUSED with the controller's code if it refuses any step;
 *      REFUSED 0xFF (bench, protocol.hpp) if it does not answer in time.
 *   The delivery's frames carry the DELIVER command's expiry: the controller (from step 2b)
 *   then stops a delivery that runs past what the operator allowed.
 *
 * ARM AND DANGEROUS COMMANDS (Interfaces Sec. 5.2; REQ-029, REQ-030, REQ-112):
 *   The controller sees the onboard computer's sequence numbers, not the ground's (data_link.hpp),
 *   so the onboard computer pairs the ground's ARM with its command.
 *   - A ground ARM (Destination 0x02 or 0x01, 0x0D, identifier of the dangerous command (1), its
 *     ground sequence number (4)) is not passed on. The identifier must be a dangerous command of
 *     the destination: the controller's (0x20, 0x21, 0x22, 0x24, 0x26, Sec. 5.1 table) or the
 *     onboard computer's (0x06, 0x07, 0x08, 0x09, 0x0C), else REFUSED 0x06. One ARM is held, for
 *     either destination; a new one replaces it. Answer: RECEIVED, then DONE.
 *   - A ground dangerous command for the onboard computer is checked in the same way (repeat,
 *     then REFUSED 0x01, then REFUSED 0x02); with a valid ARM the ARM is used up and
 *     runOnboardDangerous answers (SET LIMITS runs; the other four are REFUSED 0x06 "not built"
 *     until a handler exists). It never touches data link A.
 *   - A ground dangerous command for the controller: no held ARM naming its identifier and its
 *     ground sequence number -> REFUSED 0x01 (the ARM stays held; it allows that one command
 *     only); the ARM older than kArmExpiryMs -> REFUSED 0x02 (it is dropped). Otherwise the ARM
 *     is used up and the command joins the queue marked armed.
 *   - A dangerous command that arrives while a delivery or the test image runs, or the test image
 *     is due, is refused 0x05 (bench choice, as for a full queue) and does not use up the ARM: the
 *     pair would otherwise wait behind up to 40 KB of stop-and-wait.
 *   - The ARM's age is checked again when the pair is about to go out (sendNext, just before
 *     sendArm), against the time its ARM arrived: older than kArmExpiryMs -> REFUSED 0x02 and the
 *     command is dropped (REQ-030).
 *   - A ground sequence number that an accepted ARM or dangerous command used before (the last 64)
 *     with other payload bytes is refused 0x04 (REQ-065). With the same bytes it is a resend after
 *     a lost answer (Interfaces Sec. 4.2, REQ-031; owner, 13 Oct 2026): once the first has ended
 *     DONE it is answered ALREADY DONE and nothing else happens (no ARM held, nothing sent to the
 *     controller); while the first has not ended it is answered RECEIVED and the first one's final
 *     answer follows as usual. A first that ended REFUSED is answered REFUSED with the same
 *     code, never ALREADY DONE, which would tell the ground the command ran (Owner decided, 13
 *     Oct 2026).
 *   - When an armed command's turn comes (sendNext), the pair goes out: an ARM naming the
 *     sequence number the command will carry on data link A, then the command. The ARM's
 *     answers are the onboard computer's own and do not go to the ground; if the controller
 *     refuses the ARM, the ground's command is answered REFUSED with the controller's code and
 *     is not sent. No answer to the ARM in time: the command is not sent and the ground gets no
 *     answer, as for any command the controller leaves unanswered.
 *   - Nothing between the two: see pair_ and sendNext().
 *
 * TEST IMAGE (Interfaces Sec. 5.1, Sec. 5.5 step 7; REQ-139): when a HEALTH report shows mode
 *   TRIAL and the test image has not been sent in this trial, the onboard computer sends it
 *   once, on its own: UPLOAD BEGIN (image number 0xFFFF, 1,024 bytes), 4 UPLOAD CHUNKs, UPLOAD
 *   END, Destination 0x02, stop-and-wait. It runs through the Delivery machinery (the same
 *   steps, timeouts and answer handling as a delivery of 1,024 bytes; it needs no second
 *   implementation of stop-and-wait), started only when data link A is idle: no delivery
 *   running, nothing outstanding. Ground commands that arrive meanwhile wait in the queue,
 *   as during a delivery. The commands are the onboard computer's own, so no answer goes to
 *   the ground: DONE, the controller's refusal code or no answer is logged, and not retried.
 *   The controller's EVENT reports (trial tests passed or failed, ...) reach the ground as
 *   all reports do (test_image.hpp for the image and the once-per-trial rule). A second EVENT
 *   0x0006 within one trial sends the image again (decision R, accepted by the owner, 13 Oct 2026).
 *
 * TRIAL OVERRUN (decision H, accepted by the owner, 13 Oct 2026; Interfaces Sec. 5.4 event 0x0106;
 *   REQ-013, REQ-037, REQ-061): no EVENT 0x0007 or 0x0008 within config::kTrialMs +
 *   config::kTrialMarginMs of the trial's start (trial_watch.hpp) -> the updater runs
 *   "restore.sh reset" (restart_runner.hpp) as a child process and keeps serving both links.
 *   When the child ends: the controller was reset (script line "RESTORE DONE: reset", or "RESTORE
 *   FAILED: after reset") -> EVENT 0x0106 with detail 0x01 "trial past its time" to the ground;
 *   otherwise the reset is not shown to have happened, so nothing is sent and the log says which
 *   step failed. The script path is --restore-script, default restore.sh next to the program.
 */
#ifndef FSIR_UPDATER_UPDATER_HPP
#define FSIR_UPDATER_UPDATER_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "clock_record.hpp"
#include "command_gate.hpp"
#include "controller_watch.hpp"
#include "data_link.hpp"
#include "durable_record.hpp"
#include "frame.hpp"
#include "image_checker.hpp"
#include "limits_record.hpp"
#include "protocol.hpp"
#include "report_store.hpp"
#include "restart_runner.hpp"
#include "test_image.hpp"
#include "trial_watch.hpp"
#include "update_manager.hpp"

class Updater final : public ControllerFrameHandler
{
public:
  Updater(CommandGate & gate, DataLink & link, UploadStore & store, const ImageChecker & checker,
    ReportStore & reports, ControllerWatch & watch, uint32_t beat_ms, const char * restart_script,
    uint64_t restart_deadline_ms);

  // The ground link's socket is readable.
  void onGroundReadable(uint64_t now_ms);

  // Data link A's device is readable. False if the device failed.
  bool onLinkReadable(uint64_t listen_ms, uint64_t now_ms);

  // Timeouts, the controller watch, and the next frame for data link A. Every loop pass.
  void tick(uint64_t now_ms);

  // see ControllerFrameHandler
  void onControllerFrame(const frame::Frame & f) override;

private:
  enum class Sender : uint8_t
  {
    kNone,
    kGround,     // a ground command passed on
    kDelivery,   // a step of DELIVER
    kBeat,       // the onboard computer's own GET STATUS
    kArm,        // the ARM of an ARM/command pair, the onboard computer's own
    kDeliveryCancel,   // the UPLOAD CANCEL ending a cancelled delivery (decision 21)
  };

  // Where an ARM/command pair has got to. The queue's front is the command (armed) throughout.
  enum class PairStage : uint8_t
  {
    kNone,
    kArmOutstanding,   // the ARM is on data link A, waiting for its last answer
    kCommandNext,      // the controller accepted the ARM: the command is the next frame sent
  };

  // The ground's ARM, held until its command arrives or it expires.
  struct HeldArm
  {
    bool held;
    uint8_t identifier;           // the dangerous command it names
    uint32_t command_sequence;    // that command's ground sequence number
    uint64_t at_ms;               // when it arrived
  };

  void onGroundCommand(const frame::Frame & c);
  void onGroundArm(const frame::Frame & c);
  uint8_t armCheck(const frame::Frame & c);
  void onboardDangerous(const frame::Frame & c);
  uint8_t runOnboardDangerous(uint8_t id, const uint8_t * payload, uint16_t length);
  uint8_t runSetLimits(const uint8_t * payload, uint16_t length);   // decision 24
  uint8_t runSetOnboardTime(const uint8_t * payload, uint16_t length);   // decision 25
  void loadClockOffset();   // decision 25: reads the "clock-offset" record at start
  int64_t groundTimeS() const;   // decision 25: system clock + offset, seconds
  uint32_t groundTimeStampS() const;   // groundTimeS cut to 32 bits, for the report stamp
  uint8_t expiryCheck(const frame::Frame & c) const;   // decision 25, REQ-066
  void loadLimits();   // decision 24: reads the "limits" record at start
  bool dangerousBusy() const;
  // own: the memory this kind of command belongs to; other: the other one (decision 26).
  template <typename Own, typename Other>
  bool answerRepeat(const frame::Frame & c, const Own & own, const Other & other);
  void endDone(uint32_t sequence);   // DONE to the ground; the command is finished in the memories
  void endRefused(uint32_t sequence, uint8_t code);   // REFUSED likewise
  void finishCommand(uint32_t sequence, bool done, uint8_t refusal_code);
  void onFirmwareSettled(uint16_t code);   // decision 22
  void sendDeliveryCancel();   // decision 21
  void finishCancelledDelivery(uint8_t controller_code);   // decision 21
  void sendArm();
  void sendQueued();
  void onOnboardCommand(const frame::Frame & c);
  void onControllerAnswer(const frame::Answer & a);
  void onControllerReport(const frame::Frame & f);

  void uploadBegin(const frame::Frame & c);
  void uploadChunk(const frame::Frame & c);
  void uploadEnd(const frame::Frame & c);
  void uploadCancel(const frame::Frame & c);
  void deliver(const frame::Frame & c);
  void getStatus(const frame::Frame & c);
  uint8_t deliveryModeCheck() const;
  void finishDelivery(Delivery::Step step, uint8_t code);
  void startTestImage();
  void trialWatch(const frame::Frame * report);   // decision H, separable
  void onTrialOverrun();   // decision H
  void restartWatch();   // decision H: polls the restart script every tick
  void onRestartFinished(const RestartRunner::Outcome & outcome);   // decision H
  void silenceWatch();   // decision 20: the resets of a silent controller, every tick
  void onSilentReset();  // decision 20: one reset of a silent controller
  void sendOwnEvent(uint16_t code, uint8_t detail);   // the onboard computer's own EVENT, to the ground
  // The same with a detail of detail_length bytes (at most report::kEventMaxBytes - 15).
  void sendOwnEventDetail(uint16_t code, const uint8_t * detail, size_t detail_length);

  void sendNext();
  void onTimeout();

  void received(uint32_t sequence);
  void done(uint32_t sequence);
  void refused(uint32_t sequence, uint8_t code);

  CommandGate & gate_;
  DataLink & link_;
  UploadStore & store_;
  const ImageChecker & checker_;
  ReportStore & reports_;
  ControllerWatch & watch_;
  uint32_t beat_ms_;
  uint64_t next_beat_ms_;
  uint64_t now_ms_;
  Sender sender_;
  uint32_t reports_dropped_;
  ForwardQueue queue_;
  ForwardTable table_;
  Delivery delivery_;
  HeldArm arm_;
  PairStage pair_;
  TestImageTrigger test_trigger_;
  TrialWatch trial_watch_;   // decision H, separable
  RestartRunner restart_;    // decision H: restore.sh reset as a child process
  uint8_t restart_reason_;   // EVENT 0x0106 reason of the reset now running (protocol.hpp)
  UsedSequences used_;        // ARMs and dangerous commands, the last 64
  RepeatMemory repeats_;      // every other ground command, the last 256 (decision 26)
  DurableRecord limits_file_; // the "limits" record in the data directory (decision 24)
  DurableRecord clock_file_;  // the "clock-offset" record in the data directory (decision 25)
  int64_t clock_offset_s_;    // seconds added to the system clock to give onboard ground time
  bool cancel_requested_;     // decision 21: an UPLOAD CANCEL waits for the delivery to stop
  uint32_t cancel_sequence_;  // ground sequence number of that UPLOAD CANCEL
  bool have_delivered_;       // decision 22: a DELIVER ended DONE; its image is not yet deleted
  uint16_t delivered_number_; // the image number of that delivery
  bool test_image_running_;   // delivery_ carries the test image, not a ground DELIVER
  std::array<uint8_t, frame::kPayloadMax> payload_;
  std::array<uint8_t, report::kEventMaxBytes> report_;
};

#endif  // FSIR_UPDATER_UPDATER_HPP
