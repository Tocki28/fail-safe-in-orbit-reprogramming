#include "updater.hpp"

#include <cstring>

#include "bytes.hpp"
#include "clock.hpp"
#include "config.hpp"
#include "log.hpp"
#include "test_image.hpp"

namespace
{

// The onboard computer's own commands (the beat) answer to no ground command, so they carry
// no ground expiry. Bench: the largest value, "never", as test/controller/link_probe.py
// does. Open item: what expiry the onboard computer's own commands carry once the controller
// checks expiry (REQ-133, step 2b).
constexpr uint32_t kNoExpiry = 0xFFFFFFFFU;

/**
 * @brief Tells whether a command to the onboard computer is a known one with the payload length its fields need.
 *
 * The checks on arrival for commands to the onboard computer: a command built on the bench,
 * with the payload length its fields need (Interfaces Sec. 5.1).
 *
 * @param[in] id  the command identifier, payload byte 0
 * @param[in] length  payload length of the command, in bytes
 * @return true: known command, length fits; false: otherwise
 */
bool arrivalOk(uint8_t id, uint16_t length)
{
  switch (id) {
    case command::kUploadBegin:
      return length == command::kUploadBeginBytes;
    case command::kUploadChunk:
      return length > command::kUploadChunkHeadBytes &&
             length <= command::kUploadChunkHeadBytes + config::kChunkBytes;
    case command::kUploadEnd:
    case command::kUploadCancel:
    case command::kDeliver:
      return length == command::kImageNumberOnlyBytes;
    case command::kGetStatus:
      return length == command::kGetStatusBytes;
    case command::kArm:   // an ARM for the onboard computer's own commands: not built
      return false;
  }
  // Every other identifier (0x06 to 0x0C are not built on the bench, the rest unknown).
  return false;
}

/**
 * @brief Tells whether a ground command for the controller is one of its commands, with the payload length its fields need.
 *
 * The allowed list is the controller rows of the Interfaces Sec. 5.1 table, 0x20 to 0x26, each
 * with its exact payload length. Anything else sent to Destination 0x02 is refused: the UPLOAD
 * commands (0x01 to 0x04) must never reach the controller from the ground, because they would
 * carry an image past the onboard computer's checks (REQ-047, audit findings #1 and #12);
 * 0x40 EQUIPMENT is TBD in the table and so is not allowed yet. The ARM (0x0D) is handled
 * before this test.
 *
 * @param[in] id  the command identifier, payload byte 0
 * @param[in] length  payload length of the command, in bytes
 * @return true: a controller command with the right length; false: otherwise
 */
bool controllerCommandOk(uint8_t id, uint16_t length)
{
  switch (id) {
    case command::kSwitch:
    case command::kReturnToPrevious:
    case command::kLeaveFallback:
    case command::kGetStatus:
    case command::kConfirm:
      return length == command::kIdBytes;
    case command::kStartFailed:
      return length == command::kStartFailedBytes;
    case command::kSetControllerTime:
      return length == command::kSetControllerTimeBytes;
  }
  return false;   // every other identifier: not a command the ground may send to the controller
}

/**
 * @brief Tells whether a command to the onboard computer names an image number.
 *
 * UPLOAD BEGIN, CHUNK, END, CANCEL and DELIVER carry the image number in payload bytes 1 and 2.
 *
 * @param[in] id  the command identifier, payload byte 0
 * @return true: the command carries an image number; false: it does not
 */
bool carriesImageNumber(uint8_t id)
{
  switch (id) {
    case command::kUploadBegin:
    case command::kUploadChunk:
    case command::kUploadEnd:
    case command::kUploadCancel:
    case command::kDeliver:
      return true;
    case command::kGetStatus:
    case command::kArm:
      return false;
  }
  return false;
}

/**
 * @brief Tells whether a command to the controller is one that needs an ARM.
 *
 * The "Dangerous: YES" rows for the controller in the Interfaces command table [Sec. 5.1]:
 * SWITCH, RETURN TO PREVIOUS, START FAILED, SET CONTROLLER TIME, CONFIRM. (0x40 EQUIPMENT is
 * TBD there and is not treated as dangerous.)
 *
 * @param[in]     id  the command identifier, payload byte 0
 * @return true: dangerous, needs an ARM; false: not
 */
bool isDangerous(uint8_t id)
{
  return id == command::kSwitch || id == command::kReturnToPrevious ||
         id == command::kStartFailed || id == command::kSetControllerTime ||
         id == command::kConfirm;
}

/**
 * @brief Reads the image number from an UPLOAD or DELIVER command.
 *
 * The number sits in the payload right after the identifier byte (command::kIdBytes).
 *
 * @param[in] c  the command frame
 * @return image number
 */
uint16_t imageNumber(const frame::Frame & c)
{
  return bytes::readU16(&c.payload[command::kIdBytes]);
}

}  // namespace

/**
 * @brief Wires the updater to the ground gate, data link A, the store, the checker, the report
 *        store and the controller watch.
 *
 * The test-image trigger starts with no trial seen: nothing is sent before a HEALTH report
 * shows mode TRIAL (Interfaces Sec. 5.4).
 *
 * @param[in]     gate     the ground link (answers, reports, commands)
 * @param[in]     link     data link A, one command at a time
 * @param[in]     store    stored images, for DELIVER
 * @param[in]     checker  the image checks, run again before a delivery (REQ-047)
 * @param[in]     reports  the report store, which keeps the last HEALTH report
 * @param[in]     watch    the controller watch (REQ-061)
 * @param[in]     beat_ms  period of the idle GET STATUS to the controller, in ms; 0: off
 * @param[in]     restart_script  path of restore.sh, run as "restore.sh reset" when a trial overruns
 *                                (decision H), copied; null or empty: no restart path, the overrun is only logged
 * @return nothing (constructor)
 */
Updater::Updater(CommandGate & gate, DataLink & link, UploadStore & store,
  const ImageChecker & checker, ReportStore & reports, ControllerWatch & watch, uint32_t beat_ms,
  const char * restart_script)
: gate_(gate), link_(link), store_(store), checker_(checker), reports_(reports), watch_(watch),
  beat_ms_(beat_ms), next_beat_ms_(0), now_ms_(0), sender_(Sender::kNone), reports_dropped_(0),
  queue_(), table_(), delivery_(), arm_{}, pair_(PairStage::kNone), test_trigger_(), trial_watch_(config::kTrialMs, config::kTrialMarginMs),
  restart_(restart_script, config::kRestartScriptMs, config::kRestartKillGraceMs), used_(),
  test_image_running_(false), payload_{},
  report_{}
{
}

// ---- Answers to the ground ---------------------------------------------------------------

/**
 * @brief Answers a ground command RECEIVED.
 *
 * Sent through the ground gate with answer::kReceivedCode; the result of the gate is not used.
 *
 * @param[in] sequence  the ground command's sequence number
 * @return nothing
 */
void Updater::received(uint32_t sequence)
{
  (void)gate_.answer(sequence, answer::kReceived, answer::kReceivedCode);
}

/**
 * @brief Answers a ground command DONE.
 *
 * Sent through the ground gate with answer::kSuccess; the result of the gate is not used.
 *
 * @param[in] sequence  the ground command's sequence number
 * @return nothing
 */
void Updater::done(uint32_t sequence)
{
  (void)gate_.answer(sequence, answer::kDone, answer::kSuccess);
}

/**
 * @brief Answers a ground command REFUSED with a reason.
 *
 * Sent through the ground gate; the result of the gate is not used.
 *
 * @param[in] sequence  the ground command's sequence number
 * @param[in] code  the refusal reason (protocol.hpp)
 * @return nothing
 */
void Updater::refused(uint32_t sequence, uint8_t code)
{
  (void)gate_.answer(sequence, answer::kRefused, code);
}

// ---- From the ground ---------------------------------------------------------------------

/**
 * @brief Reads one command from the ground link and handles it.
 *
 * The ground link's socket is readable. Does nothing if the gate gives no command.
 *
 * @param[in] now_ms  current time, in ms
 * @return nothing
 */
void Updater::onGroundReadable(uint64_t now_ms)
{
  now_ms_ = now_ms;
  frame::Frame c{};
  if (gate_.receive(c)) {
    onGroundCommand(c);
  }
}

/**
 * @brief Takes one ground command: onboard commands run, ARMs are held, the rest is queued
 *        for data link A.
 *
 * A dangerous command for the controller is checked against the held ARM here, when it arrives
 * [Interfaces, Sec. 5.2: "when the named command arrives"], and queued marked armed; the pair
 * itself goes out when its turn comes (sendNext). The ARM is used up only once the command is
 * queued, so a full queue (REFUSED 0x05) does not waste it. Before that: the controller's allowed
 * list (REFUSED 0x06), a ground sequence number used before by an ARM or dangerous command
 * (REFUSED 0x04, REQ-065), and a delivery or test image in the way (REFUSED 0x05, ARM kept).
 * The ARM's arrival time travels with the command so that sendNext can check its age again.
 *
 * @param[in]     c  the command frame; c.payload is valid during the call
 * @return nothing
 */
void Updater::onGroundCommand(const frame::Frame & c)
{
  if (c.destination == frame::kDestOnboard) {
    onOnboardCommand(c);
    return;
  }
  uint8_t id = (c.length > 0) ? c.payload[0] : 0;
  if (id == command::kArm) {
    onGroundArm(c);
    return;
  }
  // Only the controller's own commands go on (audit findings #1, #12): an UPLOAD command or an
  // unknown identifier or a wrong length is refused here and never reaches data link A.
  if (c.length == 0 || !controllerCommandOk(id, c.length)) {
    logging::line("ground: command 0x%02x with %u payload bytes for the controller is not on the "
      "allowed list (Sec. 5.1, 0x20 to 0x26); refused 0x06", id, c.length);
    refused(c.sequence, answer::kImpossible);
    return;
  }
  bool armed = false;
  uint64_t arm_at_ms = 0;
  if (isDangerous(id)) {
    if (used_.contains(c.sequence)) {
      logging::line("ground: dangerous command 0x%02x sequence %u: that sequence number was used "
        "before; refused 0x04", id, c.sequence);
      refused(c.sequence, answer::kOldCommand);
      return;
    }
    uint8_t code = armCheck(c);
    if (code != answer::kSuccess) {
      refused(c.sequence, code);
      return;
    }
    if (dangerousBusy()) {
      logging::line("ground: dangerous command 0x%02x sequence %u refused 0x05: a delivery or the "
        "test image runs or is due; the ARM stays held", id, c.sequence);
      refused(c.sequence, answer::kNotInThisMode);
      return;
    }
    armed = true;
    arm_at_ms = arm_.at_ms;
  }
  // For the controller: passed on in arrival order, when data link A is free (sendNext).
  if (!queue_.push(c, armed, arm_at_ms)) {
    logging::line("ground: command sequence %u for the controller refused 0x05: %zu commands "
      "already wait for data link A", c.sequence, config::kForwardQueueSize);
    refused(c.sequence, answer::kNotInThisMode);
    return;
  }
  if (armed) {
    arm_.held = false;
    used_.add(c.sequence);
  }
  sendNext();
}

/**
 * @brief Tells whether data link A is taken by something a dangerous command must not wait behind.
 *
 * A delivery (up to 40 KB in 256-byte steps) or the test image, running or due, would keep an
 * armed command in the queue for far longer than the ARM's validity; the command is refused
 * instead of queued (REQ-030, REQ-112; audit finding #1).
 *
 * @return true: a delivery or the test image runs, or the test image is due; false: link A is free of them
 */
bool Updater::dangerousBusy() const
{
  return delivery_.active() || test_trigger_.due();
}

/**
 * @brief Takes a ground ARM for the controller: checks it, holds it, answers RECEIVED and DONE.
 *
 * The ARM is not passed on: the controller would see a sequence number that is the ground's,
 * not the one its dangerous command will carry on data link A [Interfaces, Sec. 5.2]. One ARM
 * is held; a new one replaces the old.
 *
 * @param[in]     c  the ARM frame (Destination 0x02, payload 0x0D, identifier (1), sequence (4))
 * @return nothing
 */
void Updater::onGroundArm(const frame::Frame & c)
{
  if (c.length != command::kArmBytes || !isDangerous(c.payload[1])) {
    logging::line("ground: ARM sequence %u names 0x%02x or has %u payload bytes: not a dangerous "
      "command of the controller; refused 0x06", c.sequence,
      (c.length > 1) ? c.payload[1] : 0, c.length);
    refused(c.sequence, answer::kImpossible);
    return;
  }
  if (used_.contains(c.sequence)) {
    logging::line("ground: ARM sequence %u: that sequence number was used before; refused 0x04",
      c.sequence);
    refused(c.sequence, answer::kOldCommand);
    return;
  }
  used_.add(c.sequence);
  arm_.held = true;
  arm_.identifier = c.payload[1];
  arm_.command_sequence = bytes::readU32(&c.payload[2]);
  arm_.at_ms = now_ms_;
  logging::line("ground: ARM sequence %u held for command 0x%02x, ground sequence %u (valid "
    "%llu ms)", c.sequence, arm_.identifier, arm_.command_sequence,
    static_cast<unsigned long long>(config::kArmExpiryMs));
  received(c.sequence);
  done(c.sequence);
}

/**
 * @brief Checks a ground dangerous command against the held ARM; does not use the ARM up.
 *
 * REFUSED 0x01 when no held ARM names this command's identifier and ground sequence number
 * (REQ-029, REQ-112); REFUSED 0x02 when the held ARM is older than kArmExpiryMs (REQ-030), and
 * that ARM is dropped. A non-matching ARM stays held: it allows its own command only
 * [Interfaces, Sec. 5.2].
 *
 * @param[in]     c  the dangerous command frame
 * @return answer::kSuccess if armed; else the refusal reason (0x01 or 0x02)
 */
uint8_t Updater::armCheck(const frame::Frame & c)
{
  uint8_t id = c.payload[0];
  if (!arm_.held || arm_.identifier != id || arm_.command_sequence != c.sequence) {
    logging::line("ground: dangerous command 0x%02x sequence %u without an ARM that names it; "
      "refused 0x01", id, c.sequence);
    return answer::kNotArmed;
  }
  if (now_ms_ - arm_.at_ms > config::kArmExpiryMs) {
    logging::line("ground: ARM for command 0x%02x sequence %u is %llu ms old, past %llu ms; "
      "refused 0x02", id, c.sequence, static_cast<unsigned long long>(now_ms_ - arm_.at_ms),
      static_cast<unsigned long long>(config::kArmExpiryMs));
    arm_.held = false;
    return answer::kArmExpired;
  }
  return answer::kSuccess;
}

/**
 * @brief Runs a ground command addressed to the onboard computer.
 *
 * A command that fails the arrival checks (arrivalOk) is answered REFUSED 0x06 only. Any other is
 * answered RECEIVED, then run, and ends in DONE or REFUSED (Interfaces Sec. 4.2, REQ-033). An
 * image number 0xFFFF (the test image's) is refused 0x06 on arrival.
 *
 * @param[in] c  the command frame
 * @return nothing
 */
void Updater::onOnboardCommand(const frame::Frame & c)
{
  uint8_t id = (c.length > 0) ? c.payload[0] : 0;
  if (!arrivalOk(id, c.length)) {
    logging::line("ground: command 0x%02x with %u payload bytes is not one the onboard "
      "computer runs; refused 0x06", id, c.length);
    refused(c.sequence, answer::kImpossible);
    return;
  }
  // Image number 0xFFFF is the controller's test image, which only the onboard computer sends
  // itself (Sec. 5.1); from the ground it is an impossible value (audit finding #11).
  if (carriesImageNumber(id) && imageNumber(c) == command::kTestImageNumber) {
    logging::line("ground: command 0x%02x names image number 0xFFFF, the test image's; refused "
      "0x06", id);
    refused(c.sequence, answer::kImpossible);
    return;
  }
  received(c.sequence);
  switch (id) {
    case command::kUploadBegin:   uploadBegin(c);  return;
    case command::kUploadChunk:   uploadChunk(c);  return;
    case command::kUploadEnd:     uploadEnd(c);    return;
    case command::kUploadCancel:  uploadCancel(c); return;
    case command::kDeliver:       deliver(c);      return;
    case command::kGetStatus:     getStatus(c);    return;
    case command::kArm:           break;
  }
  refused(c.sequence, answer::kImpossible);   // not reached: arrivalOk lets only the six above in
}

/**
 * @brief Runs UPLOAD BEGIN: opens the upload in the store.
 *
 * Answers DONE, or REFUSED with the store's reason. Logs whether it starts or resumes.
 *
 * @param[in] c  the UPLOAD BEGIN command frame
 * @return nothing
 */
void Updater::uploadBegin(const frame::Frame & c)
{
  uint16_t number = imageNumber(c);
  uint32_t length = bytes::readU32(&c.payload[3]);
  uint32_t stored = 0;
  uint8_t code = store_.begin(number, length, stored);
  if (code != answer::kSuccess) {
    logging::line("UPLOAD BEGIN image %u, %u bytes: refused 0x%02x", number, length, code);
    refused(c.sequence, code);
    return;
  }
  logging::line("UPLOAD BEGIN image %u, %u bytes: %s at byte %u", number, length,
    (stored > 0) ? "resuming" : "starting", stored);
  done(c.sequence);
}

/**
 * @brief Runs UPLOAD CHUNK: gives the chunk to the store.
 *
 * Answers DONE, or REFUSED with the store's reason. A chunk already stored is logged and answered DONE.
 *
 * @param[in] c  the UPLOAD CHUNK command frame
 * @return nothing
 */
void Updater::uploadChunk(const frame::Frame & c)
{
  uint16_t number = imageNumber(c);
  uint32_t offset = bytes::readU32(&c.payload[3]);
  size_t size = c.length - command::kUploadChunkHeadBytes;
  bool written = false;
  uint8_t code = store_.chunk(number, offset, &c.payload[command::kUploadChunkHeadBytes], size,
    written);
  if (code != answer::kSuccess) {
    logging::line("UPLOAD CHUNK image %u at %u (%zu bytes): refused 0x%02x; %u bytes stored",
      number, offset, size, code, store_.stored());
    refused(c.sequence, code);
    return;
  }
  if (!written) {
    logging::line("UPLOAD CHUNK image %u at %u: already stored", number, offset);
  }
  done(c.sequence);
}

/**
 * @brief Runs UPLOAD END: the store checks the whole image and keeps it.
 *
 * Answers DONE, or REFUSED with the reason of the failed check.
 *
 * @param[in] c  the UPLOAD END command frame
 * @return nothing
 */
void Updater::uploadEnd(const frame::Frame & c)
{
  uint16_t number = imageNumber(c);
  ImageVerdict verdict{};
  uint8_t code = store_.end(number, checker_, verdict);
  if (code != answer::kSuccess) {
    logging::line("UPLOAD END image %u: refused 0x%02x (%s)", number, code, verdict.reason);
    refused(c.sequence, code);
    return;
  }
  logging::line("UPLOAD END image %u: checks passed (kind 0x%02x, version %u); stored", number,
    verdict.kind, verdict.version);
  done(c.sequence);
}

/**
 * @brief Runs UPLOAD CANCEL: removes the stored bytes of the image.
 *
 * Answers DONE.
 *
 * @param[in] c  the UPLOAD CANCEL command frame
 * @return nothing
 */
void Updater::uploadCancel(const frame::Frame & c)
{
  uint16_t number = imageNumber(c);
  (void)store_.cancel(number);
  logging::line("UPLOAD CANCEL image %u: stored bytes removed", number);
  done(c.sequence);
}

/**
 * @brief Checks the controller's mode, from the last HEALTH report, for a delivery.
 *
 * DELIVER is allowed in OPERATE and FALLBACK only (Interfaces Sec. 5.1). Mode TRIAL gives
 * answer::kTrialRunning (REQ-050). No HEALTH report, one older than config::kSilenceMs, or another
 * mode gives answer::kNotInThisMode.
 *
 * @return answer::kSuccess if a delivery may start; else the refusal reason
 */
uint8_t Updater::deliveryModeCheck() const
{
  if (!reports_.haveHealth() || now_ms_ - reports_.lastHealthMs() > config::kSilenceMs) {
    return answer::kNotInThisMode;
  }
  uint8_t mode = reports_.lastHealth().mode;
  if (mode == report::kModeTrial) {
    return answer::kTrialRunning;
  }
  if (mode != report::kModeOperate && mode != report::kModeFallback) {
    return answer::kNotInThisMode;
  }
  return answer::kSuccess;
}

/**
 * @brief Runs DELIVER: checks that a delivery may start, loads and checks the image, and starts it.
 *
 * The steps are those listed under DELIVER in updater.hpp (REQ-047, REQ-048, REQ-050): no delivery
 * running, controller mode, a stored image, the check before delivery, main firmware only. A failed
 * step answers REFUSED with its reason; otherwise the delivery starts and the answer comes at its end.
 *
 * @param[in] c  the DELIVER command frame
 * @return nothing
 */
void Updater::deliver(const frame::Frame & c)
{
  uint16_t number = imageNumber(c);
  if (delivery_.active()) {
    logging::line("DELIVER image %u: refused 0x05: the delivery of image %u still runs", number,
      delivery_.number());
    refused(c.sequence, answer::kNotInThisMode);
    return;
  }
  uint8_t code = deliveryModeCheck();
  if (code != answer::kSuccess) {
    if (!reports_.haveHealth()) {
      logging::line("DELIVER image %u: refused 0x%02x: controller mode not known (no HEALTH "
        "report yet)", number, code);
    } else if (now_ms_ - reports_.lastHealthMs() > config::kSilenceMs) {
      logging::line("DELIVER image %u: refused 0x%02x: controller mode not known (no HEALTH "
        "report for %llu ms; the last said %s)", number, code,
        static_cast<unsigned long long>(now_ms_ - reports_.lastHealthMs()),
        modeName(reports_.lastHealth().mode));
    } else {
      logging::line("DELIVER image %u: refused 0x%02x: controller mode %s", number, code,
        modeName(reports_.lastHealth().mode));
    }
    refused(c.sequence, code);
    return;
  }
  size_t size = 0;
  if (!store_.load(number, delivery_.buffer(), delivery_.capacity(), size)) {
    logging::line("DELIVER image %u: refused 0x06: no checked image of that number", number);
    refused(c.sequence, answer::kImpossible);
    return;
  }
  // REQ-047: checked again after its last transfer (here: from storage), just before delivery.
  ImageVerdict verdict = checker_.check(delivery_.buffer(), size);
  if (verdict.code != answer::kSuccess) {
    logging::line("DELIVER image %u: refused 0x%02x: check before delivery failed (%s)", number,
      verdict.code, verdict.reason);
    refused(c.sequence, verdict.code);
    return;
  }
  if (verdict.kind != IMAGE_KIND_MAIN) {
    logging::line("DELIVER image %u: refused 0x06: kind 0x%02x; only main firmware is delivered",
      number, verdict.kind);
    refused(c.sequence, answer::kImpossible);
    return;
  }
  logging::line("DELIVER image %u: %zu bytes, version %u, to the controller", number, size,
    verdict.version);
  delivery_.start(number, size, c.sequence, c.expiry);
  sendNext();
}

/**
 * @brief Runs GET STATUS: logs the state of the upload, the delivery, the controller and data link A.
 *
 * Answers DONE.
 *
 * @param[in] c  the GET STATUS command frame
 * @return nothing
 */
void Updater::getStatus(const frame::Frame & c)
{
  logging::line("status: upload %s (%u bytes stored); delivery %s; controller %s, last HEALTH "
    "mode %s; data link A frames sent %u, received %u; last report number %u",
    store_.open() ? "open" : "none", store_.stored(), delivery_.active() ? "running" : "idle",
    watch_.silent() ? "silent" : "heard",
    reports_.haveHealth() ? modeName(reports_.lastHealth().mode) : "none",
    link_.sentCount(), link_.receivedCount(), reports_.lastNumber());
  done(c.sequence);
}

/**
 * @brief Ends the running delivery, or the test image, and tells whoever is owed the result.
 *
 * A ground DELIVER is answered DONE or REFUSED (REQ-033). The test image is the onboard
 * computer's own command (Interfaces Sec. 5.1), so nobody on the ground asked and no answer
 * goes down: the result is logged, and the controller's EVENT reports tell the ground the rest.
 *
 * @param[in]     step  kDone or kRefused (Delivery::onFinalAnswer, or a timeout)
 * @param[in]     code  the refusal reason when step is kRefused (protocol.hpp; 0xFF: no answer, 0xFD: cannot send)
 * @return nothing
 */
void Updater::finishDelivery(Delivery::Step step, uint8_t code)
{
  if (test_image_running_) {
    if (step == Delivery::Step::kDone) {
      logging::line("test image: done; the controller accepted UPLOAD END");
    } else if (code == answer::kNoAnswerBench) {
      logging::line("test image: no answer from the controller after %zu of %zu bytes; not "
        "retried", delivery_.sent(), delivery_.size());
    } else if (code == answer::kCannotSendBench) {
      logging::line("test image: data link A cannot send, after %zu of %zu bytes; not retried",
        delivery_.sent(), delivery_.size());
    } else {
      logging::line("test image: refused 0x%02x (%s) after %zu of %zu bytes; not retried", code,
        refusalName(code), delivery_.sent(), delivery_.size());
    }
    test_image_running_ = false;
  } else if (step == Delivery::Step::kDone) {
    logging::line("DELIVER image %u: done; the controller accepted UPLOAD END",
      delivery_.number());
    done(delivery_.groundSequence());
  } else {
    logging::line("DELIVER image %u: refused 0x%02x (%s) after %zu of %zu bytes",
      delivery_.number(), code, refusalName(code), delivery_.sent(), delivery_.size());
    refused(delivery_.groundSequence(), code);
  }
  delivery_.stop();
}

/**
 * @brief Starts sending the test image to the controller through the Delivery machinery.
 *
 * Delivery already does UPLOAD BEGIN, CHUNKs of 256 bytes and UPLOAD END with stop-and-wait,
 * the answer rules and a bounded wait per step, so the test image reuses it: 1,024 bytes, image
 * number 0xFFFF [Interfaces, Sec. 5.1, Sec. 5.5]. Only called when no delivery runs and data
 * link A is idle. The frames carry no expiry (kNoExpiry, as the beat does) because no ground
 * command is behind them. UPLOAD BEGIN waits up to kBeginTimeoutMs, which is the slot-erase
 * time and longer than the controller needs here (it writes no slot, Sec. 5.1): a longer
 * bound only costs time when the controller is silent, and it is still bounded.
 *
 * @return nothing
 */
void Updater::startTestImage()
{
  std::array<uint8_t, test_image::kBytes> image{};
  test_image::build(image);
  std::memcpy(delivery_.buffer(), image.data(), image.size());
  delivery_.start(command::kTestImageNumber, image.size(), 0, kNoExpiry);
  test_image_running_ = true;
  test_trigger_.markStarted();
  logging::line("test image: %zu bytes, image number 0x%04x, to the controller (trial running)",
    image.size(), command::kTestImageNumber);
}

// ---- From the controller -----------------------------------------------------------------

/**
 * @brief Reads data link A and hands each frame from the controller to this updater.
 *
 * Data link A's device is readable. False if the device failed.
 *
 * @param[in] listen_ms  how long the link may wait for data, in ms
 * @param[in] now_ms  current time, in ms
 * @return true: the device is fine; false: the device failed
 */
bool Updater::onLinkReadable(uint64_t listen_ms, uint64_t now_ms)
{
  now_ms_ = now_ms;
  return link_.receive(*this, listen_ms);
}

/**
 * @brief Takes one good frame from the controller: tells the watch, then passes answers and reports on.
 *
 * The controller sends to Destination 0x01 (its main.c, commit 5c0b5f8); 0x03 is accepted too, since the
 * Interfaces document leaves that choice open. 0x02 would be a frame for the controller,
 * e.g. our own echoed back.
 * Any good frame tells the controller watch it is alive. A frame addressed to the controller, an answer
 * of other than 6 bytes, and a frame of another type are dropped and logged.
 *
 * @param[in] f  the frame received from the controller
 * @return nothing
 */
void Updater::onControllerFrame(const frame::Frame & f)
{
  watch_.heard(now_ms_);
  if (logging::verbose()) {
    logging::line("data link A: received type 0x%02x to 0x%02x, sequence %u, %u payload bytes",
      f.type, f.destination, f.sequence, f.length);
  }
  // The controller sends to Destination 0x01 (its main.c, commit 5c0b5f8); 0x03 is accepted too, since the
  // Interfaces document leaves that choice open. 0x02 would be a frame for the controller,
  // e.g. our own echoed back.
  if (f.destination == frame::kDestController) {
    logging::line("data link A: frame for the controller came back; dropped");
    return;
  }
  if (f.type == frame::kTypeAnswer) {
    frame::Answer a{};
    if (!frame::decodeAnswer(f, a)) {
      logging::line("data link A: answer of %u bytes, not 6; dropped", f.length);
      return;
    }
    onControllerAnswer(a);
  } else if (f.type == frame::kTypeReport) {
    onControllerReport(f);
  } else {
    logging::line("data link A: frame type 0x%02x from the controller; dropped", f.type);
  }
}

/**
 * @brief Takes one answer from the controller and gives it to whoever sent the command.
 *
 * A ground command's answers go to the ground with the ground's sequence number; a delivery's
 * and the test image's drive their next step; the beat's are not used. The ARM of an ARM/command
 * pair is the onboard computer's own, so its answers do not go to the ground: DONE lets the
 * command go next, REFUSED refuses the ground's command with the controller's code and drops it
 * [Interfaces, Sec. 5.2]. A late answer to a ground command is still passed on.
 *
 * @param[in]     a  the answer: sequence number answered, kind, code
 * @return nothing
 */
void Updater::onControllerAnswer(const frame::Answer & a)
{
  bool final = frame::isFinal(a.kind);
  uint32_t ground_sequence = 0;

  if (!link_.busy() || a.answered != link_.outstanding()) {
    // Not the outstanding command: a late answer to a ground command still goes to the ground.
    if (table_.find(a.answered, ground_sequence)) {
      logging::line("controller: late answer 0x%02x code 0x%02x to sequence %u; passed on as "
        "ground sequence %u", a.kind, a.code, a.answered, ground_sequence);
      (void)gate_.answer(ground_sequence, a.kind, a.code);
      if (final) {
        table_.remove(a.answered);
      }
    } else {
      logging::line("controller: answer 0x%02x to sequence %u matches no command; dropped",
        a.kind, a.answered);
    }
    return;
  }

  switch (sender_) {
    case Sender::kGround:
      if (table_.find(a.answered, ground_sequence)) {
        (void)gate_.answer(ground_sequence, a.kind, a.code);
        if (final) {
          logging::line("controller: answer 0x%02x code 0x%02x to ground sequence %u", a.kind,
            a.code, ground_sequence);
          table_.remove(a.answered);
        }
      }
      break;
    case Sender::kDelivery:
      if (final) {
        uint8_t code = answer::kSuccess;
        Delivery::Step step = delivery_.onFinalAnswer(a.kind, a.code, code);
        if (step != Delivery::Step::kContinue) {
          finishDelivery(step, code);
        }
      }
      break;
    case Sender::kArm:
      if (final) {
        if (a.kind == answer::kRefused || a.kind == answer::kAlreadyDone) {
          // Do not send the command; the ground's dangerous command is refused with the
          // controller's code. ALREADY DONE to an ARM can only mean a sequence-number clash
          // (the updater sends each ARM once), so it counts as a refusal, 0x04 (REQ-065).
          uint8_t code = (a.kind == answer::kRefused) ? a.code : answer::kOldCommand;
          logging::line("controller: ARM sequence %u answered 0x%02x code 0x%02x; ground command "
            "sequence %u refused 0x%02x and not sent", a.answered, a.kind, a.code,
            queue_.front().ground_sequence, code);
          refused(queue_.front().ground_sequence, code);
          queue_.pop();
          pair_ = PairStage::kNone;
        } else {
          pair_ = PairStage::kCommandNext;
        }
      }
      break;
    case Sender::kBeat:
    case Sender::kNone:
      break;
  }
  if (final) {
    link_.release();
    sender_ = Sender::kNone;
  }
}

/**
 * @brief Takes one report from the controller: numbers and stamps it, sends it to the ground,
 *        and, for a HEALTH report, lets the test-image rule see the mode.
 *
 * The test image is due when a HEALTH report shows mode TRIAL, once per trial (test_image.hpp).
 * Only a report that passed the Sec. 5.4 checks counts, so a damaged one cannot start or end a
 * trial. The mode is read before the ground check below: the trial happens whether or not a
 * ground has spoken yet. A restarted trial (EVENT 0x0006) makes the image due again, and the
 * trial events and modes feed the trial watch (trial_watch.hpp).
 *
 * @param[in]     f  the report frame from data link A; f.payload is valid during the call
 * @return nothing
 */
void Updater::onControllerReport(const frame::Frame & f)
{
  if (!reports_.accept(f.payload, f.length, clocks::onboardTimeS(), now_ms_, report_.data(),
        report_.size())) {
    return;
  }
  if (f.payload[report::kOffsetKind] == report::kHealth) {
    test_trigger_.onHealthMode(f.payload[report::kOffsetMode]);
  } else if (f.payload[report::kOffsetKind] == report::kEvent && f.length >= report::kEventMinBytes) {
    uint16_t code = bytes::readU16(&f.payload[report::kOffsetEventCode]);
    if (code == report::kEventTrialStarted) {
      test_trigger_.onTrialStartedEvent();
    }
  }
  trialWatch(&f);   // decision H, separable: see trialWatch
  if (!gate_.haveGround()) {
    if (reports_dropped_ == 0) {
      logging::line("reports: no ground has spoken yet; reports are dropped until one does");
    }
    reports_dropped_++;
    return;
  }
  (void)gate_.sendReport(report_.data(), f.length);
}

// ---- Data link A: one command at a time ----------------------------------------------------

/**
 * @brief Sends the front ground command of the queue on data link A, then removes it.
 *
 * Only when data link A is idle. The controller's answers go to the ground with the ground's
 * sequence number (ForwardTable). For an armed command this is the second frame of its pair.
 *
 * @return nothing
 */
void Updater::sendQueued()
{
  uint32_t sequence = 0;
  const ForwardedCommand & c = queue_.front();
  uint8_t id = (c.length > 0) ? c.payload[0] : 0;
  uint32_t timeout_ms = (id == command::kUploadBegin) ? config::kBeginTimeoutMs : config::kAnswerTimeoutMs;
  if (link_.sendCommand(c.payload.data(), c.length, c.expiry, timeout_ms, now_ms_, sequence)) {
    table_.add(sequence, c.ground_sequence);
    sender_ = Sender::kGround;
    logging::line("ground command 0x%02x sequence %u passed on to the controller as "
      "sequence %u", id, c.ground_sequence, sequence);
  } else {
    // Refused rather than dropped in silence (audit finding #13): the ground then knows the
    // command did not go. Bench code 0xFD "cannot send" (protocol.hpp) is used, not 0x05: the
    // cause is the missing link, not the controller's mode, and a ground that read 0x05 would
    // look for a mode problem.
    logging::line("ground command 0x%02x sequence %u not passed on: data link A cannot send; "
      "refused 0xfd", id, c.ground_sequence);
    refused(c.ground_sequence, answer::kCannotSendBench);
  }
  queue_.pop();
}

/**
 * @brief Sends the ARM of the front (armed) command: the first frame of the pair.
 *
 * The ARM names the command by identifier and by the sequence number the command will carry on
 * data link A: the one after the ARM's own, since the link has one counter (data_link.hpp). The
 * command stays at the front of the queue until its own turn (pair_ kCommandNext) or its
 * refusal. The ARM's expiry is the ground command's.
 *
 * @return nothing
 */
void Updater::sendArm()
{
  const ForwardedCommand & c = queue_.front();
  uint32_t arm_sequence = link_.nextSequence();
  uint32_t command_sequence = (arm_sequence == UINT32_MAX) ? 1U : arm_sequence + 1U;
  std::array<uint8_t, command::kArmBytes> arm{};
  arm[0] = command::kArm;
  arm[1] = c.payload[0];
  bytes::writeU32(&arm[2], command_sequence);
  uint32_t sequence = 0;
  if (link_.sendCommand(arm.data(), static_cast<uint16_t>(arm.size()), c.expiry,
        config::kAnswerTimeoutMs, now_ms_, sequence)) {
    sender_ = Sender::kArm;
    pair_ = PairStage::kArmOutstanding;
    logging::line("ARM sequence %u sent for ground command 0x%02x sequence %u, which will carry "
      "sequence %u", sequence, c.payload[0], c.ground_sequence, command_sequence);
  } else {
    logging::line("ARM for ground command 0x%02x sequence %u not sent: data link A cannot "
      "send; refused 0xfd", c.payload[0], c.ground_sequence);
    refused(c.ground_sequence, answer::kCannotSendBench);
    queue_.pop();
  }
}

/**
 * @brief Sends the next command on data link A, if the link is idle.
 *
 * Order: the second frame of an ARM/command pair first, then the delivery, the test image, the
 * waiting ground commands, the beat. The pair has nothing between its two frames because (a)
 * stop-and-wait: the command is sent as soon as the ARM's last answer has freed the link, and
 * (b) that check comes first here, before the delivery, the test image, the queue and the beat,
 * which are the only other places that send; and the ARM is sent only from the queue's front,
 * which stays there until the command is sent [Interfaces, Sec. 5.2: "with no frame between
 * them"]. A delivery's steps must not be split by other commands to the controller, e.g. a
 * SWITCH while a slot is half written, and the ARM/command pair is not started while a delivery
 * runs. The test image starts here too, and only here: data link A idle and no delivery
 * running, so it never splits a delivery or a pair and never overtakes a command already
 * sent. Once started it is a delivery, so the ground commands queued meanwhile wait for it as
 * they wait for any delivery [Interfaces, Sec. 5.1: during a trial the controller accepts
 * UPLOAD commands for the test image only, so its steps are kept together as a delivery's are].
 *
 * @return nothing
 */
void Updater::sendNext()
{
  if (link_.busy()) {
    return;
  }
  uint32_t sequence = 0;
  uint32_t timeout_ms = config::kAnswerTimeoutMs;

  if (pair_ == PairStage::kCommandNext) {
    pair_ = PairStage::kNone;
    sendQueued();
    return;
  }

  if (!delivery_.active() && test_trigger_.due()) {
    startTestImage();
  }

  if (delivery_.active()) {
    uint16_t length = delivery_.next(payload_.data(), payload_.size(), timeout_ms);
    if (length == 0 || !link_.sendCommand(payload_.data(), length, delivery_.expiry(),
                         timeout_ms, now_ms_, sequence)) {
      logging::line("DELIVER image %u: data link A cannot send", delivery_.number());
      finishDelivery(Delivery::Step::kRefused, answer::kCannotSendBench);
      return;
    }
    sender_ = Sender::kDelivery;
    return;
  }

  // Bounded by the queue's size: each pass sends, or removes an expired ARM pair and goes on.
  for (size_t pass = 0; pass < config::kForwardQueueSize && !queue_.empty(); pass++) {
    const ForwardedCommand & front = queue_.front();
    if (front.armed && now_ms_ - front.arm_at_ms > config::kArmExpiryMs) {
      // The ARM was valid on arrival but waited in the queue too long: REQ-030 holds at the
      // moment the ARM goes to the controller, not only at arrival (audit finding #1).
      logging::line("ground: ARM for command 0x%02x sequence %u is %llu ms old, past %llu ms, "
        "when its turn came; refused 0x02 and dropped", front.payload[0], front.ground_sequence,
        static_cast<unsigned long long>(now_ms_ - front.arm_at_ms),
        static_cast<unsigned long long>(config::kArmExpiryMs));
      refused(front.ground_sequence, answer::kArmExpired);
      queue_.pop();
      continue;
    }
    if (front.armed) {
      sendArm();
    } else {
      sendQueued();
    }
    return;
  }

  if (beat_ms_ > 0 && now_ms_ >= next_beat_ms_) {
    next_beat_ms_ = now_ms_ + beat_ms_;
    const uint8_t get_status = command::kGetStatus;
    if (link_.sendCommand(&get_status, 1, kNoExpiry, config::kAnswerTimeoutMs, now_ms_,
          sequence)) {
      sender_ = Sender::kBeat;
    }
  }
}

/**
 * @brief Handles the timeout of the command outstanding on data link A (the link is released).
 *
 * Every wait is bounded. Ground command: the ground gets no answer, a late one is still passed
 * on. Delivery or test image: ends with 0xFF (protocol.hpp). ARM of a pair: the command is not
 * sent and the ground's command is answered REFUSED 0xFF "controller did not answer" (decision S,
 * accepted by the owner, 13 Oct 2026; Interfaces Sec. 5.2). Beat: logged.
 *
 * @return nothing
 */
void Updater::onTimeout()
{
  uint32_t sequence = link_.outstanding();
  switch (sender_) {
    case Sender::kGround:
      logging::line("controller: no last answer to sequence %u in time; the ground gets none "
        "(a late one is still passed on)", sequence);
      break;
    case Sender::kDelivery:
      logging::line("controller: no last answer to sequence %u (delivery) in time", sequence);
      finishDelivery(Delivery::Step::kRefused, answer::kNoAnswerBench);
      break;
    case Sender::kArm:
      // Decision S (accepted by the owner, 13 Oct 2026): the operator who sent a dangerous
      // command is told it did not go, with the bench code 0xFF "controller did not answer".
      logging::line("controller: no answer to ARM sequence %u in time; the ground command sequence "
        "%u is not sent; refused 0xff (controller did not answer)", sequence,
        queue_.front().ground_sequence);
      refused(queue_.front().ground_sequence, answer::kNoAnswerBench);
      queue_.pop();
      pair_ = PairStage::kNone;
      break;
    case Sender::kBeat:
      logging::line("controller: no answer to GET STATUS sequence %u (beat) in time", sequence);
      break;
    case Sender::kNone:
      break;
  }
  sender_ = Sender::kNone;
}

/**
 * @brief The one wiring point of the trial-overrun watch (decision H, accepted by the owner, 13 Oct 2026).
 *
 * Called with a good report from the controller (the HEALTH mode or the EVENT code is fed to the
 * TrialWatch) and, with a null pointer, on every tick (the clock is checked). To drop decision H,
 * delete this function, its two calls, onTrialOverrun, restartWatch, onRestartFinished, the members
 * trial_watch_ and restart_, trial_watch.* and restart_runner.*; nothing else uses them.
 *
 * @param[in]     report  a report frame that passed the report store's checks, or nullptr on a tick
 * @return nothing
 */
void Updater::trialWatch(const frame::Frame * report)
{
  if (report == nullptr) {
    if (trial_watch_.expired(now_ms_)) {
      onTrialOverrun();
    }
    return;
  }
  if (report->payload[report::kOffsetKind] == report::kHealth) {
    trial_watch_.onHealthMode(report->payload[report::kOffsetMode], now_ms_);
  } else if (report->payload[report::kOffsetKind] == report::kEvent &&
             report->length >= report::kEventMinBytes) {
    trial_watch_.onEvent(bytes::readU16(&report->payload[report::kOffsetEventCode]), now_ms_);
  }
}

/**
 * @brief Acts on a trial that ran past its time plus margin: starts the restart script.
 *
 * Decision H (accepted by the owner, 13 Oct 2026): the firmware on trial enforces the trial time,
 * so the onboard computer times it too, and when that limit did not work it restarts the
 * controller (REQ-013, REQ-061) by running "restore.sh reset" (restart_runner.hpp). The script
 * runs as a child process under a deadline; nothing here waits for it (restartWatch picks up its
 * end). The EVENT 0x0106 goes to the ground only once the reset has happened
 * (onRestartFinished). A script that cannot be started, or one that is already running, is logged
 * and nothing is sent: no reset happened, and the ground is not told one did.
 *
 * @return nothing
 */
void Updater::onTrialOverrun()
{
  logging::line("trial: no EVENT 0x0007 or 0x0008 within %llu s + %llu s margin of the trial's "
    "start (REQ-037); restarting the controller with the restart script",
    static_cast<unsigned long long>(config::kTrialMs / 1000),
    static_cast<unsigned long long>(config::kTrialMarginMs / 1000));
  if (!restart_.configured()) {
    logging::line("trial: no restart script is configured (--restore-script); nothing restarted, "
      "no EVENT 0x0106 sent");
    return;
  }
  if (restart_.running()) {
    logging::line("trial: the restart script %s is still running; not started again, no new "
      "EVENT 0x0106", restart_.script());
    return;
  }
  int error = 0;
  if (!restart_.start(now_ms_, error)) {
    logging::line("trial: cannot start %s reset: %s; the controller was not restarted, no EVENT "
      "0x0106 sent", restart_.script(), std::strerror(error));
    return;
  }
  logging::line("trial: %s reset started (deadline %llu s, then TERM, KILL after %llu s)",
    restart_.script(), static_cast<unsigned long long>(config::kRestartScriptMs / 1000),
    static_cast<unsigned long long>(config::kRestartKillGraceMs / 1000));
}

/**
 * @brief Looks at the restart script once, without waiting; acts when it has ended.
 *
 * Called every tick. The reaping and the deadline are in RestartRunner::poll, which never blocks
 * (decision H: the main loop keeps serving both links while the script runs).
 *
 * @return nothing
 */
void Updater::restartWatch()
{
  RestartRunner::Outcome outcome{};
  if (restart_.running() && restart_.poll(now_ms_, outcome)) {
    onRestartFinished(outcome);
  }
}

/**
 * @brief Logs how the restart script ended and tells the ground if the controller was reset.
 *
 * EVENT 0x0106 "controller restarted by the onboard computer", detail 0x01 "trial past its time"
 * (Interfaces Sec. 5.4 table; REQ-013) goes to the ground when the reset happened: the script's
 * line was "RESTORE DONE: reset", or "RESTORE FAILED: after reset" (the reset ran, OpenOCD then
 * did not end cleanly). In every other case the reset is not shown to have happened (the script
 * failed before it, was stopped, or printed nothing) and nothing new is sent; the log names the
 * line, the exit status or signal, and whether the deadline stopped it.
 *
 * @param[in]     outcome  how the child ended (RestartRunner::poll)
 * @return nothing
 */
void Updater::onRestartFinished(const RestartRunner::Outcome & outcome)
{
  const char * how = outcome.abandoned ? "given up after KILL, not reaped"
                   : outcome.killed ? "killed after the deadline"
                   : outcome.deadline_hit ? "stopped with TERM after the deadline"
                   : "ended by itself";
  logging::line("trial: restart script %s: exit status %d, signal %d; result line \"%s\"", how,
    outcome.exit_code, outcome.term_signal, outcome.line.data());
  switch (outcome.result) {
    case RestartRunner::Result::kDone:
      logging::line("trial: the controller was reset; sending EVENT 0x0106 (%s), reason 0x01 "
        "trial past its time", eventName(report::kEventControllerRestarted));
      sendOwnEvent(report::kEventControllerRestarted, report::kRestartReasonTrialPastTime);
      break;
    case RestartRunner::Result::kFailedAfterReset:
      logging::line("trial: the script failed after the reset ran (the line says so); the "
        "controller was reset, so EVENT 0x0106 is sent, reason 0x01 trial past its time");
      sendOwnEvent(report::kEventControllerRestarted, report::kRestartReasonTrialPastTime);
      break;
    case RestartRunner::Result::kFailed:
      logging::line("trial: the restart script did not show that the controller was reset (no "
        "\"RESTORE DONE: reset\" and no \"RESTORE FAILED: after reset\"); no EVENT 0x0106 sent");
      break;
  }
}

/**
 * @brief Sends the onboard computer's own EVENT, with a one-byte detail, to the ground.
 *
 * The event is numbered and stamped by the report store like any report, then sent when a ground
 * has spoken (report_store.hpp; reports before that are dropped, as for the controller's).
 *
 * @param[in]     code    the event code (Interfaces Sec. 5.4, 0x0101 to 0x0106)
 * @param[in]     detail  the detail byte (for 0x0106: the reason)
 * @return nothing
 */
void Updater::sendOwnEvent(uint16_t code, uint8_t detail)
{
  std::array<uint8_t, report::kEventMinBytes + 1> event{};
  event[report::kOffsetKind] = report::kEvent;
  bytes::writeU16(&event[report::kOffsetEventCode], code);
  event[report::kEventDetailOffset] = detail;
  if (!reports_.accept(event.data(), event.size(), clocks::onboardTimeS(), now_ms_, report_.data(),
        report_.size())) {
    logging::line("EVENT 0x%04x not sent: the report store refused it", code);
    return;
  }
  if (!gate_.haveGround()) {
    logging::line("EVENT 0x%04x not sent: no ground has spoken yet", code);
    return;
  }
  if (!gate_.sendReport(report_.data(), static_cast<uint16_t>(event.size()))) {
    logging::line("EVENT 0x%04x not sent: the ground link did not take it", code);
  }
}

/**
 * @brief Runs the timeouts, the controller watch, and sends the next frame on data link A.
 *
 * Timeouts, the controller watch, and the next frame for data link A. Every loop pass.
 *
 * @param[in] now_ms  current time, in ms
 * @return nothing
 */
void Updater::tick(uint64_t now_ms)
{
  now_ms_ = now_ms;
  if (link_.timedOut(now_ms)) {
    onTimeout();
  }
  trialWatch(nullptr);   // decision H, separable: see trialWatch
  restartWatch();        // decision H: the restart script, if one runs
  watch_.check(now_ms);
  sendNext();
}
