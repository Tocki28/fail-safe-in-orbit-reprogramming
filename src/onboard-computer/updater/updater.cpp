#include "updater.hpp"

#include "bytes.hpp"
#include "clock.hpp"
#include "config.hpp"
#include "log.hpp"

namespace
{

// The onboard computer's own commands (the beat) answer to no ground command, so they carry
// no ground expiry. Bench: the largest value, "never", as test/controller/link_probe.py
// does. Open item: what expiry the onboard computer's own commands carry once the controller
// checks expiry (REQ-133, step 2b).
constexpr uint32_t kNoExpiry = 0xFFFFFFFFU;

// The checks on arrival for commands to the onboard computer: a command built on the bench,
// with the payload length its fields need (Interfaces Sec. 5.1).
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
    default:
      return false;
  }
}

uint16_t imageNumber(const frame::Frame & c)
{
  return bytes::readU16(&c.payload[command::kIdBytes]);
}

}  // namespace

Updater::Updater(CommandGate & gate, DataLink & link, UploadStore & store,
  const ImageChecker & checker, ReportStore & reports, ControllerWatch & watch, uint32_t beat_ms)
: gate_(gate), link_(link), store_(store), checker_(checker), reports_(reports), watch_(watch),
  beat_ms_(beat_ms), next_beat_ms_(0), now_ms_(0), sender_(Sender::kNone), reports_dropped_(0),
  queue_(), table_(), delivery_(), payload_{}, report_{}
{
}

// ---- Answers to the ground ---------------------------------------------------------------

void Updater::received(uint32_t sequence)
{
  (void)gate_.answer(sequence, answer::kReceived, answer::kReceivedCode);
}

void Updater::done(uint32_t sequence)
{
  (void)gate_.answer(sequence, answer::kDone, answer::kSuccess);
}

void Updater::refused(uint32_t sequence, uint8_t code)
{
  (void)gate_.answer(sequence, answer::kRefused, code);
}

// ---- From the ground ---------------------------------------------------------------------

void Updater::onGroundReadable(uint64_t now_ms)
{
  now_ms_ = now_ms;
  frame::Frame c{};
  if (gate_.receive(c)) {
    onGroundCommand(c);
  }
}

void Updater::onGroundCommand(const frame::Frame & c)
{
  if (c.destination == frame::kDestOnboard) {
    onOnboardCommand(c);
    return;
  }
  // For the controller: passed on in arrival order, when data link A is free (sendNext).
  if (!queue_.push(c)) {
    logging::line("ground: command sequence %u for the controller refused 0x05: %zu commands "
      "already wait for data link A", c.sequence, config::kForwardQueueSize);
    refused(c.sequence, answer::kNotInThisMode);
    return;
  }
  sendNext();
}

void Updater::onOnboardCommand(const frame::Frame & c)
{
  uint8_t id = (c.length > 0) ? c.payload[0] : 0;
  if (!arrivalOk(id, c.length)) {
    logging::line("ground: command 0x%02x with %u payload bytes is not one the onboard "
      "computer runs; refused 0x06", id, c.length);
    refused(c.sequence, answer::kImpossible);
    return;
  }
  received(c.sequence);
  switch (id) {
    case command::kUploadBegin:   uploadBegin(c);  break;
    case command::kUploadChunk:   uploadChunk(c);  break;
    case command::kUploadEnd:     uploadEnd(c);    break;
    case command::kUploadCancel:  uploadCancel(c); break;
    case command::kDeliver:       deliver(c);      break;
    case command::kGetStatus:     getStatus(c);    break;
    default:                      refused(c.sequence, answer::kImpossible); break;
  }
}

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

void Updater::uploadCancel(const frame::Frame & c)
{
  uint16_t number = imageNumber(c);
  (void)store_.cancel(number);
  logging::line("UPLOAD CANCEL image %u: stored bytes removed", number);
  done(c.sequence);
}

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

void Updater::finishDelivery(Delivery::Step step, uint8_t code)
{
  if (step == Delivery::Step::kDone) {
    logging::line("DELIVER image %u: done; the controller accepted UPLOAD END",
      delivery_.number());
    done(delivery_.groundSequence());
  } else {
    logging::line("DELIVER image %u: refused 0x%02x after %zu of %zu bytes",
      delivery_.number(), code, delivery_.sent(), delivery_.size());
    refused(delivery_.groundSequence(), code);
  }
  delivery_.stop();
}

// ---- From the controller -----------------------------------------------------------------

bool Updater::onLinkReadable(uint64_t listen_ms, uint64_t now_ms)
{
  now_ms_ = now_ms;
  return link_.receive(*this, listen_ms);
}

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
    case Sender::kBeat:
    case Sender::kNone:
      break;
  }
  if (final) {
    link_.release();
    sender_ = Sender::kNone;
  }
}

void Updater::onControllerReport(const frame::Frame & f)
{
  if (!reports_.accept(f.payload, f.length, clocks::onboardTimeS(), now_ms_, report_.data(),
        report_.size())) {
    return;
  }
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

// Order: the delivery first (its steps must not be split by other commands to the controller,
// e.g. a SWITCH while a slot is half written), then waiting ground commands, then the beat.
void Updater::sendNext()
{
  if (link_.busy()) {
    return;
  }
  uint32_t sequence = 0;
  uint32_t timeout_ms = config::kAnswerTimeoutMs;

  if (delivery_.active()) {
    uint16_t length = delivery_.next(payload_.data(), payload_.size(), timeout_ms);
    if (length == 0 || !link_.sendCommand(payload_.data(), length, delivery_.expiry(),
                         timeout_ms, now_ms_, sequence)) {
      logging::line("DELIVER image %u: data link A cannot send", delivery_.number());
      finishDelivery(Delivery::Step::kRefused, answer::kNoAnswerBench);
      return;
    }
    sender_ = Sender::kDelivery;
    return;
  }

  if (!queue_.empty()) {
    const ForwardedCommand & c = queue_.front();
    uint8_t id = (c.length > 0) ? c.payload[0] : 0;
    timeout_ms = (id == command::kUploadBegin) ? config::kBeginTimeoutMs : config::kAnswerTimeoutMs;
    if (link_.sendCommand(c.payload.data(), c.length, c.expiry, timeout_ms, now_ms_, sequence)) {
      table_.add(sequence, c.ground_sequence);
      sender_ = Sender::kGround;
      logging::line("ground command 0x%02x sequence %u passed on to the controller as "
        "sequence %u", id, c.ground_sequence, sequence);
    } else {
      logging::line("ground command 0x%02x sequence %u not passed on: data link A cannot send; "
        "the ground gets no answer", id, c.ground_sequence);
    }
    queue_.pop();
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
    case Sender::kBeat:
      logging::line("controller: no answer to GET STATUS sequence %u (beat) in time", sequence);
      break;
    case Sender::kNone:
      break;
  }
  sender_ = Sender::kNone;
}

void Updater::tick(uint64_t now_ms)
{
  now_ms_ = now_ms;
  if (link_.timedOut(now_ms)) {
    onTimeout();
  }
  watch_.check(now_ms);
  sendNext();
}
