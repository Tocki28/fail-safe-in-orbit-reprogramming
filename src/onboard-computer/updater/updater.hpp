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
 *     REFUSED (Interfaces Sec. 4.2, REQ-033). Commands 0x06 to 0x0D (SET LIMITS to ARM) are
 *     not built on the bench: REFUSED 0x06.
 *   Destination 0x02 (controller): passed on over data link A with the onboard computer's own
 *     sequence number; the controller's answers come back to the ground with the ground's
 *     sequence number (why: data_link.hpp). No answer from the controller in time: nothing is
 *     made up for the ground, which sees an unanswered command; a late answer is still passed
 *     on. While data link A is busy (a delivery), they wait in a queue of 8; when it is full,
 *     REFUSED 0x05 (bench choice: Sec. 4.2 has no reason for "busy").
 * From the controller:
 *   answers: to the command outstanding on data link A; reports: numbered, stamped and sent
 *   to the ground (report_store.hpp). Any good frame tells the controller watch it is alive.
 *
 * DELIVER (REQ-047, REQ-048, REQ-050):
 *   1. a delivery already running: REFUSED 0x05 (bench choice, as above);
 *   2. the controller's mode, from the last HEALTH report: TRIAL -> REFUSED 0x07 (REQ-050).
 *      DELIVER is allowed in OPERATE and FALLBACK only (Interfaces Sec. 5.1); no HEALTH report
 *      in the last 5 s, or another mode -> REFUSED 0x05 (bench choice: the onboard computer
 *      cannot show that no trial runs, so it does not deliver);
 *   3. no stored image of that number -> REFUSED 0x06;
 *   4. the image checked again, just before delivery (REQ-047) -> its refusal reason; only
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
 *   - A ground ARM for the controller (Destination 0x02, 0x0D, identifier of the dangerous
 *     command (1), its ground sequence number (4)) is not passed on. The identifier must be a
 *     dangerous command of the controller (0x20, 0x21, 0x22, 0x24, 0x26, Sec. 5.1 table), else
 *     REFUSED 0x06. One ARM is held; a new one replaces it. Answer: RECEIVED, then DONE.
 *   - A ground dangerous command for the controller: no held ARM naming its identifier and its
 *     ground sequence number -> REFUSED 0x01 (the ARM stays held; it allows that one command
 *     only); the ARM older than kArmExpiryMs -> REFUSED 0x02 (it is dropped). Otherwise the ARM
 *     is used up and the command joins the queue marked armed.
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
 *   all reports do (test_image.hpp for the image and the once-per-trial rule).
 */
#ifndef FSIR_UPDATER_UPDATER_HPP
#define FSIR_UPDATER_UPDATER_HPP

#include <array>
#include <cstdint>

#include "command_gate.hpp"
#include "controller_watch.hpp"
#include "data_link.hpp"
#include "frame.hpp"
#include "image_checker.hpp"
#include "protocol.hpp"
#include "report_store.hpp"
#include "test_image.hpp"
#include "update_manager.hpp"

class Updater final : public ControllerFrameHandler
{
public:
  Updater(CommandGate & gate, DataLink & link, UploadStore & store, const ImageChecker & checker,
    ReportStore & reports, ControllerWatch & watch, uint32_t beat_ms);

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
  bool test_image_running_;   // delivery_ carries the test image, not a ground DELIVER
  std::array<uint8_t, frame::kPayloadMax> payload_;
  std::array<uint8_t, report::kEventMaxBytes> report_;
};

#endif  // FSIR_UPDATER_UPDATER_HPP
