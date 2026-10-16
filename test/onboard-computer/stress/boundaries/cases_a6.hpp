/*
 * cases_a6.hpp — Part A.6 of the stress audit: the repeat memories under eviction pressure.
 *
 * Question: can an old dangerous command run twice? A dangerous command S is run once with its
 * ARM (answered DONE). Then k other commands push S out of the memory that holds it (64 entries
 * for ARMs and dangerous commands, 256 for the rest; owner decision 26, config.hpp:127-153).
 * Then S is sent again with the same bytes, with its ARM (R1) or alone (R2), inside the Expiry
 * window or past it, and across an updater restart. A fake controller counts how often S is
 * passed on to it. Included once, by boundaries.cpp.
 */
#ifndef FSIR_STRESS_BOUNDARIES_CASES_A6_HPP
#define FSIR_STRESS_BOUNDARIES_CASES_A6_HPP

#include <array>
#include <string>
#include <vector>

#include "common.hpp"

namespace stress
{

// Ground sequence numbers of the ARM and of the dangerous command under test. Test choice.
constexpr uint32_t kA6ArmSeq = 1000;
constexpr uint32_t kA6CmdSeq = 1001;

// First ground sequence number of the filler commands; far from the two above. Test choice.
constexpr uint32_t kA6FillerBase = 0x00100000U;

// How far ahead of the system clock the Expiry of the tested pair is, seconds. Test choice:
// large against the few seconds a case takes, small against 2^32.
constexpr int64_t kExpiryAheadS = 600;

// The answers are read after this many filler commands, so that the test's UDP receive buffer
// cannot overflow (an overflow would lose answers, not commands). Test choice.
constexpr size_t kDrainEvery = 32;

// The system clock minus kT0 at which the Expiry of the tested pair is past, seconds: one second
// after it (updater.cpp:363, "lower than" the onboard ground time).
constexpr int64_t kPastAfterS = kExpiryAheadS + 1;

// Ground time sent in the tested SET ONBOARD TIME, and in a later "good" one, as seconds after
// kT0. Test choice: far enough from each other and from the clock to be told apart in the
// difference of EVENT 0x0105 (more than config::kClockDifferenceMaxS = 5 s).
constexpr int64_t kStaleTimeAheadS = 1000;
constexpr int64_t kGoodTimeAheadS = 2000;

enum class Subject : uint8_t
{
  kSwitch,          // SWITCH 0x20, a dangerous command of the controller
  kSetLimits,       // SET LIMITS 0x06, a dangerous command of the onboard computer
  kSetOnboardTime,  // SET ONBOARD TIME 0x09, onboard, exempt from the expiry check
};

enum class Filler : uint8_t
{
  kArms,              // k ARMs: one used-sequence entry each
  kPairs,             // k ARM + SWITCH pairs: two entries each
  kOthers,            // k GET STATUS to the onboard computer: repeat-memory entries
  kOthersController,  // k GET STATUS to the controller: repeat-memory entries, passed on
};

enum class Resend : uint8_t
{
  kArmAndCommand,   // R1: the ARM again, then the command, the same bytes
  kCommandOnly,     // R2: the command alone
};

// One run of the question.
struct A6Case
{
  Subject subject;
  Filler filler;
  size_t k;
  Resend resend;
  bool past;         // the resend comes after the Expiry
  bool restart;      // the updater is restarted between the fillers and the resend
  bool unanswered;   // the controller never answers the first SWITCH
};

// What a run found.
struct A6Result
{
  std::string first;         // answers to the first command
  std::string arm_resend;    // answers to the ARM sent again ("not sent" for R2)
  std::string cmd_resend;    // answers to the command sent again
  uint32_t passed;           // times the command was passed on after the fillers
  bool ran_again;            // the command ran a second time
  size_t refused_fillers;    // fillers that were refused (a setup error if not 0)
  size_t entries_after;      // used-sequence entries added after the command
};

/**
 * @brief Names the subject for a log line.
 *
 * @param[in]     s  the subject
 * @return "SWITCH", "SET LIMITS" or "SET ONBOARD TIME"
 */
inline const char * subjectName(Subject s)
{
  switch (s) {
    case Subject::kSwitch:         return "SWITCH";
    case Subject::kSetLimits:      return "SET LIMITS";
    case Subject::kSetOnboardTime: return "SET ONBOARD TIME";
  }
  std::_Exit(2);   // a value that is none of the three: corrupted memory (code rule B1)
}

/**
 * @brief Names the filler for a log line.
 *
 * @param[in]     f  the filler
 * @return a short name
 */
inline const char * fillerName(Filler f)
{
  switch (f) {
    case Filler::kArms:             return "ARMs";
    case Filler::kPairs:            return "ARM+SWITCH pairs";
    case Filler::kOthers:           return "GET STATUS to onboard";
    case Filler::kOthersController: return "GET STATUS to controller";
  }
  std::_Exit(2);   // a value that is none of the four: corrupted memory (code rule B1)
}

/**
 * @brief Gives the destination of the subject's ARM and command.
 *
 * @param[in]     s  the subject
 * @return frame::kDestController for SWITCH, frame::kDestOnboard for the other two
 */
inline uint8_t subjectDestination(Subject s)
{
  return (s == Subject::kSwitch) ? frame::kDestController : frame::kDestOnboard;
}

/**
 * @brief Gives the command identifier of the subject.
 *
 * @param[in]     s  the subject
 * @return 0x20, 0x06 or 0x09
 */
inline uint8_t subjectId(Subject s)
{
  switch (s) {
    case Subject::kSwitch:         return command::kSwitch;
    case Subject::kSetLimits:      return command::kSetLimits;
    case Subject::kSetOnboardTime: return command::kSetOnboardTime;
  }
  std::_Exit(2);   // a value that is none of the three: corrupted memory (code rule B1)
}

/**
 * @brief Gives the payload of the subject's command.
 *
 * @param[in]     s  the subject
 * @return the command's payload bytes
 */
inline std::vector<uint8_t> subjectPayload(Subject s)
{
  switch (s) {
    case Subject::kSwitch:         return {command::kSwitch};
    case Subject::kSetLimits:      return limitsPayload(kLimitsVersion, kLimitsIdentity);
    case Subject::kSetOnboardTime:
      return timePayload(static_cast<uint32_t>(kT0 + kStaleTimeAheadS));
  }
  std::_Exit(2);   // a value that is none of the three: corrupted memory (code rule B1)
}

/**
 * @brief Counts the refusals among answers.
 *
 * @param[in]     answers  what the ground received
 * @return the number of REFUSED answers
 */
inline size_t countRefused(const std::vector<GroundAnswer> & answers)
{
  size_t n = 0;
  for (const GroundAnswer & a : answers) {
    n += (a.kind == answer::kRefused) ? 1U : 0U;
  }
  return n;
}

/**
 * @brief Sends k filler commands, each under a new ground sequence number.
 *
 * Bounded by k. The answers are read every kDrainEvery commands and refusals are counted.
 *
 * @param[in,out] rig      the rig
 * @param[in]     filler   which kind of command
 * @param[in]     k        how many (pairs for kPairs)
 * @param[out]    refused  how many fillers were refused
 * @return the number of used-sequence entries the fillers added (ARMs 1, pairs 2, others 0)
 */
inline size_t runFiller(Rig & rig, Filler filler, size_t k, size_t & refused)
{
  refused = 0;
  size_t entries = 0;
  uint32_t seq = kA6FillerBase;
  rig.discard();
  for (size_t i = 0; i < k; i++) {
    bool sent = true;
    switch (filler) {
      case Filler::kArms:
        sent = rig.ground(frame::kDestController, seq,
          armPayload(command::kSwitch, seq + 0x1000000U), kFarExpiry);
        seq++;
        entries += 1;
        break;
      case Filler::kPairs:
        sent = rig.ground(frame::kDestController, seq, armPayload(command::kSwitch, seq + 1U),
          kFarExpiry) && rig.ground(frame::kDestController, seq + 1U, {command::kSwitch},
          kFarExpiry);
        seq += 2U;
        entries += 2;
        break;
      case Filler::kOthers:
        sent = rig.ground(frame::kDestOnboard, seq++, {command::kGetStatus}, kFarExpiry);
        break;
      case Filler::kOthersController:
        sent = rig.ground(frame::kDestController, seq++, {command::kGetStatus}, kFarExpiry);
        break;
    }
    if (!sent) {
      std::printf("SETUP    filler %zu did not reach the gate\n", i);
      (void)std::fflush(stdout);
      std::_Exit(2);
    }
    if ((i + 1) % kDrainEvery == 0 || i + 1 == k) {
      refused += countRefused(rig.answers());
    }
  }
  return entries;
}

/**
 * @brief Names what stopped (or did not stop) a command sent again.
 *
 * @param[in]     answers  what the ground received for the command
 * @param[in]     seq      the command's ground sequence number
 * @param[in]     ran      the command ran again
 * @return a short text: the answer that stopped it, or "NOT STOPPED"
 */
inline std::string stoppedBy(const std::vector<GroundAnswer> & answers, uint32_t seq, bool ran)
{
  if (ran) {
    return "NOT STOPPED";
  }
  if (finalIs(answers, seq, answer::kAlreadyDone, 0)) {
    return "ALREADY DONE (used-sequence memory)";
  }
  for (const GroundAnswer & a : answers) {
    if (a.answered == seq && a.kind == answer::kRefused) {
      if (a.code == answer::kNotArmed) {
        return "REFUSED 0x01 (no ARM held for it)";
      }
      if (a.code == answer::kArmExpired) {
        return "REFUSED 0x02 (ARM expired)";
      }
      if (a.code == answer::kPastExpiry) {
        return "REFUSED 0x03 (Expiry past)";
      }
      if (a.code == answer::kOldCommand) {
        return "REFUSED 0x04 (old command)";
      }
      return fmt("REFUSED 0x%02x", a.code);
    }
  }
  return hasKind(answers, seq, answer::kReceived) ? "RECEIVED only (entry pending)" : "no answer";
}

/**
 * @brief Runs the first command, the fillers and the resend of one case, and reads the result.
 *
 * Steps: ARM (seq 1000) and the command (seq 1001) with Expiry kT0 + 600 s; the fillers; an
 * optional restart of the updater on the same data directory; an optional move of the system
 * clock past the Expiry; the resend. "Ran again" is: SWITCH passed on to the controller at least
 * once more; for the two onboard subjects, a final DONE.
 *
 * @param[in]     c     the case
 * @param[in]     name  a unique name for the data directory
 * @return what the run found
 */
inline A6Result runA6(const A6Case & c, const std::string & name)
{
  stress_clock::setSystemTimeS(kT0);
  const uint32_t expiry = static_cast<uint32_t>(kT0 + kExpiryAheadS);
  const std::string dir = freshDir(name);
  auto rig = makeRig(dir);
  const uint8_t dest = subjectDestination(c.subject);
  const std::vector<uint8_t> arm = armPayload(subjectId(c.subject), kA6CmdSeq);
  const std::vector<uint8_t> cmd = subjectPayload(c.subject);
  A6Result res{};
  (void)step(*rig, dest, kA6ArmSeq, arm, expiry);
  if (c.unanswered) {
    rig->ignoreOnce(command::kSwitch);
  }
  const std::vector<GroundAnswer> first = step(*rig, dest, kA6CmdSeq, cmd, expiry);
  if (c.unanswered) {
    rig->advance(config::kAnswerTimeoutMs + 1U);
    rig->tick();
  }
  res.first = show(first, kA6CmdSeq);
  res.entries_after = runFiller(*rig, c.filler, c.k, res.refused_fillers);
  if (c.filler == Filler::kPairs) {
    res.entries_after = 2 * c.k;
  }
  if (c.restart) {
    rig.reset();
    rig = makeRig(dir);
  }
  if (c.past) {
    stress_clock::setSystemTimeS(kT0 + kPastAfterS);
  }
  rig->clearCounts();
  if (c.resend == Resend::kArmAndCommand) {
    res.arm_resend = show(step(*rig, dest, kA6ArmSeq, arm, expiry), kA6ArmSeq);
  } else {
    res.arm_resend = "not sent";
  }
  const std::vector<GroundAnswer> again = step(*rig, dest, kA6CmdSeq, cmd, expiry);
  res.cmd_resend = show(again, kA6CmdSeq) + " -> " + stoppedBy(again, kA6CmdSeq, false);
  if (c.subject == Subject::kSwitch) {
    res.passed = rig->passed(command::kSwitch);
    res.ran_again = res.passed > 0;
  } else {
    res.passed = finalIs(again, kA6CmdSeq, answer::kDone, 0) ? 1U : 0U;
    res.ran_again = res.passed > 0;
  }
  res.cmd_resend = show(again, kA6CmdSeq) + " -> " + stoppedBy(again, kA6CmdSeq, res.ran_again);
  return res;
}

/**
 * @brief Prints the result of one run as one line, OK if the command did not run again.
 *
 * A command that ran again is a FINDING. The line says whether the command was still inside the
 * 64 entries of the memory (the documented protection), beyond them (the documented limit,
 * config.hpp:127-132, decision 26), or after a restart (decision 26: memories not kept).
 *
 * @param[in]     id   the check's name
 * @param[in]     c    the case
 * @param[in]     res  what the run found
 * @return nothing
 */
inline void emitA6(const std::string & id, const A6Case & c, const A6Result & res)
{
  const bool memory_is_used_ring = (c.filler == Filler::kArms || c.filler == Filler::kPairs);
  std::string where;
  if (res.ran_again) {
    if (c.restart) {
      where = " [after a restart: memories are not kept, decision 26 accepted risk]";
    } else if (memory_is_used_ring && res.entries_after < config::kUsedSequenceCount) {
      where = " [INSIDE the 64 entries: within the documented protection]";
    } else {
      where = " [beyond the memory: the documented limit, config.hpp:127-132, decision 26]";
    }
  }
  const std::string what = fmt("%s; %zu %s (%zu used-sequence entries after it); %s; Expiry %s%s%s",
    subjectName(c.subject), c.k, fillerName(c.filler), res.entries_after,
    c.resend == Resend::kArmAndCommand ? "ARM+command sent again" : "command alone sent again",
    c.past ? "past" : "in the future", c.restart ? "; updater restarted" : "",
    c.unanswered ? "; first SWITCH unanswered" : "");
  emit(res.ran_again ? "FINDING" : "OK", id, what,
    "the command is not passed on or run a second time [REQ-065, REQ-031; updater.hpp:20-28]",
    fmt("first: %s | ARM sent again: %s | command sent again: %s | passed on / ran: %u%s%s",
      res.first.c_str(), res.arm_resend.c_str(), res.cmd_resend.c_str(), res.passed,
      res.refused_fillers > 0 ? " | SETUP: some fillers were refused" : "", where.c_str()));
}

/**
 * @brief Runs the eviction question over a list of filler counts, both resends, both Expiry cases.
 *
 * @param[in]     tag      prefix of the check names
 * @param[in]     subject  the dangerous command under test
 * @param[in]     filler   the kind of filler
 * @param[in]     counts   the filler counts k
 * @return nothing
 */
inline void sweepA6(const std::string & tag, Subject subject, Filler filler,
  const std::vector<size_t> & counts)
{
  for (const size_t k : counts) {
    for (const Resend resend : {Resend::kArmAndCommand, Resend::kCommandOnly}) {
      for (const bool past : {false, true}) {
        const A6Case c{subject, filler, k, resend, past, false, false};
        const std::string id = fmt("%s/k%zu-%s-%s", tag.c_str(), k,
          resend == Resend::kArmAndCommand ? "R1" : "R2", past ? "past" : "future");
        emitA6(id, c, runA6(c, id));
      }
    }
  }
}

/**
 * @brief k ARMs after the dangerous command, k = 62, 63, 64, 65, 100, 1000.
 *
 * Each ARM adds one entry to the 64-entry used-sequence memory. S and its own ARM are two
 * entries; 62 later ARMs fill the ring exactly.
 *
 * @return nothing
 */
inline void caseA6ArmFillers()
{
  sweepA6("a6-arm-fillers", Subject::kSwitch, Filler::kArms, {62, 63, 64, 65, 100, 1000});
}

/**
 * @brief k ARM + SWITCH pairs after the dangerous command; k = 31, 32, 33, 63, 64, 65, 100, 1000.
 *
 * Each pair adds two entries. 31 pairs fill the ring exactly (62 entries after S).
 *
 * @return nothing
 */
inline void caseA6PairFillers()
{
  sweepA6("a6-pair-fillers", Subject::kSwitch, Filler::kPairs, {31, 32, 33, 63, 64, 65, 100, 1000});
}

/**
 * @brief k ordinary commands after the dangerous command, k = 255, 256, 257, 1000.
 *
 * Ordinary commands go to the other memory (256 entries) and must not push the dangerous command
 * out of the 64-entry one (decision 26: "upload traffic cannot push the dangerous commands out").
 * Fillers go to the onboard computer, and for k = 1000 also to the controller.
 *
 * @return nothing
 */
inline void caseA6OtherFillers()
{
  sweepA6("a6-other-fillers", Subject::kSwitch, Filler::kOthers, {255, 256, 257, 1000});
  sweepA6("a6-other-fillers-ctrl", Subject::kSwitch, Filler::kOthersController, {1000});
}

/**
 * @brief Resend across an updater restart, with no filler at all (decision 26: not kept).
 *
 * @return nothing
 */
inline void caseA6Restart()
{
  for (const Subject subject : {Subject::kSwitch, Subject::kSetLimits,
                                Subject::kSetOnboardTime}) {
    for (const Resend resend : {Resend::kArmAndCommand, Resend::kCommandOnly}) {
      for (const bool past : {false, true}) {
        const A6Case c{subject, Filler::kArms, 0, resend, past, true, false};
        const std::string id = fmt("a6-restart/%s-%s-%s", subjectName(subject),
          resend == Resend::kArmAndCommand ? "R1" : "R2", past ? "past" : "future");
        emitA6(id, c, runA6(c, id));
      }
    }
  }
}

/**
 * @brief The onboard dangerous commands as subjects, then a stale SET ONBOARD TIME replayed.
 *
 * SET LIMITS and SET ONBOARD TIME with k ARMs in between (62, 63, 64, 1000), both Expiry cases,
 * R1 only. SET ONBOARD TIME is exempt from the expiry check (updater.cpp:77-90), so a past
 * Expiry does not stop its replay. Then: SET ONBOARD TIME to kT0 + 1000, a later SET ONBOARD TIME
 * to kT0 + 2000, 64 ARMs, the first one replayed with Expiry 1; EVENT 0x0105 shows the clock
 * going back.
 *
 * @return nothing
 */
inline void caseA6OnboardSubjects()
{
  for (const Subject subject : {Subject::kSetLimits, Subject::kSetOnboardTime}) {
    for (const size_t k : std::vector<size_t>{62, 63, 64, 1000}) {
      for (const bool past : {false, true}) {
        const A6Case c{subject, Filler::kArms, k, Resend::kArmAndCommand, past, false, false};
        const std::string id = fmt("a6-onboard/%s-k%zu-R1-%s", subjectName(subject), k,
          past ? "past" : "future");
        emitA6(id, c, runA6(c, id));
      }
    }
  }
  stress_clock::setSystemTimeS(kT0);
  auto rig = makeRig(freshDir("a6-onboard-replay"));
  Rig & r = *rig;
  const uint32_t stale = static_cast<uint32_t>(kT0 + kStaleTimeAheadS);
  const uint32_t good = static_cast<uint32_t>(kT0 + kGoodTimeAheadS);
  (void)step(r, frame::kDestOnboard, 2000, armPayload(command::kSetOnboardTime, 2001), 1U);
  (void)step(r, frame::kDestOnboard, 2001, timePayload(stale), 1U);
  (void)step(r, frame::kDestOnboard, 2002, armPayload(command::kSetOnboardTime, 2003), 1U);
  (void)step(r, frame::kDestOnboard, 2003, timePayload(good), 1U);
  (void)r.takeReports();
  size_t refused = 0;
  (void)runFiller(r, Filler::kArms, config::kUsedSequenceCount, refused);
  (void)step(r, frame::kDestOnboard, 2000, armPayload(command::kSetOnboardTime, 2001), 1U);
  const std::vector<GroundAnswer> replay = step(r, frame::kDestOnboard, 2001, timePayload(stale),
    1U);
  int32_t detail = 0;
  const bool seen = clockEventDetail(r.takeReports(), detail);
  const bool ran = finalIs(replay, 2001, answer::kDone, 0);
  emit(ran ? "FINDING" : "OK", "a6-onboard/replay-stale-time",
    "SET ONBOARD TIME kT0+1000 (seq 2001), a later one kT0+2000, 64 ARMs, then the first again "
    "with its ARM, Expiry 1",
    "the old command is refused [REQ-065; updater.hpp:20-28]",
    fmt("answer: %s | EVENT 0x0105 difference: %s", show(replay, 2001).c_str(),
      seen ? fmt("%d s (onboard ground time minus the replayed ground time)", detail).c_str()
           : "no event"));
}

/**
 * @brief A SWITCH the controller never answered, then eviction and a resend.
 *
 * The first SWITCH gets no answer, so the updater's entry stays pending (updater.cpp:1395-1400,
 * onTimeout does not finish it). k = 0 (resend at once), 62, 63, 64 ARMs between.
 *
 * @return nothing
 */
inline void caseA6Unanswered()
{
  for (const size_t k : std::vector<size_t>{0, 62, 63, 64}) {
    for (const Resend resend : {Resend::kArmAndCommand, Resend::kCommandOnly}) {
      const A6Case c{Subject::kSwitch, Filler::kArms, k, resend, false, false, true};
      const std::string id = fmt("a6-unanswered/k%zu-%s", k,
        resend == Resend::kArmAndCommand ? "R1" : "R2");
      emitA6(id, c, runA6(c, id));
    }
  }
}

/**
 * @brief The two memories against each other (a number held by the other kind of command).
 *
 * I1: S evicted from the 64 by 64 ARMs; an ordinary GET STATUS then takes number S (new for the
 * repeat memory); S and its ARM are sent again: the cross-memory check should refuse 0x04.
 * I2: S still in the 64; an ordinary command under number S: REFUSED 0x04 (both destinations).
 * I3: an ordinary command G in the 256; an ARM under number G: REFUSED 0x04; after 256 other
 * ordinary commands push G out, an ARM under number G is accepted, and then an ordinary command
 * under G is refused 0x04 by the used-sequence memory.
 *
 * @return nothing
 */
inline void caseA6Interplay()
{
  stress_clock::setSystemTimeS(kT0);
  const uint32_t expiry = static_cast<uint32_t>(kT0 + kExpiryAheadS);
  const std::string src = "updater.cpp:540-575 (answerRepeat, other memory); updater.hpp:25-26";
  {
    auto rig = makeRig(freshDir("a6-interplay-1"));
    Rig & r = *rig;
    (void)step(r, frame::kDestController, kA6ArmSeq, armPayload(command::kSwitch, kA6CmdSeq),
      expiry);
    (void)step(r, frame::kDestController, kA6CmdSeq, {command::kSwitch}, expiry);
    size_t refused = 0;
    (void)runFiller(r, Filler::kArms, config::kUsedSequenceCount, refused);
    check("a6-interplay/I1-ordinary-takes-S", "after S was evicted, GET STATUS under number S",
      "RECEIVED,DONE",
      show(step(r, frame::kDestOnboard, kA6CmdSeq, {command::kGetStatus}, expiry), kA6CmdSeq),
      src);
    r.clearCounts();
    const std::string arm = show(step(r, frame::kDestController, kA6ArmSeq,
      armPayload(command::kSwitch, kA6CmdSeq), expiry), kA6ArmSeq);
    const std::vector<GroundAnswer> sw = step(r, frame::kDestController, kA6CmdSeq,
      {command::kSwitch}, expiry);
    emit(r.passed(command::kSwitch) > 0 ? "FINDING" : "OK", "a6-interplay/I1-resend",
      "ARM + SWITCH sent again after an ordinary command took number S",
      "REFUSED 0x04 by the cross-memory check, SWITCH not passed on [updater.cpp:540-575]",
      fmt("ARM: %s | SWITCH: %s | SWITCH passed on: %u", arm.c_str(),
        show(sw, kA6CmdSeq).c_str(), r.passed(command::kSwitch)));
  }
  {
    auto rig = makeRig(freshDir("a6-interplay-2"));
    Rig & r = *rig;
    (void)step(r, frame::kDestController, kA6ArmSeq, armPayload(command::kSwitch, kA6CmdSeq),
      expiry);
    (void)step(r, frame::kDestController, kA6CmdSeq, {command::kSwitch}, expiry);
    check("a6-interplay/I2-onboard", "S in the 64; GET STATUS to the onboard computer under S",
      "REFUSED(0x04)", show(step(r, frame::kDestOnboard, kA6CmdSeq, {command::kGetStatus}, expiry),
        kA6CmdSeq), src);
    check("a6-interplay/I2-controller", "S in the 64; GET STATUS to the controller under S",
      "REFUSED(0x04)", show(step(r, frame::kDestController, kA6CmdSeq, {command::kGetStatus},
        expiry), kA6CmdSeq), src);
  }
  {
    auto rig = makeRig(freshDir("a6-interplay-3"));
    Rig & r = *rig;
    constexpr uint32_t kG = 3000;   // test choice
    (void)step(r, frame::kDestOnboard, kG, {command::kGetStatus}, expiry);
    check("a6-interplay/I3-arm-under-G", "G in the 256; an ARM under number G", "REFUSED(0x04)",
      show(step(r, frame::kDestController, kG, armPayload(command::kSwitch, kG + 1U), expiry), kG),
      src);
    size_t refused = 0;
    (void)runFiller(r, Filler::kOthers, config::kRepeatCount, refused);
    check("a6-interplay/I3-arm-after-evict", "G pushed out of the 256 by 256 ordinary commands; "
      "an ARM under number G", "RECEIVED,DONE", show(step(r, frame::kDestController, kG,
        armPayload(command::kSwitch, kG + 1U), expiry), kG), src);
    check("a6-interplay/I3-ordinary-after-arm", "an ordinary command under number G, now held by "
      "the 64", "REFUSED(0x04)", show(step(r, frame::kDestOnboard, kG, {command::kGetStatus},
        expiry), kG), src);
  }
}

/**
 * @brief The 256-entry repeat memory's own boundary, and a non-dangerous controller command.
 *
 * GET STATUS (onboard) run once, then k other ordinary commands, k = 254, 255, 256, 257, then
 * sent again: ALREADY DONE while fewer than 256 later entries exist, run again from 256
 * (decision 26: 256 entries). Also LEAVE FALLBACK (0x23, not dangerous, Interfaces Sec. 5.1) to
 * the controller with k = 255, 256, 1000: after eviction it is passed on a second time.
 *
 * @return nothing
 */
inline void caseA6RepeatEviction()
{
  stress_clock::setSystemTimeS(kT0);
  const uint32_t expiry = static_cast<uint32_t>(kT0 + kExpiryAheadS);
  const std::string src = "decision 26; config.hpp:145-153";
  constexpr uint32_t kSubject = 4000;   // test choice
  for (const size_t k : std::vector<size_t>{254, 255, 256, 257}) {
    auto rig = makeRig(freshDir(fmt("a6-repeat-%zu", k)));
    Rig & r = *rig;
    (void)step(*rig, frame::kDestOnboard, kSubject, {command::kGetStatus}, expiry);
    size_t refused = 0;
    (void)runFiller(r, Filler::kOthers, k, refused);
    const std::string want = (k < config::kRepeatCount) ? "ALREADY_DONE" : "RECEIVED,DONE";
    check(fmt("a6-repeat-eviction/get-status-k%zu", k),
      fmt("GET STATUS, %zu ordinary commands later, sent again", k), want,
      show(step(r, frame::kDestOnboard, kSubject, {command::kGetStatus}, expiry), kSubject), src);
  }
  for (const size_t k : std::vector<size_t>{255, 256, 1000}) {
    auto rig = makeRig(freshDir(fmt("a6-repeat-fallback-%zu", k)));
    Rig & r = *rig;
    (void)step(r, frame::kDestController, kSubject, {command::kLeaveFallback}, expiry);
    size_t refused = 0;
    (void)runFiller(r, Filler::kOthers, k, refused);
    r.clearCounts();
    (void)step(r, frame::kDestController, kSubject, {command::kLeaveFallback}, expiry);
    const uint32_t passed = r.passed(command::kLeaveFallback);
    const std::string want = (k < config::kRepeatCount) ? "0" : "1";
    emit(fmt("%u", passed) == want ? "OK" : "FINDING",
      fmt("a6-repeat-eviction/leave-fallback-k%zu", k),
      fmt("LEAVE FALLBACK (not dangerous), %zu ordinary commands later, sent again", k),
      fmt("passed on %s time(s) (decision 26: forgotten from 256 later entries on) [%s]",
        want.c_str(), src.c_str()), fmt("passed on %u time(s)", passed));
  }
}

}  // namespace stress

#endif  // FSIR_STRESS_BOUNDARIES_CASES_A6_HPP
