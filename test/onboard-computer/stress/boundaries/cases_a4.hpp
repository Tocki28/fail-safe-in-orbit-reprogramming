/*
 * cases_a4.hpp — Part A.4 of the stress audit: wrap and boundary cases of the updater.
 *
 * Sequence numbers near 2^32, the clock offset at its int64 limits, an Expiry on the onboard
 * ground time, the ARM at 30 s, the trial watch at 150 s, the silence watch at 5 s and the reset
 * gap at 60 s. Included once, by boundaries.cpp.
 */
#ifndef FSIR_STRESS_BOUNDARIES_CASES_A4_HPP
#define FSIR_STRESS_BOUNDARIES_CASES_A4_HPP

#include <array>
#include <limits>
#include <string>
#include <vector>

#include "common.hpp"
#include "controller_watch.hpp"
#include "trial_watch.hpp"

namespace stress
{

// First sequence number of the filler commands in the sequence cases. Test choice.
constexpr uint32_t kCaseBase = 1000;

// The sequence numbers the data link hands out from a start of 0xFFFFFFFC: up to 2^32 - 1, then
// 1 (data_link.cpp:81-82, data_link.hpp:28 "1 after 2^32 - 1"; Interfaces Sec. 4.0: 1 to 2^32-1).
// Index 0 is 0xFFFFFFFC, the start the wrap build sets (Makefile, WRAP_START).
constexpr uint32_t kWrapStart = 0xFFFFFFFCU;

/**
 * @brief Gives the data-link sequence number the n-th frame carries in the wrap build.
 *
 * @param[in]     n  0 for the first frame sent
 * @return kWrapStart + n, going from 0xFFFFFFFF to 1
 */
inline uint32_t wrapLink(uint32_t n)
{
  const uint32_t to_top = UINT32_MAX - kWrapStart;   // frames after the first before the top
  return (n <= to_top) ? kWrapStart + n : n - to_top;
}

/**
 * @brief Ground sequence numbers 0xFFFFFFFE, 0xFFFFFFFF, 0 and 1 through the used-sequence memory.
 *
 * ARM and SET LIMITS to the onboard computer, and ARM and SWITCH to the controller, under numbers
 * on both sides of 2^32 - 1. Each is run, sent again with the same bytes (ALREADY DONE), and
 * sent again with other bytes (REFUSED 0x04). The Interfaces table gives sequence numbers 1 to
 * 2^32 - 1 (main.tex:224); 0 is outside it and its handling is not written.
 *
 * @return nothing
 */
inline void caseSeqUsedWrap()
{
  stress_clock::setSystemTimeS(kT0);
  auto rig = makeRig(freshDir("seq-used-wrap"));
  Rig & r = *rig;
  const std::string src = "updater.hpp:136-142; command_gate.hpp:42-47; REQ-031, REQ-065";
  struct Pair
  {
    uint32_t arm;
    uint32_t cmd;
  };
  const std::array<Pair, 2> pairs = {{{0xFFFFFFFEU, 0xFFFFFFFFU}, {0U, 1U}}};
  for (const Pair & p : pairs) {
    const std::string id = fmt("seq-used-wrap/onboard-%08x-%08x", p.arm, p.cmd);
    const std::vector<uint8_t> arm = armPayload(command::kSetLimits, p.cmd);
    const std::vector<uint8_t> cmd = limitsPayload(kLimitsVersion, kLimitsIdentity);
    check(id + "/arm", "ARM naming SET LIMITS", "RECEIVED,DONE",
      show(step(r, frame::kDestOnboard, p.arm, arm, kFarExpiry), p.arm), src);
    check(id + "/cmd", "SET LIMITS with the ARM", "DONE",
      show(step(r, frame::kDestOnboard, p.cmd, cmd, kFarExpiry), p.cmd), src);
    check(id + "/arm-again", "same ARM bytes again", "ALREADY_DONE",
      show(step(r, frame::kDestOnboard, p.arm, arm, kFarExpiry), p.arm), src);
    check(id + "/cmd-again", "same SET LIMITS bytes again", "ALREADY_DONE",
      show(step(r, frame::kDestOnboard, p.cmd, cmd, kFarExpiry), p.cmd), src);
    check(id + "/cmd-other", "SET LIMITS under the same number, other bytes", "REFUSED(0x04)",
      show(step(r, frame::kDestOnboard, p.cmd,
        limitsPayload(kLimitsVersion + 1U, kLimitsIdentity), kFarExpiry), p.cmd), src);
  }
  emit("NOTE", "seq-used-wrap/ground-sequence-0",
    "ground sequence number 0 (ARM and SET LIMITS above were accepted under 0 and 1)",
    "expected behaviour not specified: Interfaces Sec. 4.0 (main.tex:224) gives sequence numbers "
    "1 to 2^32-1 and \"one higher than the sender's last frame\"; no source says how the onboard "
    "computer treats 0, or the step 0xFFFFFFFF to 0 or to 1",
    "accepted and remembered like any other number; no check that a number is one higher than "
    "the last (command_gate.cpp:108-135 receive() does not look at the sequence)");
  // The controller's side: ARM and SWITCH under 0xFFFFFFFC and 0xFFFFFFFD; SWITCH goes on once.
  const uint32_t arm_seq = 0xFFFFFFFCU;
  const uint32_t cmd_seq = 0xFFFFFFFDU;
  r.clearCounts();
  (void)step(r, frame::kDestController, arm_seq, armPayload(command::kSwitch, cmd_seq),
    kFarExpiry);
  const std::vector<GroundAnswer> first = step(r, frame::kDestController, cmd_seq,
    {command::kSwitch}, kFarExpiry);
  check("seq-used-wrap/controller-first", "SWITCH under 0xFFFFFFFD with its ARM", "DONE",
    show(first, cmd_seq), src);
  const std::vector<GroundAnswer> again = step(r, frame::kDestController, cmd_seq,
    {command::kSwitch}, kFarExpiry);
  check("seq-used-wrap/controller-again", "SWITCH again, same bytes", "ALREADY_DONE",
    show(again, cmd_seq), src);
  check("seq-used-wrap/controller-passed", "SWITCH frames passed on to the controller", "1",
    fmt("%u", r.passed(command::kSwitch)), src);
}

/**
 * @brief The 64-entry used-sequence ring with numbers that cross 2^32 - 1.
 *
 * 64 ARMs under 0xFFFFFFE0 to 0x0000001F fill the ring exactly; all 64 are recognised when sent
 * again. A 65th ARM (0x20) pushes out the oldest (0xFFFFFFE0) and nothing else. Source: the ring
 * is "the last 64" (config.hpp:127-131).
 *
 * @return nothing
 */
inline void caseSeqUsedRingWrap()
{
  stress_clock::setSystemTimeS(kT0);
  auto rig = makeRig(freshDir("seq-used-ring-wrap"));
  Rig & r = *rig;
  const std::string src = "config.hpp:127-131 (the last 64); command_gate.hpp:60-66";
  constexpr uint32_t kFirst = 0xFFFFFFE0U;
  constexpr size_t kRing = config::kUsedSequenceCount;
  const std::vector<uint8_t> arm = armPayload(command::kSetLimits, kCaseBase);
  size_t first_ok = 0;
  for (size_t i = 0; i < kRing; i++) {
    const uint32_t s = kFirst + static_cast<uint32_t>(i);
    first_ok += (show(step(r, frame::kDestOnboard, s, arm, kFarExpiry), s) == "RECEIVED,DONE")
                  ? 1U : 0U;
  }
  check("seq-used-ring-wrap/fill", "64 ARMs under 0xFFFFFFE0..0x0000001F accepted", "64",
    fmt("%zu", first_ok), src);
  size_t known = 0;
  for (size_t i = 0; i < kRing; i++) {
    const uint32_t s = kFirst + static_cast<uint32_t>(i);
    known += (show(step(r, frame::kDestOnboard, s, arm, kFarExpiry), s) == "ALREADY_DONE")
               ? 1U : 0U;
  }
  check("seq-used-ring-wrap/all-known", "all 64 sent again", "64 ALREADY_DONE",
    fmt("%zu ALREADY_DONE", known), src);
  const uint32_t extra = kFirst + static_cast<uint32_t>(kRing);   // 0x00000020
  check("seq-used-ring-wrap/65th", "a 65th ARM, number 0x00000020", "RECEIVED,DONE",
    show(step(r, frame::kDestOnboard, extra, arm, kFarExpiry), extra), src);
  size_t still_known = 0;
  for (size_t i = 1; i < kRing; i++) {
    const uint32_t s = kFirst + static_cast<uint32_t>(i);
    still_known += (show(step(r, frame::kDestOnboard, s, arm, kFarExpiry), s) == "ALREADY_DONE")
                     ? 1U : 0U;
  }
  check("seq-used-ring-wrap/others-kept", "entries 1..63 after the 65th", "63 ALREADY_DONE",
    fmt("%zu ALREADY_DONE", still_known), src);
  const std::string oldest = show(step(r, frame::kDestOnboard, kFirst, arm, kFarExpiry), kFirst);
  check("seq-used-ring-wrap/oldest-gone", "the oldest (0xFFFFFFE0) sent again after the 65th",
    "RECEIVED,DONE (forgotten: a new ARM)", oldest + (oldest == "RECEIVED,DONE" ?
      " (forgotten: a new ARM)" : ""), src);
}

/**
 * @brief The 256-entry repeat memory with ordinary commands whose numbers cross 2^32 - 1.
 *
 * 256 GET STATUS commands under 0xFFFFFF80 to 0x0000007F fill the memory; all 256 are answered
 * ALREADY DONE when sent again; the 257th (0x80) pushes out only the oldest. Source: decision 26
 * (256 for the rest), config.hpp:145-153.
 *
 * @return nothing
 */
inline void caseSeqRepeatWrap()
{
  stress_clock::setSystemTimeS(kT0);
  auto rig = makeRig(freshDir("seq-repeat-wrap"));
  Rig & r = *rig;
  const std::string src = "decision 26; config.hpp:145-153; REQ-031, REQ-065";
  constexpr uint32_t kFirst = 0xFFFFFF80U;
  constexpr size_t kRing = config::kRepeatCount;
  const std::vector<uint8_t> status = {command::kGetStatus};
  size_t ran = 0;
  for (size_t i = 0; i < kRing; i++) {
    const uint32_t s = kFirst + static_cast<uint32_t>(i);
    ran += (show(step(r, frame::kDestOnboard, s, status, kFarExpiry), s) == "RECEIVED,DONE")
             ? 1U : 0U;
  }
  check("seq-repeat-wrap/fill", "256 GET STATUS under 0xFFFFFF80..0x0000007F", "256",
    fmt("%zu", ran), src);
  size_t known = 0;
  for (size_t i = 0; i < kRing; i++) {
    const uint32_t s = kFirst + static_cast<uint32_t>(i);
    known += (show(step(r, frame::kDestOnboard, s, status, kFarExpiry), s) == "ALREADY_DONE")
               ? 1U : 0U;
  }
  check("seq-repeat-wrap/all-known", "all 256 sent again", "256 ALREADY_DONE",
    fmt("%zu ALREADY_DONE", known), src);
  const uint32_t extra = kFirst + static_cast<uint32_t>(kRing);   // 0x00000080
  check("seq-repeat-wrap/257th", "a 257th GET STATUS, number 0x00000080", "RECEIVED,DONE",
    show(step(r, frame::kDestOnboard, extra, status, kFarExpiry), extra), src);
  size_t kept = 0;
  for (size_t i = 1; i < kRing; i++) {
    const uint32_t s = kFirst + static_cast<uint32_t>(i);
    kept += (show(step(r, frame::kDestOnboard, s, status, kFarExpiry), s) == "ALREADY_DONE")
              ? 1U : 0U;
  }
  check("seq-repeat-wrap/others-kept", "entries 1..255 after the 257th", "255 ALREADY_DONE",
    fmt("%zu ALREADY_DONE", kept), src);
  const std::string oldest = show(step(r, frame::kDestOnboard, kFirst, status, kFarExpiry), kFirst);
  check("seq-repeat-wrap/oldest-gone", "the oldest (0xFFFFFF80) sent again after the 257th",
    "RECEIVED,DONE (forgotten: run again)", oldest + (oldest == "RECEIVED,DONE" ?
      " (forgotten: run again)" : ""), src);
}

/**
 * @brief ForwardTable with data-link sequence numbers on both sides of 2^32 - 1 and 0.
 *
 * Does an answer find its ground sequence number? Direct use of the public class
 * (command_gate.hpp:127-137). Also: 16 entries; the 17th replaces the oldest; a number added
 * twice is found in the lower slot, not the newer entry.
 *
 * @return nothing
 */
inline void caseSeqForwardTable()
{
  const std::string src = "command_gate.hpp:127-137; command_gate.cpp:441-481";
  ForwardTable t;
  t.add(0xFFFFFFFEU, 100);
  t.add(0xFFFFFFFFU, 101);
  t.add(1U, 102);
  t.add(0U, 103);
  uint32_t g = 0;
  const std::array<uint32_t, 4> links = {0xFFFFFFFEU, 0xFFFFFFFFU, 1U, 0U};
  for (size_t i = 0; i < links.size(); i++) {
    const bool found = t.find(links[i], g);
    check(fmt("seq-forward-table/find-%08x", links[i]), "answer to a link number near the wrap",
      fmt("found, ground sequence %zu", 100 + i), found ? fmt("found, ground sequence %u", g)
                                                        : "not found", src);
  }
  t.remove(0xFFFFFFFFU);
  check("seq-forward-table/remove", "0xFFFFFFFF removed; the others stay", "not found; 3 found",
    fmt("%s; %d found", t.find(0xFFFFFFFFU, g) ? "found" : "not found",
      (t.find(0xFFFFFFFEU, g) ? 1 : 0) + (t.find(1U, g) ? 1 : 0) + (t.find(0U, g) ? 1 : 0)),
    src);
  ForwardTable full;
  for (uint32_t i = 0; i < config::kForwardTableSize; i++) {
    full.add(0xFFFFFFF0U + i, 200 + i);   // 0xFFFFFFF0 .. 0xFFFFFFFF
  }
  full.add(1U, 300);   // the 17th: replaces the oldest, 0xFFFFFFF0
  check("seq-forward-table/17th", "17th entry (link 1) replaces the oldest (0xFFFFFFF0)",
    "0xFFFFFFF0 not found; 0xFFFFFFF1 found, 0xFFFFFFFF found and 1 found",
    fmt("0xFFFFFFF0 %s; 0xFFFFFFF1 %s, 0xFFFFFFFF %s and 1 %s",
      full.find(0xFFFFFFF0U, g) ? "found" : "not found", full.find(0xFFFFFFF1U, g) ? "found"
      : "not found", full.find(0xFFFFFFFFU, g) ? "found" : "not found",
      full.find(1U, g) ? "found" : "not found"), src);
  ForwardTable dup;
  dup.add(7U, 1);
  dup.add(7U, 2);
  const bool found = dup.find(7U, g);
  emit("NOTE", "seq-forward-table/duplicate",
    "link number 7 added for ground 1, then again for ground 2",
    "expected behaviour not specified (a link number repeats only after 2^32 frames, and an "
    "entry lives 16 adds)",
    found ? fmt("find gives ground sequence %u (the lower slot, not necessarily the newest)", g)
          : "not found");
}

/**
 * @brief The updater's own link numbers across 2^32 - 1 to 1, end to end (wrap build only).
 *
 * The data link starts at 0xFFFFFFFC (Makefile, boundaries-wrap). Six ground commands get link
 * numbers 0xFFFFFFFC, 0xFFFFFFFD, 0xFFFFFFFE, 0xFFFFFFFF, 1, 2 (0 is never used,
 * data_link.cpp:81-82); each answer must reach the ground under the ground's number. An ARM/SWITCH
 * pair is sent at every position of the wrap; the ARM must name the number the SWITCH carries
 * (updater.cpp:1572-1575). Late answers (after the timeout) for numbers 0xFFFFFFFF and 1 still
 * find their ground number; an answer for number 0 matches nothing.
 *
 * @return nothing
 */
inline void caseSeqLinkWrap()
{
  stress_clock::setSystemTimeS(kT0);
  const std::string src = "data_link.cpp:81-82; data_link.hpp:28; updater.cpp:1565-1575, "
                          "1405-1415; command_gate.hpp:127-137";
  {
    auto rig = makeRig(freshDir("seq-link-wrap-plain"));
    Rig & r = *rig;
    r.autoAnswer(false);
    for (uint32_t i = 0; i < 6; i++) {
      const uint32_t g = 500 + i;
      r.discard();
      r.clearCounts();
      (void)r.ground(frame::kDestController, g, {command::kGetStatus}, kFarExpiry);
      const uint32_t link = r.frames().empty() ? 0U : r.frames().back().sequence;
      check(fmt("seq-link-wrap/plain-%u-number", i), "link number of the frame",
        fmt("0x%08x", wrapLink(i)), fmt("0x%08x", link), src);
      r.answer(link, answer::kDone, answer::kSuccess);
      const std::string got = show(r.answers(), g);
      check(fmt("seq-link-wrap/plain-%u-answer", i), "controller's DONE reaches the ground",
        "DONE under the ground number", got + (got == "DONE" ? " under the ground number" : ""),
        src);
    }
  }
  for (uint32_t pos = 0; pos < 6; pos++) {
    auto rig = makeRig(freshDir(fmt("seq-link-wrap-pair-%u", pos)));
    Rig & r = *rig;
    for (uint32_t i = 0; i < pos; i++) {
      (void)step(*rig, frame::kDestController, 600 + i, {command::kGetStatus}, kFarExpiry);
    }
    r.clearCounts();
    const uint32_t arm_g = 700 + pos;
    const uint32_t cmd_g = 800 + pos;
    (void)step(r, frame::kDestController, arm_g, armPayload(command::kSwitch, cmd_g),
      kFarExpiry);
    const std::vector<GroundAnswer> got = step(r, frame::kDestController, cmd_g,
      {command::kSwitch}, kFarExpiry);
    uint32_t arm_link = 0;
    uint32_t named = 0;
    uint32_t sw_link = 0;
    for (const LinkFrame & f : r.frames()) {
      if (f.payload.size() == command::kArmBytes && f.payload[0] == command::kArm) {
        arm_link = f.sequence;
        named = bytes::readU32(&f.payload[2]);
      } else if (!f.payload.empty() && f.payload[0] == command::kSwitch) {
        sw_link = f.sequence;
      }
    }
    check(fmt("seq-link-wrap/pair-at-%u", pos),
      fmt("ARM at link number 0x%08x names the SWITCH's number", wrapLink(pos)),
      fmt("ARM 0x%08x names 0x%08x, SWITCH carries 0x%08x, ground gets DONE", wrapLink(pos),
        wrapLink(pos + 1), wrapLink(pos + 1)),
      fmt("ARM 0x%08x names 0x%08x, SWITCH carries 0x%08x, ground gets %s", arm_link, named,
        sw_link, show(got, cmd_g).c_str()), src);
  }
  {
    auto rig = makeRig(freshDir("seq-link-wrap-late"));
    Rig & r = *rig;
    for (uint32_t i = 0; i < 3; i++) {
      (void)step(r, frame::kDestController, 900 + i, {command::kGetStatus}, kFarExpiry);
    }
    for (uint32_t i = 0; i < 2; i++) {
      const uint32_t g = 910 + i;
      const uint32_t link = wrapLink(3 + i);   // 0xFFFFFFFF, then 1
      r.ignoreOnce(command::kGetStatus);
      (void)step(r, frame::kDestController, g, {command::kGetStatus}, kFarExpiry);
      r.advance(config::kAnswerTimeoutMs + 1);
      r.tick();
      r.answer(link, answer::kDone, answer::kSuccess);
      const std::string late = show(r.answers(), g);
      check(fmt("seq-link-wrap/late-0x%08x", link), "a late answer after the timeout",
        "DONE under the ground number", late + (late == "DONE" ? " under the ground number" : ""),
        src);
    }
    r.answer(0U, answer::kDone, answer::kSuccess);
    check("seq-link-wrap/answer-for-0", "an answer for link number 0, which is never used",
      "nothing reaches the ground", r.answers().empty() ? "nothing reaches the ground"
      : "an answer reached the ground", src);
  }
}

/**
 * @brief The clock offset record INT64_MAX, loaded at start.
 *
 * Onboard ground time is clamped to INT64_MAX (updater.cpp:320-330), so every Expiry is in the
 * past: all commands but SET ONBOARD TIME and its ARM are refused 0x03. SET ONBOARD TIME to a
 * normal time recovers; EVENT 0x0105 carries the difference limited to 4 signed bytes
 * (updater.cpp:979-983).
 *
 * @return nothing
 */
inline void caseClockOffsetMax()
{
  stress_clock::setSystemTimeS(kT0);
  const std::string dir = freshDir("clock-offset-max");
  check("clock-offset-max/write", "clock-offset record INT64_MAX written", "written",
    writeOffset(dir, INT64_MAX) ? "written" : "write failed", "clock_record.hpp:36-41");
  auto rig = makeRig(dir);
  Rig & r = *rig;
  const std::string src = "updater.cpp:320-330, 357-370; main.tex:225";
  check("clock-offset-max/get-status", "GET STATUS with Expiry 0xFFFFFFFF", "REFUSED(0x03)",
    show(step(r, frame::kDestOnboard, 11, {command::kGetStatus}, kFarExpiry), 11), src);
  const uint32_t target = static_cast<uint32_t>(kT0) + 100U;
  check("clock-offset-max/arm", "ARM naming SET ONBOARD TIME, Expiry 1 (exempt)",
    "RECEIVED,DONE", show(step(r, frame::kDestOnboard, 12,
      armPayload(command::kSetOnboardTime, 13), 1U), 12), "updater.cpp:77-90 (expiryExempt)");
  check("clock-offset-max/set", "SET ONBOARD TIME to system clock + 100 s", "DONE",
    show(step(r, frame::kDestOnboard, 13, timePayload(target), 1U), 13),
    "updater.cpp:77-90; decision 25");
  int32_t detail = 0;
  const bool seen = clockEventDetail(r.takeReports(), detail);
  check("clock-offset-max/event-0105", "EVENT 0x0105 detail (onboard minus ground, limited)",
    fmt("%d", std::numeric_limits<int32_t>::max()), seen ? fmt("%d", detail) : "no event",
    "updater.cpp:979-983");
  check("clock-offset-max/after", "GET STATUS with Expiry 0xFFFFFFFF after the correction",
    "RECEIVED,DONE", show(step(r, frame::kDestOnboard, 14, {command::kGetStatus}, kFarExpiry), 14),
    src);
}

/**
 * @brief The clock offset record INT64_MIN, loaded at start; SET ONBOARD TIME with ground time 0.
 *
 * Onboard ground time is then about -9.2e18 (updater.cpp:326-327): no Expiry from 1 up is ever
 * past, so the expiry check refuses only Expiry 0. Written as an observation; the sources do not
 * say what a stored offset this far from the system clock should do. SET ONBOARD TIME to ground
 * time 0 does not overflow (clock-offset-min-overflow does, for 0xFFFFFFFF).
 *
 * @return nothing
 */
inline void caseClockOffsetMin()
{
  stress_clock::setSystemTimeS(kT0);
  const std::string dir = freshDir("clock-offset-min");
  check("clock-offset-min/write", "clock-offset record INT64_MIN written", "written",
    writeOffset(dir, INT64_MIN) ? "written" : "write failed", "clock_record.hpp:36-41");
  auto rig = makeRig(dir);
  Rig & r = *rig;
  const std::string src = "updater.cpp:320-330, 357-370";
  const std::string old_expiry = show(step(r, frame::kDestOnboard, 21, {command::kGetStatus}, 1U),
    21);
  check("clock-offset-min/expiry-1", "GET STATUS with Expiry 1", "RECEIVED,DONE (Expiry 1 is not "
    "below the onboard ground time)", old_expiry + (old_expiry == "RECEIVED,DONE" ?
      " (Expiry 1 is not below the onboard ground time)" : ""), src);
  emit("NOTE", "clock-offset-min/expiry-defeated",
    "with offset INT64_MIN, a command whose Expiry passed years ago (Expiry 1) is run",
    "expected behaviour not specified: no source says what a stored offset this far from the "
    "system clock means for the expiry check (updater.cpp:320-330 only clamps the sum)",
    "Expiry 1 accepted; only Expiry 0 is refused");
  check("clock-offset-min/arm", "ARM naming SET ONBOARD TIME", "RECEIVED,DONE",
    show(step(r, frame::kDestOnboard, 22, armPayload(command::kSetOnboardTime, 23), 1U), 22), src);
  check("clock-offset-min/set-0", "SET ONBOARD TIME to ground time 0", "DONE",
    show(step(r, frame::kDestOnboard, 23, timePayload(0U), 1U), 23), "decision 25");
  int32_t detail = 0;
  const bool seen = clockEventDetail(r.takeReports(), detail);
  check("clock-offset-min/event-0105", "EVENT 0x0105 detail", fmt("%d",
    std::numeric_limits<int32_t>::min()), seen ? fmt("%d", detail) : "no event",
    "updater.cpp:979-983");
}

/**
 * @brief SET ONBOARD TIME to 0xFFFFFFFF while the stored offset is INT64_MIN (expects UBSan).
 *
 * difference = onboard ground time - ground time (updater.cpp:957-958). With the sum clamped to
 * INT64_MIN + system clock, taking away a ground time above the system clock goes below INT64_MIN.
 * The unit has -fsanitize=undefined with no recovery, so this case is expected to be ended by the
 * sanitizer; boundaries.cpp reports that as DIED with the sanitizer's line.
 *
 * @return nothing
 */
inline void caseClockOffsetMinOverflow()
{
  stress_clock::setSystemTimeS(kT0);
  const std::string dir = freshDir("clock-offset-min-overflow");
  (void)writeOffset(dir, INT64_MIN);
  auto rig = makeRig(dir);
  Rig & r = *rig;
  (void)step(r, frame::kDestOnboard, 31, armPayload(command::kSetOnboardTime, 32), 1U);
  const std::vector<GroundAnswer> got = step(r, frame::kDestOnboard, 32,
    timePayload(0xFFFFFFFFU), 1U);
  emit("OK", "clock-offset-min-overflow/survived",
    "SET ONBOARD TIME 0xFFFFFFFF with offset INT64_MIN", "no signed overflow",
    "no sanitizer stop; the answer was " + show(got, 32));
}

/**
 * @brief SET ONBOARD TIME with ground time 0, from a normal start.
 *
 * Offset becomes -system clock; the onboard ground time then counts from 0, one per system
 * second. Expiry 1 passes at ground time 0 and 1, fails at 2; an Expiry equal to the ground time
 * passes (updater.cpp:347-352).
 *
 * @return nothing
 */
inline void caseClockSetTimeZero()
{
  stress_clock::setSystemTimeS(kT0);
  auto rig = makeRig(freshDir("clock-set-time-0"));
  Rig & r = *rig;
  const std::string src = "updater.cpp:347-370, 940-990; decision 25";
  (void)step(r, frame::kDestOnboard, 41, armPayload(command::kSetOnboardTime, 42), 0U);
  check("clock-set-time-0/set", "SET ONBOARD TIME to ground time 0 (Expiry 0, exempt)", "DONE",
    show(step(r, frame::kDestOnboard, 42, timePayload(0U), 0U), 42), src);
  int32_t detail = 0;
  const bool seen = clockEventDetail(r.takeReports(), detail);
  check("clock-set-time-0/event-0105", "EVENT 0x0105 detail", fmt("%d", static_cast<int32_t>(kT0)),
    seen ? fmt("%d", detail) : "no event", "updater.cpp:979-983");
  check("clock-set-time-0/expiry-0", "GET STATUS with Expiry 0 at ground time 0", "REFUSED(0x03)",
    show(step(r, frame::kDestOnboard, 43, {command::kGetStatus}, 0U), 43), "updater.cpp:363");
  check("clock-set-time-0/expiry-0-equal", "Expiry 1 at ground time 0", "RECEIVED,DONE",
    show(step(r, frame::kDestOnboard, 44, {command::kGetStatus}, 1U), 44), src);
  stress_clock::setSystemTimeS(kT0 + 1);
  check("clock-set-time-0/ground-1", "Expiry 1 at ground time 1 (equal)", "RECEIVED,DONE",
    show(step(r, frame::kDestOnboard, 45, {command::kGetStatus}, 1U), 45), src);
  stress_clock::setSystemTimeS(kT0 + 2);
  check("clock-set-time-0/ground-2", "Expiry 1 at ground time 2 (past)", "REFUSED(0x03)",
    show(step(r, frame::kDestOnboard, 46, {command::kGetStatus}, 1U), 46), src);
}

/**
 * @brief SET ONBOARD TIME to 0xFFFFFFFF, the largest ground time the field holds.
 *
 * At ground time 0xFFFFFFFF an Expiry of 0xFFFFFFFF passes (equal). One second later the
 * onboard ground time is 2^32, above every 4-byte Expiry: every command but SET ONBOARD TIME and
 * its ARM is refused 0x03 (updater.cpp:363, 80-90). The correction path stays open. The offset
 * survives a restart. Stamps of reports are cut to 32 bits (updater.cpp:340-343).
 *
 * @return nothing
 */
inline void caseClockSetTimeMax()
{
  stress_clock::setSystemTimeS(kT0);
  const std::string dir = freshDir("clock-set-time-max");
  const std::string src = "updater.cpp:347-370, 940-990; decision 25";
  {
    auto rig = makeRig(dir);
    Rig & r = *rig;
    (void)step(r, frame::kDestOnboard, 51, armPayload(command::kSetOnboardTime, 52), 0U);
    check("clock-set-time-max/set", "SET ONBOARD TIME to ground time 0xFFFFFFFF", "DONE",
      show(step(r, frame::kDestOnboard, 52, timePayload(0xFFFFFFFFU), 0U), 52), src);
    int32_t detail = 0;
    const bool seen = clockEventDetail(r.takeReports(), detail);
    check("clock-set-time-max/event-0105", "EVENT 0x0105 detail (onboard - ground)",
      fmt("%d", std::numeric_limits<int32_t>::min()), seen ? fmt("%d", detail) : "no event",
      "updater.cpp:979-983");
    check("clock-set-time-max/equal", "Expiry 0xFFFFFFFF at ground time 0xFFFFFFFF",
      "RECEIVED,DONE",
      show(step(r, frame::kDestOnboard, 53, {command::kGetStatus}, kFarExpiry), 53), src);
    stress_clock::setSystemTimeS(kT0 + 1);
    check("clock-set-time-max/one-second-later", "Expiry 0xFFFFFFFF at ground time 2^32",
      "REFUSED(0x03)", show(step(r, frame::kDestOnboard, 54, {command::kGetStatus}, kFarExpiry),
        54), src);
    emit("NOTE", "clock-set-time-max/lock-out",
      "after SET ONBOARD TIME 0xFFFFFFFF, one second later",
      "expected behaviour not specified: no source says what happens when the onboard ground "
      "time passes the largest Expiry a frame can carry (Interfaces Sec. 4.0: Expiry is 4 bytes)",
      "every command except SET ONBOARD TIME (and its ARM) is refused 0x03; the updater's own "
      "frames to the controller carry Expiry 0xFFFFFFFF");
    check("clock-set-time-max/recover-arm", "ARM naming SET ONBOARD TIME, Expiry 0",
      "RECEIVED,DONE",
      show(step(r, frame::kDestOnboard, 55, armPayload(command::kSetOnboardTime, 56), 0U), 55),
      "updater.cpp:80-90");
    check("clock-set-time-max/recover-set", "SET ONBOARD TIME back to system clock", "DONE",
      show(step(r, frame::kDestOnboard, 56, timePayload(static_cast<uint32_t>(kT0 + 1)), 0U), 56),
      "updater.cpp:80-90");
    check("clock-set-time-max/recovered", "GET STATUS after the correction", "RECEIVED,DONE",
      show(step(r, frame::kDestOnboard, 57, {command::kGetStatus}, kFarExpiry), 57), src);
    (void)step(r, frame::kDestOnboard, 58, armPayload(command::kSetOnboardTime, 59), 0U);
    (void)step(r, frame::kDestOnboard, 59, timePayload(0xFFFFFFFFU), 0U);
  }
  auto again = makeRig(dir);
  stress_clock::setSystemTimeS(kT0 + 2);
  const std::string after = show(step(*again, frame::kDestOnboard, 60, {command::kGetStatus},
    kFarExpiry), 60);
  check("clock-set-time-max/restart", "after a restart, Expiry 0xFFFFFFFF with the stored offset "
    "and the system clock 1 s later", "REFUSED(0x03) (the stored offset is read at start)",
    after + (after == "REFUSED(0x03)" ? " (the stored offset is read at start)" : ""),
    "updater.cpp:285-312");
}

/**
 * @brief Gives the Expiry boundary at one ground time for several commands.
 *
 * Expiry = ground time - 1, ground time, ground time + 1 for GET STATUS to the onboard computer,
 * GET STATUS to the controller, and an ARM; then SET LIMITS under a past Expiry with a valid ARM
 * held (REFUSED 0x03 and the ARM stays held), then the same SET LIMITS with a good Expiry.
 *
 * @param[in]     id_prefix  prefix of the check names
 * @param[in]     dir        data directory
 * @param[in]     ground_s   the onboard ground time the rig has at the system clock kT0
 * @return nothing
 */
inline void expiryBoundaryAt(const std::string & id_prefix, const std::string & dir,
  int64_t ground_s)
{
  auto rig = makeRig(dir);
  Rig & r = *rig;
  const std::string src = "updater.cpp:347-352, 363; main.tex:225 (\"After it, the command is "
                          "refused\"); REQ-066, REQ-133";
  const uint32_t g = static_cast<uint32_t>(ground_s);
  uint32_t seq = 100;
  struct Probe
  {
    const char * name;
    uint32_t expiry;
    const char * expected;
  };
  const std::array<Probe, 3> probes = {{{"g-1", g - 1U, "REFUSED(0x03)"},
                                        {"g", g, "RECEIVED,DONE"},
                                        {"g+1", g + 1U, "RECEIVED,DONE"}}};
  for (const Probe & p : probes) {
    const uint32_t s = seq++;
    check(id_prefix + "/onboard-" + p.name, fmt("GET STATUS to onboard, Expiry %s = %u", p.name,
      p.expiry), p.expected, show(step(r, frame::kDestOnboard, s, {command::kGetStatus},
        p.expiry), s), src);
  }
  for (const Probe & p : probes) {
    const uint32_t s = seq++;
    const std::string want = (std::string(p.expected) == "REFUSED(0x03)") ? p.expected : "DONE";
    check(id_prefix + "/controller-" + p.name, fmt("GET STATUS to controller, Expiry %s", p.name),
      want, show(step(r, frame::kDestController, s, {command::kGetStatus}, p.expiry), s), src);
  }
  for (const Probe & p : probes) {
    const uint32_t s = seq++;
    check(id_prefix + "/arm-" + p.name, fmt("ARM naming SET LIMITS, Expiry %s", p.name),
      p.expected, show(step(r, frame::kDestOnboard, s, armPayload(command::kSetLimits, seq + 50U),
        p.expiry), s), src);
  }
  // SET LIMITS with a past Expiry and a valid held ARM: refused 0x03 before the ARM is looked at
  // (updater.cpp:496-512), so the ARM stays held and the same command with a good Expiry runs.
  const uint32_t arm_s = seq++;
  const uint32_t cmd_s = seq++;
  (void)step(r, frame::kDestOnboard, arm_s, armPayload(command::kSetLimits, cmd_s), g);
  check(id_prefix + "/setlimits-past", "SET LIMITS, Expiry g-1, valid ARM held", "REFUSED(0x03)",
    show(step(r, frame::kDestOnboard, cmd_s, limitsPayload(kLimitsVersion, kLimitsIdentity),
      g - 1U), cmd_s), src);
  const std::string then = show(step(r, frame::kDestOnboard, cmd_s,
    limitsPayload(kLimitsVersion, kLimitsIdentity), g), cmd_s);
  check(id_prefix + "/setlimits-then", "the same SET LIMITS, Expiry g", "DONE (not remembered; "
    "ARM still held)", then + (then == "DONE" ? " (not remembered; ARM still held)" : ""),
    "updater.cpp:496-512 (\"not remembered\")");
}

/**
 * @brief Expiry on the onboard ground time with offset 0 (system clock = ground time).
 *
 * @return nothing
 */
inline void caseExpiryBoundary()
{
  stress_clock::setSystemTimeS(kT0);
  expiryBoundaryAt("expiry-boundary", freshDir("expiry-boundary"), kT0);
}

/**
 * @brief Expiry on the onboard ground time with a stored offset of +100 s and -100 s.
 *
 * @return nothing
 */
inline void caseExpiryBoundaryOffset()
{
  stress_clock::setSystemTimeS(kT0);
  constexpr int64_t kOffsetS = 100;   // test choice: any non-zero offset shows the sum is used
  const std::string plus = freshDir("expiry-boundary-plus");
  (void)writeOffset(plus, kOffsetS);
  expiryBoundaryAt("expiry-boundary-offset-plus100", plus, kT0 + kOffsetS);
  const std::string minus = freshDir("expiry-boundary-minus");
  (void)writeOffset(minus, -kOffsetS);
  expiryBoundaryAt("expiry-boundary-offset-minus100", minus, kT0 - kOffsetS);
}

/**
 * @brief Expiry 0 and the commands exempt from the expiry check.
 *
 * Expiry 0 is refused for every command except SET ONBOARD TIME and an ARM naming it, both to the
 * onboard computer (updater.cpp:77-90, 348-352).
 *
 * @return nothing
 */
inline void caseExpiryExempt()
{
  stress_clock::setSystemTimeS(kT0);
  auto rig = makeRig(freshDir("expiry-exempt"));
  Rig & r = *rig;
  const std::string src = "updater.cpp:77-90, 348-352; decision 25";
  check("expiry-exempt/get-status-0", "GET STATUS, Expiry 0", "REFUSED(0x03)",
    show(step(r, frame::kDestOnboard, 61, {command::kGetStatus}, 0U), 61), src);
  check("expiry-exempt/arm-limits-0", "ARM naming SET LIMITS, Expiry 0", "REFUSED(0x03)",
    show(step(r, frame::kDestOnboard, 62, armPayload(command::kSetLimits, 63), 0U), 62), src);
  check("expiry-exempt/arm-switch-0", "ARM naming SWITCH to the controller, Expiry 0",
    "REFUSED(0x03)", show(step(r, frame::kDestController, 64,
      armPayload(command::kSwitch, 65), 0U), 64), src);
  check("expiry-exempt/arm-time-0", "ARM naming SET ONBOARD TIME, Expiry 0", "RECEIVED,DONE",
    show(step(r, frame::kDestOnboard, 66, armPayload(command::kSetOnboardTime, 67), 0U), 66), src);
  check("expiry-exempt/time-0", "SET ONBOARD TIME, Expiry 0", "DONE",
    show(step(r, frame::kDestOnboard, 67, timePayload(static_cast<uint32_t>(kT0)), 0U), 67), src);
  const std::string to_controller = show(step(r, frame::kDestController, 68,
    armPayload(command::kSetOnboardTime, 69), 0U), 68);
  check("expiry-exempt/arm-time-to-controller", "ARM naming SET ONBOARD TIME, Destination "
    "controller, Expiry 0", "REFUSED(0x03) (the exemption is for the onboard computer only)",
    to_controller + (to_controller == "REFUSED(0x03)" ?
      " (the exemption is for the onboard computer only)" : ""), src);
}

/**
 * @brief A command that finished, sent again after its Expiry has passed.
 *
 * The expiry check comes before the repeat memories (updater.cpp:496-512), so the repeat is
 * refused 0x03 and not answered ALREADY DONE. The code comment calls the order a Proposal
 * (TBR), awaiting the owner's acceptance.
 *
 * @return nothing
 */
inline void caseExpiryResend()
{
  stress_clock::setSystemTimeS(kT0);
  auto rig = makeRig(freshDir("expiry-resend"));
  Rig & r = *rig;
  const uint32_t expiry = static_cast<uint32_t>(kT0) + 1U;
  check("expiry-resend/first", "GET STATUS, Expiry = system clock + 1 s", "RECEIVED,DONE",
    show(step(r, frame::kDestOnboard, 71, {command::kGetStatus}, expiry), 71), "updater.cpp:363");
  stress_clock::setSystemTimeS(kT0 + 1);
  check("expiry-resend/inside", "same command again, one second later (Expiry = ground time)",
    "ALREADY_DONE", show(step(r, frame::kDestOnboard, 71, {command::kGetStatus}, expiry), 71),
    "updater.cpp:347-352, 496-512");
  stress_clock::setSystemTimeS(kT0 + 2);
  const std::string after = show(step(r, frame::kDestOnboard, 71, {command::kGetStatus}, expiry),
    71);
  check("expiry-resend/after", "same command again, two seconds later (past)",
    "REFUSED(0x03) (expiry first, a Proposal TBR in the code comment)", after +
    (after == "REFUSED(0x03)" ? " (expiry first, a Proposal TBR in the code comment)" : ""),
    "updater.cpp:500-505");
}

/**
 * @brief ARM for SET LIMITS (onboard dangerous command) used 29,999, 30,000 and 30,001 ms later.
 *
 * Source: the held ARM is refused when older than kArmExpiryMs, 30,000 ms (config.hpp:57-60;
 * updater.cpp:723; "no older than", updater.cpp:808). At exactly 30,000 ms the ARM is accepted.
 *
 * @return nothing
 */
inline void caseArmOnboard()
{
  stress_clock::setSystemTimeS(kT0);
  auto rig = makeRig(freshDir("arm-onboard"));
  Rig & r = *rig;
  const std::string src = "config.hpp:57-60; updater.cpp:723, 808; REQ-030";
  uint32_t seq = 100;
  const std::array<uint64_t, 3> deltas = {{config::kArmExpiryMs - 1U, config::kArmExpiryMs,
                                           config::kArmExpiryMs + 1U}};
  for (const uint64_t d : deltas) {
    const uint32_t arm_s = seq++;
    const uint32_t cmd_s = seq++;
    (void)step(r, frame::kDestOnboard, arm_s, armPayload(command::kSetLimits, cmd_s),
      kFarExpiry);
    r.advance(d);
    const std::string want = (d <= config::kArmExpiryMs) ? "DONE" : "REFUSED(0x02)";
    check(fmt("arm-onboard/%llu-ms", static_cast<unsigned long long>(d)),
      fmt("SET LIMITS %llu ms after its ARM", static_cast<unsigned long long>(d)), want,
      show(step(r, frame::kDestOnboard, cmd_s, limitsPayload(kLimitsVersion, kLimitsIdentity),
        kFarExpiry), cmd_s), src);
  }
}

/**
 * @brief ARM for SWITCH (controller dangerous command), then the SWITCH 29,999, 30,000 and
 *        30,001 ms later.
 *
 * Checked on arrival (updater.cpp:723). At 30,000 ms the pair goes to the controller (an ARM frame
 * and the SWITCH); at 30,001 ms the SWITCH is refused 0x02 and nothing goes to the controller.
 *
 * @return nothing
 */
inline void caseArmControllerArrival()
{
  stress_clock::setSystemTimeS(kT0);
  auto rig = makeRig(freshDir("arm-controller-arrival"));
  Rig & r = *rig;
  const std::string src = "config.hpp:57-60; updater.cpp:723, 808; REQ-030";
  uint32_t seq = 100;
  const std::array<uint64_t, 3> deltas = {{config::kArmExpiryMs - 1U, config::kArmExpiryMs,
                                           config::kArmExpiryMs + 1U}};
  for (const uint64_t d : deltas) {
    const uint32_t arm_s = seq++;
    const uint32_t cmd_s = seq++;
    (void)step(r, frame::kDestController, arm_s, armPayload(command::kSwitch, cmd_s), kFarExpiry);
    r.advance(d);
    r.clearCounts();
    const bool ok = d <= config::kArmExpiryMs;
    const std::string got = show(step(r, frame::kDestController, cmd_s, {command::kSwitch},
      kFarExpiry), cmd_s);
    check(fmt("arm-controller-arrival/%llu-ms", static_cast<unsigned long long>(d)),
      fmt("SWITCH %llu ms after its ARM; SWITCH and ARM frames to the controller",
        static_cast<unsigned long long>(d)),
      ok ? "DONE; 1 SWITCH, 1 ARM" : "REFUSED(0x02); 0 SWITCH, 0 ARM",
      fmt("%s; %u SWITCH, %u ARM", got.c_str(), r.passed(command::kSwitch),
        r.passed(command::kArm)), src);
  }
}

/**
 * @brief The held ARM of a queued SWITCH, checked again when its turn comes (30 s boundary).
 *
 * Data link A is kept busy with an unanswered GET STATUS; ARM and SWITCH arrive, the SWITCH is
 * queued. One loop pass then runs 29,999, 30,000 or 30,001 ms after the ARM, as after a stalled
 * main loop (the answer timeout alone is 2 s, config.hpp:36, and the queue holds 8, so a wait of
 * 30 s needs a stalled loop). Source: updater.cpp:1696, sendNext re-checks with ">" kArmExpiryMs.
 *
 * @return nothing
 */
inline void caseArmControllerQueue()
{
  stress_clock::setSystemTimeS(kT0);
  const std::string src = "updater.cpp:1596-1606 (sendNext), 1696; updater.hpp:132-135; REQ-030";
  const std::array<uint64_t, 3> deltas = {{config::kArmExpiryMs - 1U, config::kArmExpiryMs,
                                           config::kArmExpiryMs + 1U}};
  uint32_t seq = 100;
  for (const uint64_t d : deltas) {
    const unsigned long long d_ms = static_cast<unsigned long long>(d);
    auto rig = makeRig(freshDir(fmt("arm-controller-queue-%llu", d_ms)));
    Rig & r = *rig;
    r.ignoreOnce(command::kGetStatus);
    (void)step(r, frame::kDestController, seq++, {command::kGetStatus}, kFarExpiry);
    const uint32_t arm_s = seq++;
    const uint32_t cmd_s = seq++;
    (void)step(r, frame::kDestController, arm_s, armPayload(command::kSwitch, cmd_s), kFarExpiry);
    const uint64_t arm_at = r.now();
    r.advance(1);
    (void)step(r, frame::kDestController, cmd_s, {command::kSwitch}, kFarExpiry);
    r.setNow(arm_at + d);
    r.clearCounts();
    r.discard();
    r.pump();
    const std::vector<GroundAnswer> got = r.answers();
    const bool ok = d <= config::kArmExpiryMs;
    check(fmt("arm-controller-queue/%llu-ms", static_cast<unsigned long long>(d)),
      fmt("queued SWITCH, loop pass %llu ms after the ARM", static_cast<unsigned long long>(d)),
      ok ? "DONE; 1 SWITCH, 1 ARM" : "REFUSED(0x02); 0 SWITCH, 0 ARM",
      fmt("%s; %u SWITCH, %u ARM", show(got, cmd_s).c_str(), r.passed(command::kSwitch),
        r.passed(command::kArm)), src);
  }
}

/**
 * @brief The trial watch at 149,999, 150,000 and 150,001 ms after the trial start.
 *
 * kTrialMs + kTrialMarginMs = 150,000 ms (config.hpp:69-70). Source: "within" that time
 * (trial_watch.hpp:9); the code expires on ">" (trial_watch.cpp:89). Checked on the class
 * (start by EVENT 0x0006 and by a HEALTH report in TRIAL) and through the Updater (log line
 * "trial: no EVENT 0x0007 or 0x0008 within").
 *
 * @return nothing
 */
inline void caseTrialWatch()
{
  stress_clock::setSystemTimeS(kT0);
  const std::string src = "trial_watch.hpp:7-9; trial_watch.cpp:89; config.hpp:69-70; decision H";
  constexpr uint64_t kStart = 5000;   // test choice: any start time above 0
  const uint64_t limit = config::kTrialMs + config::kTrialMarginMs;
  const std::array<uint64_t, 3> deltas = {{limit - 1U, limit, limit + 1U}};
  for (const uint64_t d : deltas) {
    TrialWatch by_event(config::kTrialMs, config::kTrialMarginMs);
    by_event.onEvent(report::kEventTrialStarted, kStart);
    TrialWatch by_health(config::kTrialMs, config::kTrialMarginMs);
    by_health.onHealthMode(report::kModeTrial, 0, kStart);
    const std::string want = (d > limit) ? "expired" : "not expired";
    check(fmt("trial-watch/class-event-%llu", static_cast<unsigned long long>(d)),
      fmt("TrialWatch.expired() %llu ms after EVENT 0x0006", static_cast<unsigned long long>(d)),
      want, by_event.expired(kStart + d) ? "expired" : "not expired", src);
    check(fmt("trial-watch/class-health-%llu", static_cast<unsigned long long>(d)),
      fmt("TrialWatch.expired() %llu ms after the first HEALTH in TRIAL",
        static_cast<unsigned long long>(d)),
      want, by_health.expired(kStart + d) ? "expired" : "not expired", src);
  }
  auto rig = makeRig(freshDir("trial-watch"));
  Rig & r = *rig;
  r.setNow(kStart);
  r.event(report::kEventTrialStarted);
  const size_t mark = logSize();
  size_t seen = 0;
  for (const uint64_t d : deltas) {
    r.setNow(kStart + d);
    r.tick();
    seen = logCount(mark, "trial: no EVENT 0x0007 or 0x0008");
    check(fmt("trial-watch/updater-%llu", static_cast<unsigned long long>(d)),
      fmt("Updater tick %llu ms after EVENT 0x0006: overrun logged so far",
        static_cast<unsigned long long>(d)),
      (d > limit) ? "1" : "0", fmt("%zu", seen), src);
  }
  r.setNow(kStart + limit + 2U);
  r.tick();
  check("trial-watch/updater-once", "one more tick later: still one overrun line", "1",
    fmt("%zu", logCount(mark, "trial: no EVENT 0x0007 or 0x0008")), "trial_watch.hpp:11 (once)");
}

/**
 * @brief The silence watch at 4,999 and 5,000 ms after the last good frame from the controller.
 *
 * kSilenceMs = 5,000 ms (config.hpp:93). The code is silent when now - last heard >= 5,000
 * (controller_watch.cpp:68). controller_watch.hpp:5 says "after 5 s without one"; whether exactly
 * 5 s counts is not written.
 *
 * @return nothing
 */
inline void caseSilence()
{
  stress_clock::setSystemTimeS(kT0);
  auto rig = makeRig(freshDir("silence"));
  Rig & r = *rig;
  constexpr uint64_t kHeardAt = 2000;   // test choice: later than the rig's start
  r.setNow(kHeardAt);
  r.health(report::kModeOperate);
  r.setNow(kHeardAt + config::kSilenceMs - 1U);
  r.tick();
  check("silence/4999", "tick 4,999 ms after the last good frame", "not silent",
    r.watch().silent() ? "silent" : "not silent", "controller_watch.hpp:5; config.hpp:93");
  r.setNow(kHeardAt + config::kSilenceMs);
  r.tick();
  emit("NOTE", "silence/5000", "tick exactly 5,000 ms after the last good frame",
    "expected behaviour at exactly 5 s not specified (controller_watch.hpp:5 \"After 5 s without "
    "one\"; config.hpp:92-93)",
    r.watch().silent() ? "silent (the test is now - last heard >= 5000, controller_watch.cpp:68)"
                       : "not silent (the test is strict)");
}

/**
 * @brief The reset gap at 59,999 and 60,000 ms after the previous reset, and the give-up.
 *
 * kSilentResetGapMs = 60,000 ms (config.hpp:97). First reset at the start of the silence; the
 * next after the gap; at most kSilentResetsMax = 3; one gave-up line a full gap after the third
 * (controller_watch.hpp:7-15, controller_watch.cpp:105-114). No restart script is configured, so
 * a reset is only counted (updater.cpp:1947-1957).
 *
 * @return nothing
 */
inline void caseResetGap()
{
  stress_clock::setSystemTimeS(kT0);
  const std::string src = "controller_watch.hpp:9-15; config.hpp:94-100; "
                          "controller_watch.cpp:105-114";
  auto rig = makeRig(freshDir("reset-gap"));
  Rig & r = *rig;
  const size_t mark = logSize();
  uint64_t reset_at = kRigStartMs + config::kSilenceMs;   // first tick that sees the silence
  r.setNow(reset_at);
  r.tick();
  check("reset-gap/first", "first reset at the start of the silence", "1 reset",
    fmt("%u reset", r.watch().resetsDone()), src);
  for (uint32_t n = 2; n <= config::kSilentResetsMax; n++) {
    r.setNow(reset_at + config::kSilentResetGapMs - 1U);
    r.tick();
    check(fmt("reset-gap/before-%u", n), fmt("tick %llu ms after reset %u",
      static_cast<unsigned long long>(config::kSilentResetGapMs - 1U), n - 1U),
      fmt("%u reset(s)", n - 1U), fmt("%u reset(s)", r.watch().resetsDone()), src);
    reset_at += config::kSilentResetGapMs;
    r.setNow(reset_at);
    r.tick();
    check(fmt("reset-gap/at-%u", n), fmt("tick exactly %llu ms after reset %u",
      static_cast<unsigned long long>(config::kSilentResetGapMs), n - 1U),
      fmt("%u reset(s)", n), fmt("%u reset(s)", r.watch().resetsDone()), src);
  }
  r.setNow(reset_at + config::kSilentResetGapMs - 1U);
  r.tick();
  check("reset-gap/giveup-before", "tick 59,999 ms after the last reset: give-up lines", "0",
    fmt("%zu", logCount(mark, "controller still silent after")), src);
  r.setNow(reset_at + config::kSilentResetGapMs);
  r.tick();
  r.setNow(reset_at + config::kSilentResetGapMs + 1U);
  r.tick();
  check("reset-gap/giveup-at", "tick exactly 60,000 ms after the last reset, and one more "
    "tick later: "
    "give-up lines", "1", fmt("%zu", logCount(mark, "controller still silent after")), src);
  check("reset-gap/count", "resets counted at the end", fmt("%u", config::kSilentResetsMax),
    fmt("%u", r.watch().resetsDone()), "config.hpp:98-100");
}

}  // namespace stress

#endif  // FSIR_STRESS_BOUNDARIES_CASES_A4_HPP
