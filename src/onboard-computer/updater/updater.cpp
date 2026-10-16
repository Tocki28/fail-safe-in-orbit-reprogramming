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
 * with the payload length its fields need (Interfaces Sec. 5.1). The dangerous commands 0x06 to
 * 0x09 and 0x0C pass here with their lengths, and are then checked against an ARM
 * (isOnboardDangerous); 0x06 and 0x09 have a handler, the others none yet.
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
    case command::kSetLimits:
      return length == command::kSetLimitsBytes;
    case command::kRestore:
      return length == command::kRestoreBytes;
    case command::kReprogram:
      return length == command::kReprogramBytes;
    case command::kSetOnboardTime:
      return length == command::kSetOnboardTimeBytes;
    case command::kReplaceKey:
      return length == command::kReplaceKeyBytes;
    case command::kArm:   // handled before this test (onGroundArm), which checks its own length
      return false;
  }
  // input from the link: every other identifier (0x0A, 0x0B are not built, the rest unknown).
  return false;
}

/**
 * @brief Tells whether a ground command is exempt from the expiry check.
 *
 * SET ONBOARD TIME (0x09) and an ARM (0x0D) whose named command is 0x09 are exempt (owner
 * decision 25, 15 Oct 2026), so that an onboard clock that is wrong, and with it every Expiry,
 * can still be corrected. Both only when addressed to the onboard computer: 0x09 is not a
 * controller command. The ARM must have its whole length (command::kArmBytes), as onGroundArm
 * demands. This is not an extra exemption of the ARM-then-command pair as a whole: the pair's
 * second command is exempt in its own right. Proposal (TBR), awaiting the owner's acceptance:
 * the destination and length conditions.
 *
 * @param[in]     c  the ground command frame; c.payload has c.length valid bytes
 * @return true: skip the expiry check; false: check it
 */
bool expiryExempt(const frame::Frame & c)
{
  if (c.destination != frame::kDestOnboard || c.length == 0) {
    return false;
  }
  if (c.payload[0] == command::kSetOnboardTime) {
    return true;
  }
  return c.payload[0] == command::kArm && c.length == command::kArmBytes &&
         c.payload[1] == command::kSetOnboardTime;
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
 * @brief Tells whether a command to the onboard computer is one that needs an ARM.
 *
 * The "Dangerous: YES" rows for the onboard computer in the Interfaces command table [Sec. 5.1]:
 * SET LIMITS (0x06), RESTORE (0x07), REPROGRAM (0x08), SET ONBOARD TIME (0x09), REPLACE KEY
 * (0x0C); REQ-029, REQ-112.
 *
 * @param[in]     id  the command identifier, payload byte 0
 * @return true: dangerous, needs an ARM; false: not
 */
bool isOnboardDangerous(uint8_t id)
{
  return id == command::kSetLimits || id == command::kRestore || id == command::kReprogram ||
         id == command::kSetOnboardTime || id == command::kReplaceKey;
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
 * shows mode TRIAL (Interfaces Sec. 5.4). The image limits are read from the "limits" record of
 * the store's data directory and given to the store (loadLimits; owner decision 24, 15 Oct 2026).
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
 * @param[in]     restart_deadline_ms  run time after which the restart script gets TERM, in ms;
 *                                the program passes config::kRestartScriptMs
 * @return nothing (constructor)
 */
Updater::Updater(CommandGate & gate, DataLink & link, UploadStore & store,
  const ImageChecker & checker, ReportStore & reports, ControllerWatch & watch, uint32_t beat_ms,
  const char * restart_script, uint64_t restart_deadline_ms)
: gate_(gate), link_(link), store_(store), checker_(checker), reports_(reports), watch_(watch),
  beat_ms_(beat_ms), next_beat_ms_(0), now_ms_(0), sender_(Sender::kNone), reports_dropped_(0),
  queue_(), table_(), delivery_(), arm_{}, pair_(PairStage::kNone), test_trigger_(), trial_watch_(config::kTrialMs, config::kTrialMarginMs),
  restart_(restart_script, restart_deadline_ms, config::kRestartKillGraceMs),
  restart_reason_(report::kRestartReasonTrialPastTime), used_(),
  repeats_(),
  limits_file_(store.directory(), config::kLimitsRecordName, config::kLimitsRecordBytes),
  clock_file_(store.directory(), config::kClockOffsetRecordName, config::kClockOffsetRecordBytes),
  clock_offset_s_(0),
  cancel_requested_(false), cancel_sequence_(0), have_delivered_(false), delivered_number_(0),
  test_image_running_(false), payload_{},
  report_{}
{
  loadLimits();
  loadClockOffset();
}

/**
 * @brief Reads the "limits" record and gives the limits to the store.
 *
 * Owner decision 24, 15 Oct 2026.
 *
 * Called once, by the constructor. A good record gives its minimum version and controller
 * identity. No record (first start) gives minimum version 1 and identity 0x0431 (REQ-053; the
 * identity is a Proposal, TBR, awaiting the owner's acceptance: it is the target the image
 * checker demanded before decision 24). A damaged record (RECORD_DAMAGED) fails closed: the limits
 * are not usable, every upload (at UPLOAD END) and every delivery is refused 0x0A, and the log
 * says why, until a SET LIMITS writes a good record (TBR, awaiting the owner's acceptance). A
 * record that cannot be read at all (RECORD_ERROR) is treated the same way (TBR).
 *
 * @return nothing
 */
void Updater::loadLimits()
{
  ImageLimits limits{};
  int error = 0;
  limits_record::Source source = limits_record::load(limits_file_, limits, error);
  store_.setLimits(limits);
  switch (source) {
    case limits_record::Source::kStored:
      logging::line("limits: stored record, minimum version %u, controller identity 0x%04x",
        limits.minimum_version, limits.identity);
      return;
    case limits_record::Source::kNoRecord:
      logging::line("limits: no record, starting with minimum version %u, controller identity "
        "0x%04x", limits.minimum_version, limits.identity);
      return;
    case limits_record::Source::kDamaged:
      logging::line("limits: record damaged; refusing every upload and delivery 0x0a until SET "
        "LIMITS writes a good one");
      return;
    case limits_record::Source::kUnreadable:
      logging::line("limits: record unreadable (%s); refusing every upload and delivery 0x0a "
        "until SET LIMITS writes a good one", std::strerror(error));
      return;
  }
}

/**
 * @brief Reads the "clock-offset" record and sets the clock offset.
 *
 * Owner decision 25, 15 Oct 2026; REQ-073. Called once, by the constructor. A good record gives
 * its offset. No record (first start) gives offset 0: onboard ground time is the system clock. A
 * damaged record, or one that cannot be read, also gives offset 0 and the log says so (Proposal
 * (TBR), awaiting the owner's acceptance: fail-closed, as for the limits, is not useful for a
 * clock; the next SET ONBOARD TIME writes a good record).
 *
 * @return nothing
 */
void Updater::loadClockOffset()
{
  int error = 0;
  clock_record::Source source = clock_record::load(clock_file_, clock_offset_s_, error);
  switch (source) {
    case clock_record::Source::kStored:
      logging::line("clock: stored offset %lld s; onboard ground time is the system clock plus "
        "that", static_cast<long long>(clock_offset_s_));
      return;
    case clock_record::Source::kNoRecord:
      logging::line("clock: no offset record, starting with offset 0 s");
      return;
    case clock_record::Source::kDamaged:
      logging::line("clock: offset record damaged; using offset 0 s until SET ONBOARD TIME "
        "writes a good one");
      return;
    case clock_record::Source::kUnreadable:
      logging::line("clock: offset record unreadable (%s); using offset 0 s until SET ONBOARD "
        "TIME writes a good one", std::strerror(error));
      return;
  }
}

/**
 * @brief Gives the onboard ground time: the system clock plus the stored offset.
 *
 * Seconds of Unix time, signed 64-bit (owner decision 25, 15 Oct 2026). The sum is limited to the
 * range of int64_t instead of overflowing: a stored offset is read from storage (code rule B4),
 * and a CRC-valid record could hold any value.
 *
 * @return seconds of onboard ground time
 */
int64_t Updater::groundTimeS() const
{
  const int64_t system_s = clocks::systemTimeS();
  if (clock_offset_s_ > 0 && system_s > INT64_MAX - clock_offset_s_) {
    return INT64_MAX;
  }
  if (clock_offset_s_ < 0 && system_s < INT64_MIN - clock_offset_s_) {
    return INT64_MIN;
  }
  return system_s + clock_offset_s_;
}

/**
 * @brief Gives the onboard ground time cut to 32 bits, for the onboard time of a report.
 *
 * The report field is 4 bytes (Interfaces Sec. 5.4); the cut keeps the low 32 bits, as
 * clocks::onboardTimeS did before decision 25.
 *
 * @return onboard ground time, seconds, low 32 bits
 */
uint32_t Updater::groundTimeStampS() const
{
  return static_cast<uint32_t>(static_cast<uint64_t>(groundTimeS()));
}

/**
 * @brief Checks a ground command's Expiry against the onboard ground time (REQ-066).
 *
 * Expiry is 4 bytes, seconds of ground time (Interfaces Sec. 4.0 frame table). Expiry 0 is refused
 * (owner decision 25, 15 Oct 2026: the ground console always sends a real one), and so is an
 * Expiry lower than the onboard ground time; an Expiry equal to it still passes ("after it, the
 * command is refused"). SET ONBOARD TIME and an ARM naming it are exempt (expiryExempt). The
 * comparison is done in 64-bit signed arithmetic, so a ground time beyond 32 bits cannot wrap.
 *
 * @param[in]     c  the ground command frame
 * @return answer::kSuccess: not past its expiry, or exempt; answer::kPastExpiry: refuse it 0x03
 */
uint8_t Updater::expiryCheck(const frame::Frame & c) const
{
  if (expiryExempt(c)) {
    return answer::kSuccess;
  }
  const int64_t now_s = groundTimeS();
  if (c.expiry == 0 || static_cast<int64_t>(c.expiry) < now_s) {
    logging::line("ground: command 0x%02x sequence %u has expiry %u s, onboard ground time is "
      "%lld s; refused 0x03", (c.length > 0) ? c.payload[0] : 0U, c.sequence, c.expiry,
      static_cast<long long>(now_s));
    return answer::kPastExpiry;
  }
  return answer::kSuccess;
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

/**
 * @brief Marks a command finished in both repeat memories.
 *
 * The final answer has gone to the ground. A resend with the same bytes is then answered from the
 * entry: ALREADY DONE after DONE, the same code after REFUSED (REQ-031, owner decision 26).
 * Only a pending entry of that sequence number changes, so it is harmless for a command that is
 * in neither memory (one refused on arrival).
 *
 * @param[in] sequence  the ground command's sequence number
 * @param[in] done  true: it ended DONE; false: it ended REFUSED
 * @param[in] refusal_code  the REFUSED code that went to the ground (ignored when done)
 * @return nothing
 */
void Updater::finishCommand(uint32_t sequence, bool done, uint8_t refusal_code)
{
  used_.finish(sequence, done, refusal_code);
  repeats_.finish(sequence, done, refusal_code);
}

/**
 * @brief Answers a ground command DONE and marks it finished in the repeat memories.
 *
 * @param[in] sequence  the ground command's sequence number
 * @return nothing
 */
void Updater::endDone(uint32_t sequence)
{
  done(sequence);
  finishCommand(sequence, true, 0);
}

/**
 * @brief Answers a ground command REFUSED and marks it finished in the repeat memories.
 *
 * A command that was accepted (remembered) and then refused before or while it went to the
 * controller has ended: a resend with the same bytes is refused with the same code, not answered
 * RECEIVED for ever (Interfaces Sec. 4.2, REQ-031).
 *
 * @param[in] sequence  the ground command's sequence number
 * @param[in] code  the refusal reason (protocol.hpp)
 * @return nothing
 */
void Updater::endRefused(uint32_t sequence, uint8_t code)
{
  refused(sequence, code);
  finishCommand(sequence, false, code);
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
 * (other bytes: REFUSED 0x04, REQ-065; the same bytes: RECEIVED or ALREADY DONE, REQ-031), and a
 * delivery or test image in the way (REFUSED 0x05, ARM kept). A command that is no ARM and not
 * dangerous (LEAVE FALLBACK, GET STATUS) is checked for a repeat in the other memory, RepeatMemory,
 * and remembered there once queued (owner decision 26, 15 Oct 2026).
 * The ARM's arrival time travels with the command so that sendNext can check its age again.
 *
 * Before everything else the Expiry is checked (expiryCheck, REQ-066, owner decision 25, 15 Oct
 * 2026): a command with Expiry 0 or past is refused 0x03 and not remembered, so a resend with a
 * valid Expiry runs. Placing it ahead of the arrival checks and the repeat memories is a Proposal
 * (TBR), awaiting the owner's acceptance: a resend of a finished command whose Expiry has passed
 * since is then refused 0x03, not answered ALREADY DONE.
 *
 * @param[in]     c  the command frame; c.payload is valid during the call
 * @return nothing
 */
void Updater::onGroundCommand(const frame::Frame & c)
{
  const uint8_t expiry_code = expiryCheck(c);
  if (expiry_code != answer::kSuccess) {
    refused(c.sequence, expiry_code);   // not remembered: refused() sends the answer only
    return;
  }
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
  if (!isDangerous(id) && answerRepeat(c, repeats_, used_)) {
    return;   // a repeat of a command that is no ARM and not dangerous (decision 26)
  }
  if (isDangerous(id)) {
    if (answerRepeat(c, used_, repeats_)) {
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
    // controllerCommandOk limited the length to what the ring holds (static_assert in
    // command_gate.cpp).
    if (!used_.add(c.sequence, c.payload, c.length)) {
      logging::line("ground: dangerous command 0x%02x sequence %u: payload too long for the used "
        "sequence ring; not remembered", id, c.sequence);
    }
  } else if (!isDangerous(id) && !repeats_.add(c.sequence, c.payload, c.length)) {
    // controllerCommandOk limited the length to what RepeatMemory holds (static_assert in
    // command_gate.cpp).
    logging::line("ground: command 0x%02x sequence %u: payload too long for the repeat memory; "
      "not remembered", id, c.sequence);
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
 * @brief Takes a ground ARM for the controller or the onboard computer: checks it, holds it,
 *        answers RECEIVED and DONE.
 *
 * The ARM is not passed on: the controller would see a sequence number that is the ground's,
 * not the one its dangerous command will carry on data link A [Interfaces, Sec. 5.2]. One ARM
 * is held; a new one replaces the old. The destination decides which identifiers it may name:
 * Destination 0x02, the controller's dangerous commands (isDangerous); Destination 0x01, the
 * onboard computer's (isOnboardDangerous). Both kinds share the one held ARM and the same
 * kArmExpiryMs (TBR, awaiting the owner's acceptance: the identifiers cannot overlap, so a second
 * slot would only allow two ARMs at once, which Sec. 5.2 does not ask for).
 *
 * @param[in]     c  the ARM frame (Destination 0x01 or 0x02, payload 0x0D, identifier (1),
 *                   sequence (4))
 * @return nothing
 */
void Updater::onGroundArm(const frame::Frame & c)
{
  bool onboard = (c.destination == frame::kDestOnboard);
  if (c.length != command::kArmBytes ||
      !(onboard ? isOnboardDangerous(c.payload[1]) : isDangerous(c.payload[1]))) {
    logging::line("ground: ARM sequence %u names 0x%02x or has %u payload bytes: not a dangerous "
      "command of the %s; refused 0x06", c.sequence,
      (c.length > 1) ? c.payload[1] : 0, c.length, onboard ? "onboard computer" : "controller");
    refused(c.sequence, answer::kImpossible);
    return;
  }
  if (answerRepeat(c, used_, repeats_)) {
    return;
  }
  if (!used_.add(c.sequence, c.payload, c.length)) {
    logging::line("ground: ARM sequence %u: payload too long for the used sequence ring; refused "
      "0x06", c.sequence);
    refused(c.sequence, answer::kImpossible);
    return;
  }
  arm_.held = true;
  arm_.identifier = c.payload[1];
  arm_.command_sequence = bytes::readU32(&c.payload[2]);
  arm_.at_ms = now_ms_;
  logging::line("ground: ARM sequence %u held for command 0x%02x, ground sequence %u (valid "
    "%llu ms)", c.sequence, arm_.identifier, arm_.command_sequence,
    static_cast<unsigned long long>(config::kArmExpiryMs));
  received(c.sequence);
  done(c.sequence);
  finishCommand(c.sequence, true, 0);   // RECEIVED and DONE are both sent: a resend is ALREADY DONE
}

template <typename Own, typename Other>
/**
 * @brief Answers a ground command whose sequence number is already remembered.
 *
 * The ground console resends a command with the same sequence number and the same bytes when its
 * answer is lost, so that the receiver can recognise the repeat and run the command once
 * (Interfaces Sec. 4.2, REQ-031). Same bytes, first one ended DONE: ALREADY DONE, nothing else
 * (nothing goes to the controller, no ARM is held). Same bytes, first one not ended: RECEIVED
 * only; its final answer follows. Same bytes, first one ended REFUSED: REFUSED with the same
 * code, nothing sent to the controller, so the operator learns the real reason and is never told
 * the command ran (owner decided, 13 Oct 2026). Other bytes under a used number: REFUSED 0x04,
 * a replay (REQ-065). A new number: not handled here.
 *
 * Owner decision 26 (15 Oct 2026) keeps two memories: UsedSequences for ARMs and dangerous
 * commands, RepeatMemory for every other command. A command is compared with its own memory
 * first. If its number is new there but sits in the other memory, the bytes cannot be the same
 * (the other memory holds the other kind of command), so it is refused 0x04 like any other bytes
 * under a used number. The answers go out directly, not through endRefused: they must not finish
 * the entry of the first command.
 *
 * @param[in]     c      the ground command frame
 * @param[in]     own    the memory this kind of command belongs to
 * @param[in]     other  the other memory
 * @return true: it was a repeat and has been answered; false: a new sequence number, go on
 */
bool Updater::answerRepeat(const frame::Frame & c, const Own & own, const Other & other)
{
  uint8_t refusal_code = answer::kOldCommand;
  uint8_t unused_code = 0;
  if (other.check(c.sequence, c.payload, c.length, unused_code) != SequenceRepeat::kNew) {
    logging::line("ground: sequence %u: that sequence number was used before by a command of the "
      "other kind (other bytes); refused 0x04", c.sequence);
    (void)gate_.answer(c.sequence, answer::kRefused, answer::kOldCommand);
    return true;
  }
  switch (own.check(c.sequence, c.payload, c.length, refusal_code)) {
    case SequenceRepeat::kNew:
      return false;
    case SequenceRepeat::kSameDone:
      logging::line("ground: sequence %u sent again with the same bytes after it finished; "
        "answered ALREADY DONE, nothing else done", c.sequence);
      (void)gate_.answer(c.sequence, answer::kAlreadyDone, answer::kSuccess);
      return true;
    case SequenceRepeat::kSamePending:
      logging::line("ground: sequence %u sent again with the same bytes while it has not "
        "finished; answered RECEIVED, nothing else done", c.sequence);
      received(c.sequence);
      return true;
    case SequenceRepeat::kSameRefused:
      logging::line("ground: sequence %u sent again with the same bytes after it was refused "
        "0x%02x; refused 0x%02x again, nothing else done", c.sequence, refusal_code,
        refusal_code);
      refused(c.sequence, refusal_code);
      return true;
    case SequenceRepeat::kDifferent:
      logging::line("ground: sequence %u: that sequence number was used before (other bytes); "
        "refused 0x04", c.sequence);
      refused(c.sequence, answer::kOldCommand);
      return true;
  }
  // Not reached: a value of SequenceRepeat that is none of the above is corrupted memory; the
  // command is refused rather than run (the updater has no fault reaction yet, open).
  refused(c.sequence, answer::kOldCommand);
  return true;
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
 * A command that fails the arrival checks (arrivalOk) is answered REFUSED 0x06 only, and is not
 * remembered. Any other is checked for a repeat (RepeatMemory, owner decision 26; the dangerous
 * ones in UsedSequences), then answered RECEIVED, run, and ends in DONE or REFUSED (Interfaces
 * Sec. 4.2, REQ-033). An image number 0xFFFF (the test image's) is refused 0x06 on arrival.
 *
 * @param[in] c  the command frame
 * @return nothing
 */
void Updater::onOnboardCommand(const frame::Frame & c)
{
  uint8_t id = (c.length > 0) ? c.payload[0] : 0;
  if (id == command::kArm) {
    onGroundArm(c);   // answers by itself, RECEIVED and DONE or REFUSED 0x06; no RECEIVED first
    return;
  }
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
  if (isOnboardDangerous(id)) {
    onboardDangerous(c);
    return;
  }
  // Every other command to the onboard computer is remembered too (owner decision 26, 15 Oct
  // 2026): a resend with the same bytes is not run again.
  if (answerRepeat(c, repeats_, used_)) {
    return;
  }
  if (!repeats_.add(c.sequence, c.payload, c.length)) {
    // arrivalOk limited the length to what RepeatMemory holds (static_assert in command_gate.cpp).
    logging::line("ground: command 0x%02x sequence %u: payload too long for the repeat memory; "
      "refused 0x06", id, c.sequence);
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
    case command::kSetLimits:
    case command::kRestore:
    case command::kReprogram:
    case command::kSetOnboardTime:
    case command::kReplaceKey:
    case command::kArm:           break;
  }
  // Not reached: arrivalOk lets only the six above in, and the dangerous ones went to
  // onboardDangerous, the ARM to onGroundArm.
  endRefused(c.sequence, answer::kImpossible);
}

/**
 * @brief Runs a ground dangerous command for the onboard computer: ARM check, then its handler.
 *
 * Order of checks (Interfaces Sec. 5.2; REQ-029, REQ-030, REQ-112): 1. a repeat of a used
 * sequence number (answerRepeat: ALREADY DONE, RECEIVED, the first one's REFUSED code, or
 * REFUSED 0x04); 2. a held ARM naming this identifier and this ground sequence number, else
 * REFUSED 0x01 (the ARM stays held); 3. the ARM no older than kArmExpiryMs, else REFUSED 0x02
 * (the ARM is dropped); 4. the ARM is used up, the sequence number is remembered, and
 * runOnboardDangerous runs. Like a dangerous command for the controller, a refusal before step 4
 * is REFUSED only, with no RECEIVED first. The handler's code ends the command: DONE for
 * answer::kSuccess, else REFUSED with that code, which the ring keeps so that a resend is refused
 * the same way (REQ-031). RECEIVED before a handler that takes time is the handler's decision
 * (TBR: none exists yet).
 *
 * @param[in]     c  the command frame; its identifier passed isOnboardDangerous and arrivalOk
 * @return nothing
 */
void Updater::onboardDangerous(const frame::Frame & c)
{
  uint8_t id = c.payload[0];
  if (answerRepeat(c, used_, repeats_)) {
    return;
  }
  uint8_t code = armCheck(c);
  if (code != answer::kSuccess) {
    refused(c.sequence, code);
    return;
  }
  arm_.held = false;   // used up: it allowed this one command
  if (!used_.add(c.sequence, c.payload, c.length)) {
    // arrivalOk limited the length to what the ring holds (static_assert in command_gate.cpp).
    logging::line("ground: dangerous command 0x%02x sequence %u: payload too long for the used "
      "sequence ring; not remembered", id, c.sequence);
  }
  code = runOnboardDangerous(id, c.payload, c.length);
  if (code == answer::kSuccess) {
    endDone(c.sequence);
    return;
  }
  endRefused(c.sequence, code);
}

/**
 * @brief Runs an armed dangerous command of the onboard computer: SET LIMITS has a handler, the
 *        other four do not yet.
 *
 * The place where the later decisions fill in RESTORE, REPROGRAM and REPLACE KEY (REQ-055,
 * REQ-058, REQ-084). SET LIMITS (0x06) is built (owner decision 24, 15 Oct 2026; runSetLimits) and
 * so is SET ONBOARD TIME (0x09; owner decision 25, 15 Oct 2026; runSetOnboardTime). Until the
 * others are built each is answered REFUSED 0x06 "not built", after
 * the arming check, so the order of the checks shows. The identifier reached here passed
 * isOnboardDangerous, so any other value is not expected; it is refused 0x06 and logged.
 *
 * @param[in]     id       the command identifier, payload byte 0; one of the five dangerous ones
 * @param[in]     payload  the command's payload bytes (length of them valid)
 * @param[in]     length   number of payload bytes; already checked against the command's fields
 * @return answer::kSuccess: the command ran; otherwise the refusal reason (protocol.hpp)
 */
uint8_t Updater::runOnboardDangerous(uint8_t id, const uint8_t * payload, uint16_t length)
{
  switch (id) {
    case command::kSetLimits:
      return runSetLimits(payload, length);
    case command::kSetOnboardTime:
      return runSetOnboardTime(payload, length);
    case command::kRestore:
    case command::kReprogram:
    case command::kReplaceKey:
      logging::line("ground: dangerous command 0x%02x passed the ARM check; not built: refused "
        "0x06", id);
      return answer::kImpossible;
  }
  // input from the link: not one of the five; isOnboardDangerous let only those through
  logging::line("ground: command 0x%02x is not a dangerous command of the onboard computer; "
    "refused 0x06", id);
  return answer::kImpossible;
}

/**
 * @brief Runs SET LIMITS: keeps a new minimum version and controller identity (decision 24).
 *
 * Owner decision 24 (15 Oct 2026; REQ-045, REQ-053). The fields are minimum version (4) and
 * controller identity (2), little-endian (Interfaces Sec. 5.1 table, row 0x06). The limits are
 * written as the "limits" DurableRecord first; only when that succeeded are they given to the
 * store and DONE is answered, so an answer of DONE means the record is on storage (REQ-034). A
 * write that fails answers REFUSED 0xFE (the onboard computer's storage failed, protocol.hpp) and
 * leaves the limits in force as they were; a record whose write failed only at the final
 * directory flush may already hold the new values on storage (durable_record.hpp), and is read
 * again at the next start. Lowering the minimum needs only the ARM that got here. A good write
 * also ends a fail-closed state (a damaged record at start): the limits are usable again. No
 * range is checked on either field (TBR, awaiting the owner's acceptance: decision 24 names none;
 * identity 0 or a very high minimum make every image refused, the safe direction).
 *
 * @param[in]     payload  the command's payload bytes
 * @param[in]     length   number of payload bytes; must be command::kSetLimitsBytes
 * @return answer::kSuccess: written and in force; answer::kImpossible: wrong length (not expected,
 *         arrivalOk checked it); answer::kStorageBench: the record could not be written
 */
uint8_t Updater::runSetLimits(const uint8_t * payload, uint16_t length)
{
  if (length != command::kSetLimitsBytes) {
    logging::line("SET LIMITS: %u payload bytes, not %zu; refused 0x06", length,
      command::kSetLimitsBytes);
    return answer::kImpossible;
  }
  ImageLimits next{bytes::readU32(&payload[command::kSetLimitsVersionAt]),
                   bytes::readU16(&payload[command::kSetLimitsIdentityAt]), true};
  int error = 0;
  if (!limits_record::save(limits_file_, next, error)) {
    logging::line("SET LIMITS: minimum version %u, controller identity 0x%04x: the limits record "
      "could not be written (%s); refused 0xfe, the limits in force are unchanged",
      next.minimum_version, next.identity, std::strerror(error));
    return answer::kStorageBench;
  }
  const ImageLimits before = store_.limits();
  store_.setLimits(next);
  logging::line("SET LIMITS: minimum version %u, controller identity 0x%04x written and in force "
    "(before: %s, minimum version %u, controller identity 0x%04x)", next.minimum_version,
    next.identity, before.usable ? "usable" : "not usable", before.minimum_version,
    before.identity);
  return answer::kSuccess;
}

/**
 * @brief Runs SET ONBOARD TIME: keeps the offset between the system clock and ground time
 *        (decision 25).
 *
 * Owner decision 25 (15 Oct 2026; REQ-072, REQ-073). The field is the ground time (4), unsigned
 * seconds of Unix time, little-endian (Interfaces Sec. 5.1 table, row 0x09). Steps, in this order:
 * 1. difference = onboard ground time - ground time, signed seconds, taken before anything
 * changes; 2. if its size is above config::kClockDifferenceMaxS (REQ-072), the onboard computer
 * sends its own EVENT 0x0105 (protocol.hpp: detail the difference, 4 bytes, signed,
 * little-endian, onboard minus ground; layout TBR); the event is sent before the write, so it is
 * sent even when the write then fails (Proposal (TBR), awaiting the owner's acceptance); 3. the
 * offset = ground time - system clock is written as the "clock-offset" DurableRecord; only when
 * that succeeded does the offset change in memory and DONE follow, so DONE means the record is on
 * storage (REQ-034). A write that fails answers REFUSED 0xFE (protocol.hpp) and leaves the offset
 * in force as it was. The system clock itself is not set (REQ-073). A difference beyond the range
 * of 4 signed bytes is sent as the nearest value in that range (Proposal (TBR), awaiting the
 * owner's acceptance).
 *
 * @param[in]     payload  the command's payload bytes
 * @param[in]     length   number of payload bytes; must be command::kSetOnboardTimeBytes
 * @return answer::kSuccess: written and in force; answer::kImpossible: wrong length (not expected,
 *         arrivalOk checked it); answer::kStorageBench: the record could not be written
 */
uint8_t Updater::runSetOnboardTime(const uint8_t * payload, uint16_t length)
{
  if (length != command::kSetOnboardTimeBytes) {
    logging::line("SET ONBOARD TIME: %u payload bytes, not %zu; refused 0x06", length,
      command::kSetOnboardTimeBytes);
    return answer::kImpossible;
  }
  const int64_t ground_s = bytes::readU32(&payload[command::kSetOnboardTimeAt]);
  const int64_t onboard_s = groundTimeS();
  const int64_t difference_s = onboard_s - ground_s;
  const bool reported = difference_s > config::kClockDifferenceMaxS ||
                        difference_s < -config::kClockDifferenceMaxS;
  if (reported) {
    // Limited to what 4 signed bytes hold; the sum is cast to unsigned for the little-endian write.
    const int64_t shown_s = (difference_s > INT32_MAX) ? INT32_MAX :
                            (difference_s < INT32_MIN) ? INT32_MIN : difference_s;
    std::array<uint8_t, report::kClockDifferenceDetailBytes> detail{};
    bytes::writeU32(detail.data(), static_cast<uint32_t>(static_cast<int32_t>(shown_s)));
    logging::line("SET ONBOARD TIME: onboard ground time %lld s, ground time %lld s: difference "
      "%lld s is above %lld s; EVENT 0x0105", static_cast<long long>(onboard_s),
      static_cast<long long>(ground_s), static_cast<long long>(difference_s),
      static_cast<long long>(config::kClockDifferenceMaxS));
    sendOwnEventDetail(report::kEventClockDiffers, detail.data(), detail.size());
  }
  const int64_t next_offset_s = ground_s - clocks::systemTimeS();
  int error = 0;
  if (!clock_record::save(clock_file_, next_offset_s, error)) {
    logging::line("SET ONBOARD TIME: offset %lld s: the clock-offset record could not be written "
      "(%s); refused 0xfe, the offset in force (%lld s) is unchanged",
      static_cast<long long>(next_offset_s), std::strerror(error),
      static_cast<long long>(clock_offset_s_));
    return answer::kStorageBench;
  }
  logging::line("SET ONBOARD TIME: ground time %lld s; offset %lld s written and in force "
    "(before: %lld s, difference %lld s)", static_cast<long long>(ground_s),
    static_cast<long long>(next_offset_s), static_cast<long long>(clock_offset_s_),
    static_cast<long long>(difference_s));
  clock_offset_s_ = next_offset_s;
  return answer::kSuccess;
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
    endRefused(c.sequence, code);
    return;
  }
  logging::line("UPLOAD BEGIN image %u, %u bytes: %s at byte %u", number, length,
    (stored > 0) ? "resuming" : "starting", stored);
  endDone(c.sequence);
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
    endRefused(c.sequence, code);
    return;
  }
  if (!written) {
    logging::line("UPLOAD CHUNK image %u at %u: already stored", number, offset);
  }
  endDone(c.sequence);
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
    endRefused(c.sequence, code);
    return;
  }
  logging::line("UPLOAD END image %u: checks passed (kind 0x%02x, version %u); stored", number,
    verdict.kind, verdict.version);
  endDone(c.sequence);
}

/**
 * @brief Runs UPLOAD CANCEL: removes the stored bytes of the image; stops its delivery if one runs.
 *
 * Image not being delivered: removes its .part and .img files and answers DONE. Image being
 * delivered (owner decision 21, 15 Oct 2026; REQ-051): the delivery stops after the step in
 * flight (stop-and-wait, data_link.hpp), the onboard computer sends UPLOAD CANCEL to the
 * controller (which erases the slot's first page, update_receiver.cpp runCancel), and then
 * finishCancelledDelivery answers the running DELIVER REFUSED 0x12 and this command. The answer to
 * this command is therefore not given here; RECEIVED has gone already. A second UPLOAD CANCEL for
 * the same delivery, under another sequence number, while the first one is being carried out, is
 * refused 0x05 (bench choice, as for a second DELIVER: Sec. 4.2 has no reason for "busy"; TBR,
 * awaiting the owner's acceptance).
 *
 * @param[in] c  the UPLOAD CANCEL command frame
 * @return nothing
 */
void Updater::uploadCancel(const frame::Frame & c)
{
  uint16_t number = imageNumber(c);
  if (delivery_.active() && !test_image_running_ && delivery_.number() == number) {
    if (cancel_requested_) {
      logging::line("UPLOAD CANCEL image %u sequence %u: the delivery is already being cancelled; "
        "refused 0x05", number, c.sequence);
      endRefused(c.sequence, answer::kNotInThisMode);
      return;
    }
    cancel_requested_ = true;
    cancel_sequence_ = c.sequence;
    logging::line("UPLOAD CANCEL image %u: the delivery of this image runs; it stops after the "
      "step in flight", number);
    sendNext();   // link A idle: the UPLOAD CANCEL to the controller goes at once
    return;
  }
  (void)store_.cancel(number);
  logging::line("UPLOAD CANCEL image %u: stored bytes removed", number);
  endDone(c.sequence);
}

/**
 * @brief Sends UPLOAD CANCEL to the controller for the delivery being cancelled (decision 21).
 *
 * The step in flight has ended (its answer, or its timeout); this is the next frame instead of
 * the delivery's next step. It carries the DELIVER's expiry like the delivery's other frames. If
 * data link A cannot send, the cancel ends at once with the bench code 0xFD (protocol.hpp).
 *
 * @return nothing
 */
void Updater::sendDeliveryCancel()
{
  std::array<uint8_t, command::kImageNumberOnlyBytes> cancel{};
  cancel[0] = command::kUploadCancel;
  bytes::writeU16(&cancel[command::kIdBytes], delivery_.number());
  uint32_t sequence = 0;
  if (link_.sendCommand(cancel.data(), static_cast<uint16_t>(cancel.size()), delivery_.expiry(),
        config::kAnswerTimeoutMs, now_ms_, sequence)) {
    sender_ = Sender::kDeliveryCancel;
    logging::line("DELIVER image %u: UPLOAD CANCEL sent to the controller as sequence %u",
      delivery_.number(), sequence);
    return;
  }
  logging::line("DELIVER image %u: data link A cannot send the UPLOAD CANCEL", delivery_.number());
  finishCancelledDelivery(answer::kCannotSendBench);
}

/**
 * @brief Ends a delivery the operator cancelled: answers the DELIVER, then the UPLOAD CANCEL.
 *
 * Owner decision 21 (15 Oct 2026; REQ-051): the running DELIVER is answered REFUSED 0x12
 * "delivery cancelled by the operator" (answer::kCancelled). The stored bytes of the image are
 * removed, as for any UPLOAD CANCEL (REQ-052). The UPLOAD CANCEL itself is then answered DONE when
 * the controller accepted its UPLOAD CANCEL; if the controller refused it, did not answer, or the
 * link could not send, it is answered REFUSED with that code (the controller's code, 0xFF or 0xFD),
 * because DONE would tell the operator the slot was erased. TBR, awaiting the owner's acceptance:
 * decision 21 names only the DONE case. The DELIVER is refused 0x12 in every case.
 *
 * @param[in]     controller_code  answer::kSuccess if the controller answered DONE to the
 *                UPLOAD CANCEL, else its refusal code, answer::kNoAnswerBench or
 *                answer::kCannotSendBench
 * @return nothing
 */
void Updater::finishCancelledDelivery(uint8_t controller_code)
{
  uint16_t number = delivery_.number();
  logging::line("DELIVER image %u: refused 0x%02x (delivery cancelled by the operator) after "
    "%zu of %zu bytes", number, answer::kCancelled, delivery_.sent(), delivery_.size());
  endRefused(delivery_.groundSequence(), answer::kCancelled);
  (void)store_.cancel(number);
  if (controller_code == answer::kSuccess) {
    logging::line("UPLOAD CANCEL image %u: the controller accepted UPLOAD CANCEL; stored bytes "
      "removed", number);
    endDone(cancel_sequence_);
  } else {
    logging::line("UPLOAD CANCEL image %u: the controller's UPLOAD CANCEL ended with code 0x%02x "
      "(%s); stored bytes removed, refused 0x%02x", number, controller_code,
      refusalName(controller_code), controller_code);
    endRefused(cancel_sequence_, controller_code);
  }
  cancel_requested_ = false;
  delivery_.stop();
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
    endRefused(c.sequence, answer::kNotInThisMode);
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
    endRefused(c.sequence, code);
    return;
  }
  size_t size = 0;
  if (!store_.load(number, delivery_.buffer(), delivery_.capacity(), size)) {
    logging::line("DELIVER image %u: refused 0x06: no checked image of that number", number);
    endRefused(c.sequence, answer::kImpossible);
    return;
  }
  // REQ-047: checked again after its last transfer (here: from storage), just before delivery,
  // against the limits in force now (owner decision 24, 15 Oct 2026).
  ImageVerdict verdict = checker_.check(delivery_.buffer(), size, store_.limits());
  if (verdict.code != answer::kSuccess) {
    logging::line("DELIVER image %u: refused 0x%02x: check before delivery failed (%s)", number,
      verdict.code, verdict.reason);
    endRefused(c.sequence, verdict.code);
    return;
  }
  if (verdict.kind != IMAGE_KIND_MAIN) {
    logging::line("DELIVER image %u: refused 0x06: kind 0x%02x; only main firmware is delivered",
      number, verdict.kind);
    endRefused(c.sequence, answer::kImpossible);
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
  endDone(c.sequence);
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
    have_delivered_ = true;   // owner decision 22: its file is deleted when the firmware settles
    delivered_number_ = delivery_.number();
    endDone(delivery_.groundSequence());
  } else {
    logging::line("DELIVER image %u: refused 0x%02x (%s) after %zu of %zu bytes",
      delivery_.number(), code, refusalName(code), delivery_.sent(), delivery_.size());
    endRefused(delivery_.groundSequence(), code);
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
  if (watch_.heard(now_ms_)) {
    // Decision 27 (owner, 15 Oct 2026; REQ-082): tell the ground the link works again, once.
    sendOwnEvent(report::kEventDataLinkWorking, report::kLinkIdDataLinkA);
  }
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
        finishCommand(ground_sequence, a.kind != answer::kRefused, a.code);
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
          finishCommand(ground_sequence, a.kind != answer::kRefused, a.code);
          table_.remove(a.answered);
        }
      }
      break;
    case Sender::kDelivery:
      if (final && cancel_requested_) {
        // Decision 21: the step in flight has ended; whatever its result, the delivery stops here
        // and the next frame is UPLOAD CANCEL (sendNext).
        logging::line("controller: answer 0x%02x code 0x%02x to the delivery step in flight; the "
          "delivery is being cancelled, so the step's result is not used", a.kind, a.code);
      } else if (final) {
        uint8_t code = answer::kSuccess;
        Delivery::Step step = delivery_.onFinalAnswer(a.kind, a.code, code);
        if (step != Delivery::Step::kContinue) {
          finishDelivery(step, code);
        }
      }
      break;
    case Sender::kDeliveryCancel:
      if (final) {
        // ALREADY DONE to the cancel can only be a sequence-number clash (the updater sends each
        // frame once): a refusal, 0x04, as for the delivery's other steps
        // (Delivery::onFinalAnswer).
        uint8_t code = (a.kind == answer::kRefused) ? a.code
                     : (a.kind == answer::kAlreadyDone) ? answer::kOldCommand : answer::kSuccess;
        logging::line("controller: answer 0x%02x code 0x%02x to the UPLOAD CANCEL of the delivery",
          a.kind, a.code);
        finishCancelledDelivery(code);
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
          endRefused(queue_.front().ground_sequence, code);
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
 * @brief Deletes the stored image of the last delivery once the controller's firmware is settled.
 *
 * Owner decision 22 (15 Oct 2026; REQ-052): EVENT 0x0007 (firmware confirmed) or 0x0008 (firmware
 * given up) after a delivery means the update has ended, so the onboard computer frees the
 * storage of the image it last delivered (image-n.img) and logs it. "Last delivered" is the image
 * of the last ground DELIVER that ended DONE (TBR, awaiting the owner's acceptance: the decision
 * does not say whether a delivery that was refused counts). Only the .img goes; a .part file of an
 * upload in progress stays (UploadStore::removeImage). An image refused at UPLOAD END was removed
 * there already (update_manager.cpp). Kept in RAM, so a restart of the updater forgets which image
 * was delivered (TBR). One delivery is deleted once: a second EVENT finds nothing to delete.
 *
 * @param[in]     code  report::kEventFirmwareConfirmed or report::kEventFirmwareGivenUp
 * @return nothing
 */
void Updater::onFirmwareSettled(uint16_t code)
{
  const char * what = eventName(code);
  if (!have_delivered_) {
    logging::line("EVENT 0x%04x (%s): no delivered image to delete", code, what);
    return;
  }
  uint16_t number = delivered_number_;
  have_delivered_ = false;
  if (store_.removeImage(number)) {
    logging::line("EVENT 0x%04x (%s): the delivered image %u was deleted from storage "
      "(image-%05u.img)", code, what, number, static_cast<unsigned>(number));
  } else {
    logging::line("EVENT 0x%04x (%s): the delivered image %u could not be deleted, or was already "
      "gone (image-%05u.img)", code, what, number, static_cast<unsigned>(number));
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
  if (!reports_.accept(f.payload, f.length, groundTimeStampS(), now_ms_, report_.data(),
        report_.size())) {
    return;
  }
  if (f.payload[report::kOffsetKind] == report::kHealth) {
    test_trigger_.onHealthMode(f.payload[report::kOffsetMode]);
  } else if (f.payload[report::kOffsetKind] == report::kEvent && f.length >= report::kEventMinBytes) {
    uint16_t code = bytes::readU16(&f.payload[report::kOffsetEventCode]);
    if (code == report::kEventTrialStarted) {
      test_trigger_.onTrialStartedEvent();
    } else if (code == report::kEventFirmwareConfirmed || code == report::kEventFirmwareGivenUp) {
      onFirmwareSettled(code);
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
    endRefused(c.ground_sequence, answer::kCannotSendBench);
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
    endRefused(c.ground_sequence, answer::kCannotSendBench);
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
    if (cancel_requested_) {
      sendDeliveryCancel();   // decision 21: instead of the delivery's next step
      return;
    }
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
      endRefused(front.ground_sequence, answer::kArmExpired);
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
      if (cancel_requested_) {
        // Decision 21: the step in flight ended without an answer; the cancel goes next (sendNext).
        logging::line("controller: the delivery is being cancelled, so the missing answer is not "
          "a failure of the delivery");
      } else {
        finishDelivery(Delivery::Step::kRefused, answer::kNoAnswerBench);
      }
      break;
    case Sender::kDeliveryCancel:
      logging::line("controller: no answer to the UPLOAD CANCEL of the delivery, sequence %u, in "
        "time", sequence);
      finishCancelledDelivery(answer::kNoAnswerBench);
      break;
    case Sender::kArm:
      // Decision S (accepted by the owner, 13 Oct 2026): the operator who sent a dangerous
      // command is told it did not go, with the bench code 0xFF "controller did not answer".
      logging::line("controller: no answer to ARM sequence %u in time; the ground command sequence "
        "%u is not sent; refused 0xff (controller did not answer)", sequence,
        queue_.front().ground_sequence);
      endRefused(queue_.front().ground_sequence, answer::kNoAnswerBench);
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
    trial_watch_.onHealthMode(report->payload[report::kOffsetMode],
      report->payload[report::kOffsetFailedStarts], now_ms_);   // F-2: count re-arms the watch
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
  restart_reason_ = report::kRestartReasonTrialPastTime;
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
 * or 0x02 "controller silent" (owner decision 20; Interfaces Sec. 5.4 table; REQ-013), as the
 * reset was started (restart_reason_), goes to the ground when the reset happened: the script's
 * line was "RESTORE DONE: reset", or "RESTORE FAILED: after reset" (the reset ran, OpenOCD then
 * did not end cleanly). In every other case the reset is not shown to have happened (the script
 * failed before it, was stopped, or printed nothing) and nothing new is sent; the log names the
 * line, the exit status or signal, and whether the deadline stopped it. The log lines begin
 * "trial:" for decision H's reset and "controller silent:" for decision 20's.
 *
 * @param[in]     outcome  how the child ended (RestartRunner::poll)
 * @return nothing
 */
void Updater::onRestartFinished(const RestartRunner::Outcome & outcome)
{
  const bool silent_reset = (restart_reason_ == report::kRestartReasonControllerSilent);
  const char * label = silent_reset ? "controller silent" : "trial";
  const char * reason = silent_reset ? "controller silent" : "trial past its time";
  const char * how = outcome.abandoned ? "given up after KILL, not reaped"
                   : outcome.killed ? "killed after the deadline"
                   : outcome.deadline_hit ? "stopped with TERM after the deadline"
                   : "ended by itself";
  logging::line("%s: restart script %s: exit status %d, signal %d; result line \"%s\"", label,
    how, outcome.exit_code, outcome.term_signal, outcome.line.data());
  switch (outcome.result) {
    case RestartRunner::Result::kDone:
      logging::line("%s: the controller was reset; sending EVENT 0x0106 (%s), reason 0x%02x %s",
        label, eventName(report::kEventControllerRestarted), restart_reason_, reason);
      sendOwnEvent(report::kEventControllerRestarted, restart_reason_);
      break;
    case RestartRunner::Result::kFailedAfterReset:
      logging::line("%s: the script failed after the reset ran (the line says so); the "
        "controller was reset, so EVENT 0x0106 is sent, reason 0x%02x %s", label, restart_reason_,
        reason);
      sendOwnEvent(report::kEventControllerRestarted, restart_reason_);
      break;
    case RestartRunner::Result::kFailed:
      logging::line("%s: the restart script did not show that the controller was reset (no "
        "\"RESTORE DONE: reset\" and no \"RESTORE FAILED: after reset\"); no EVENT 0x0106 sent",
        label);
      break;
  }
}

/**
 * @brief Runs the reset schedule of a silent controller (owner decision 20, 15 Oct 2026).
 *
 * Called every tick, after the controller watch was checked. The watch says what is due
 * (ControllerWatch::resetDue): a reset, or the give-up. After the last reset and a further gap of
 * silence nothing is restored: the restore method awaits the owner (decision 20, TBD), so exactly
 * one line is logged and nothing more is done until the controller is heard again.
 *
 * @return nothing
 */
void Updater::silenceWatch()
{
  switch (watch_.resetDue(now_ms_)) {
    case ControllerWatch::ResetStep::kNone:
      break;
    case ControllerWatch::ResetStep::kReset:
      onSilentReset();
      break;
    case ControllerWatch::ResetStep::kGiveUp:
      logging::line("controller still silent after %u resets; restore not run (decision 20: "
        "method awaits the owner, TBD)", static_cast<unsigned>(watch_.resetsDone()));
      watch_.gaveUp();
      break;
  }
}

/**
 * @brief Resets a silent controller with the restart script, if it is free (decision 20).
 *
 * The same runner and script as decision H (restart_runner.hpp), so a reset is never run twice at
 * once: when the runner is busy (decision H's reset, or the previous one of this silence) nothing
 * is done and the watch keeps the reset due, so it starts on the first tick the runner is free.
 * The reset is counted when it is started. Proposal (TBR), awaiting the owner's acceptance: it is
 * also counted when the script is not configured or cannot be started (logged), so the schedule
 * goes on at the gap and cannot loop every tick. EVENT 0x0106 with reason 0x02 goes to the ground
 * from onRestartFinished once the reset happened.
 *
 * @return nothing
 */
void Updater::onSilentReset()
{
  if (restart_.running()) {
    return;
  }
  watch_.resetRequested(now_ms_);
  const unsigned number = static_cast<unsigned>(watch_.resetsDone());
  if (!restart_.configured()) {
    logging::line("controller silent: no restart script is configured (--restore-script); reset "
      "%u of %u not run, no EVENT 0x0106 sent (REQ-061)", number,
      static_cast<unsigned>(config::kSilentResetsMax));
    return;
  }
  int error = 0;
  if (!restart_.start(now_ms_, error)) {
    logging::line("controller silent: cannot start %s reset %u of %u: %s; the controller was not "
      "restarted, no EVENT 0x0106 sent", restart_.script(), number,
      static_cast<unsigned>(config::kSilentResetsMax), std::strerror(error));
    return;
  }
  restart_reason_ = report::kRestartReasonControllerSilent;
  logging::line("controller silent: %s reset %u of %u started (REQ-061; next one in %llu s if "
    "still silent)", restart_.script(), number, static_cast<unsigned>(config::kSilentResetsMax),
    static_cast<unsigned long long>(config::kSilentResetGapMs / 1000));
}

/**
 * @brief Sends the onboard computer's own EVENT, with a one-byte detail, to the ground.
 *
 * The event is numbered and stamped by the report store like any report, then sent when a ground
 * has spoken (report_store.hpp; reports before that are dropped, as for the controller's).
 * Forwards to sendOwnEventDetail.
 *
 * @param[in]     code    the event code (Interfaces Sec. 5.4, 0x0101 to 0x0106)
 * @param[in]     detail  the detail byte (for 0x0106: the reason)
 * @return nothing
 */
void Updater::sendOwnEvent(uint16_t code, uint8_t detail)
{
  sendOwnEventDetail(code, &detail, 1);
}

/**
 * @brief Sends the onboard computer's own EVENT, with a detail of any length that fits, to the
 *        ground.
 *
 * The event is numbered and stamped (with the onboard ground time, owner decision 25) by the
 * report store like any report, then sent when a ground has spoken (report_store.hpp; reports
 * before that are dropped, as for the controller's). A detail longer than the EVENT layout allows
 * (report::kEventMaxBytes) is not sent and the log says so.
 *
 * @param[in]     code           the event code (Interfaces Sec. 5.4, 0x0101 to 0x0106)
 * @param[in]     detail         the detail bytes
 * @param[in]     detail_length  number of detail bytes, 0 to report::kEventMaxBytes - 15
 * @return nothing
 */
void Updater::sendOwnEventDetail(uint16_t code, const uint8_t * detail, size_t detail_length)
{
  std::array<uint8_t, report::kEventMaxBytes> event{};
  if (detail_length > event.size() - report::kEventDetailOffset) {
    logging::line("EVENT 0x%04x not sent: detail of %zu bytes is too long", code, detail_length);
    return;
  }
  event[report::kOffsetKind] = report::kEvent;
  bytes::writeU16(&event[report::kOffsetEventCode], code);
  std::memcpy(&event[report::kEventDetailOffset], detail, detail_length);
  const size_t size = report::kEventDetailOffset + detail_length;
  if (!reports_.accept(event.data(), size, groundTimeStampS(), now_ms_, report_.data(),
        report_.size())) {
    logging::line("EVENT 0x%04x not sent: the report store refused it", code);
    return;
  }
  if (!gate_.haveGround()) {
    logging::line("EVENT 0x%04x not sent: no ground has spoken yet", code);
    return;
  }
  if (!gate_.sendReport(report_.data(), static_cast<uint16_t>(size))) {
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
  if (watch_.check(now_ms)) {
    // Decision 27 (owner, 15 Oct 2026; REQ-081): tell the ground the link was lost, once.
    sendOwnEvent(report::kEventDataLinkLost, report::kLinkIdDataLinkA);
  }
  silenceWatch();        // decision 20: reset a silent controller on schedule
  sendNext();
}
