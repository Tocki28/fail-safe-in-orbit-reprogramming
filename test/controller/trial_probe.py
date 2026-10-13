#!/usr/bin/env python3
"""Bench tool: run the trial of new firmware on the controller over data link A (step 2d).

Acts as the onboard computer: ARM (0x0D), SWITCH (0x20), CONFIRM (0x26) and the test image
(UPLOAD with image number 0xFFFF) as in the Interfaces document, Sec. 5.1 and 5.2; answers as in
Sec. 4.2; HEALTH and EVENT reports as in Sec. 5.4. Framing and stop-and-wait from
link_probe.py and upload_probe.py. Standard library only.

Start: the step-2d firmware running in OPERATE from slot A or B (make -C ../../src/controller
flash-firmware runs firmware A as confirmed). R is the running slot, F the other one.

Common part of every scenario:
  1. a HEALTH report: OPERATE, slot R;
  2. firmware-F.img uploaded into slot F, every command DONE (skipped with --no-upload);
  3. SWITCH without ARM                         REFUSED 0x01 only
  4. ARM naming SWITCH, then SWITCH             RECEIVED, DONE 0x00 each; the chip restarts
  5. after the restart: EVENT 0x0006 "trial started" and HEALTH TRIAL, slot F, failed starts 1,
     trial time left 1-120 s, last self-test 0x00
--scenario confirm:
  6. ARM + CONFIRM before the test image        REFUSED 0x0F only
  7. the test image (1,024 bytes, right check value): END DONE 0x00, then EVENT 0x000A
  8. CONFIRM without ARM                        REFUSED 0x01 only
  9. ARM naming another sequence number, CONFIRM  REFUSED 0x01 only
 10. ARM + CONFIRM                              RECEIVED, EVENT 0x0007, DONE 0x00
 11. HEALTH OPERATE, slot F, trial time left 0, failed starts 0
--scenario bad-image:
  6. a test image with a wrong check value      END RECEIVED, REFUSED 0x08
  7. EVENT 0x000B detail 0x03, EVENT 0x0008, restart; HEALTH OPERATE, slot R, failed starts 0
--scenario timeout:
  6. the test image, right check value          END DONE 0x00, EVENT 0x000A
  7. no CONFIRM: EVENT 0x0008 about 120 s after EVENT 0x0006, no 0x000B; restart; HEALTH
     OPERATE, slot R, failed starts 0
Prints PASS or FAIL per check and "trial probe: all passed" or the number failed; exit status 1
if any check fails.

Usage: trial_probe.py PORT --images DIR --scenario confirm|bad-image|timeout
                      [--no-upload] [--listen SECONDS] [--verbose]
  --images  src/controller/build/images (firmware-A.img, firmware-B.img)
"""
import argparse
import os
import struct
import sys
import time
import zlib
from pathlib import Path

import link_probe as lp
import upload_probe as up

ARM, SWITCH, CONFIRM = 0x0D, 0x20, 0x26
NOT_ARMED, DAMAGED, TESTS_NOT_PASSED = 0x01, 0x08, 0x0F
EV_TRIAL_STARTED, EV_CONFIRMED, EV_GIVEN_UP = 0x0006, 0x0007, 0x0008
EV_TESTS_PASSED, EV_TESTS_FAILED = 0x000A, 0x000B
DETAIL_TEST_IMAGE = 0x03          # EVENT 0x000B detail: test image (accepted 13 Oct 2026)
TEST_IMAGE_NUMBER = 0xFFFF        # Interfaces Sec. 5.1
TEST_IMAGE_BYTES = 1024           # kTestImageMaxBytes, bench maximum
TRIAL_S = up.TRIAL_S              # kTrialMs, bench value
# Answers to ARM come within milliseconds. SWITCH and CONFIRM write the boot state first (one
# page erase, up to 24.47 ms [DS12589 Rev 6, Table 46, p. 107]); 1 s covers it with margin.
WAIT_COMMAND_S = 1.0
# From SWITCH's DONE to the trial firmware's first HEALTH: the restart, the bootloader's checks
# of the trial image (CRC-32 and two Ed25519 signatures), its log lines, and the first HEALTH
# 1 s after the firmware starts. 10 s is several times that.
WAIT_RESTART_S = 10.0
# After the test image's END: EVENT 0x000A, or 0x000B and 0x0008, follow on the same pass.
WAIT_EVENT_S = 2.0
# The time-out scenario: the trial time plus margin for the restart and the first HEALTH.
WAIT_TIMEOUT_S = TRIAL_S + 15.0
TIMEOUT_TOLERANCE_S = 2.0         # EVENT 0x0008 at 120 s after EVENT 0x0006, within this
PROGRESS_S = 10.0                 # time-out scenario: one progress line every 10 s


def events(frames: list) -> list:
    """The EVENT reports among `frames`, decoded, in order.

    Args:
        frames: Frame dicts from link_probe.Link.collect.

    Returns:
        A list of dicts from link_probe.decode_event.
    """
    return [e for e in (lp.decode_event(f) for f in frames) if e is not None]


def has_event(frames: list, code: int, detail=None) -> bool:
    """Tells whether an EVENT with the given code is among the frames.

    Args:
        frames: Frame dicts from link_probe.Link.collect.
        code: EVENT code looked for.
        detail: If not None, the first detail byte must also equal this value.

    Returns:
        True if such an EVENT was received, False otherwise.
    """
    return any(e["code"] == code and (detail is None or e["detail"][:1] == bytes([detail]))
               for e in events(frames))


def healths(frames: list) -> list:
    """Decodes the HEALTH reports among the frames.

    Args:
        frames: Frame dicts from link_probe.Link.collect.

    Returns:
        A list of dicts from link_probe.decode_health, in order.
    """
    return [h for h in (lp.decode_health(f) for f in frames) if h is not None]


def test_image(good: bool) -> bytes:
    """1,024 bytes: 1,020 pattern bytes, then their CRC-32 little-endian (Interfaces Sec. 5.1,
    Sec. 4.0); with good False, that check value with its lowest bit inverted.

    Args:
        good: True for the right check value, False for a wrong one.

    Returns:
        The image as bytes.
    """
    data = bytes((i * 31 + 7) & 0xFF for i in range(TEST_IMAGE_BYTES - 4))
    return data + struct.pack("<I", zlib.crc32(data) ^ (0 if good else 1))


class TrialProbe(up.Probe):
    def wait_until(self, mark: int, seconds: float, done) -> bool:
        """Collect frames until done(frames since mark) is true or `seconds` pass.

        New frames are appended to self.frames.

        Args:
            mark: Index in self.frames where the frames of interest start.
            seconds: Longest time to wait.
            done: Function taking a list of frames and returning true when the wait may end.

        Returns:
            True if done is true for the frames since mark at the end, False otherwise.
        """
        if done(self.frames[mark:]):
            return True
        self.frames += self.link.collect(seconds, lambda fs: done(self.frames[mark:] + fs))
        return done(self.frames[mark:])

    def arm(self, command: int, named: int) -> list:
        """Sends ARM for a dangerous command and waits for its answers.

        Args:
            command: Command id that the ARM releases (SWITCH or CONFIRM).
            named: Sequence number of the command frame that the ARM names.

        Returns:
            The (kind, code) answers to the ARM.
        """
        payload = struct.pack("<BBI", ARM, command, named)
        return self.command(payload, WAIT_COMMAND_S, f"ARM 0x{command:02x} for seq {named}")

    def dangerous(self, command: int, what: str) -> list:
        """Sends a one-byte command and waits for its answers.

        Args:
            command: Command id (SWITCH or CONFIRM).
            what: Short description printed with the frame.

        Returns:
            The (kind, code) answers to the command.
        """
        return self.command(bytes([command]), WAIT_COMMAND_S, what)

    def armed(self, command: int, what: str) -> tuple:
        """ARM naming the next frame's sequence number, then the command: (ARM's answers, the
        command's answers, the command's sequence number).

        Args:
            command: Command id (SWITCH or CONFIRM).
            what: Short description printed with the command frame.

        Returns:
            The tuple described above.
        """
        named = self.seq + 2      # command() adds 1 for the ARM, then 1 for this command
        arm_answers = self.arm(command, named)
        return arm_answers, self.dangerous(command, what), named


def report_answers(checks: lp.Checks, answers: list, want: list, what: str) -> None:
    """Reports a check that the answers equal the wanted ones.

    Args:
        checks: Result counter.
        answers: (kind, code) pairs received.
        want: (kind, code) pairs expected.
        what: Name of the check.
    """
    checks.report(answers == want, f"{what}: {up.code_text(want)} (got {up.code_text(answers)})")


def start_health(probe: TrialProbe, checks: lp.Checks):
    """Step 1: the running slot R from a HEALTH report in OPERATE; None if not usable.

    Args:
        probe: Probe used for the link.
        checks: Result counter.

    Returns:
        The slot name "A" or "B", or None if no HEALTH report in OPERATE from slot A or B arrived.
    """
    reports = probe.health(up.WAIT_HEALTH_S)
    h = reports[-1][1] if reports else None
    if h is None:
        checks.report(False, "a HEALTH report arrived")
        return None
    print("      " + lp.health_text(h))
    ok = h["mode"] == up.MODE_OPERATE and h["slot"] in up.SLOT_NAMES
    checks.report(ok, "start: HEALTH OPERATE from slot A or B (flash-firmware first otherwise)")
    return up.SLOT_NAMES[h["slot"]] if ok else None


def switch_to_trial(probe: TrialProbe, checks: lp.Checks, images: Path, running: str,
                    upload: bool) -> bool:
    """Steps 2 to 5: upload firmware-F, SWITCH refused without ARM, ARM + SWITCH, the trial.

    Args:
        probe: Probe used for the link.
        checks: Result counter.
        images: Directory with firmware-A.img and firmware-B.img.
        running: Name of the running slot R, "A" or "B".
        upload: If False, the upload of firmware-F is skipped.

    Returns:
        True if a HEALTH report in TRIAL arrived after the restart, False otherwise.
    """
    free = "B" if running == "A" else "A"
    if upload:
        image = (images / f"firmware-{free}.img").read_bytes()
        print(f"---- firmware-{free}.img into slot {free}")
        step, answers, total, _, _ = probe.upload(image, 1, f"firmware-{free}.img")
        checks.report(step == "END" and answers == up.DONE_OK,
                      f"firmware-{free}.img uploaded: every command DONE 0x00 "
                      f"(stopped at {step}: {up.code_text(answers)}; {total:.1f} s)")
    print("---- SWITCH")
    report_answers(checks, probe.dangerous(SWITCH, "SWITCH without ARM"),
                   [(lp.REFUSED, NOT_ARMED)], "SWITCH without ARM")
    mark = len(probe.frames)
    arm_answers, answers, _ = probe.armed(SWITCH, "SWITCH")
    report_answers(checks, arm_answers, up.DONE_OK, "ARM naming SWITCH")
    report_answers(checks, answers, up.DONE_OK, "SWITCH after its ARM")

    print(f"---- restart into slot {free} (trial)")
    in_trial = probe.wait_until(
        mark, WAIT_RESTART_S,
        lambda fs: has_event(fs, EV_TRIAL_STARTED) and any(
            h["mode"] == up.MODE_TRIAL for h in healths(fs)))
    trial = [h for h in healths(probe.frames[mark:]) if h["mode"] == up.MODE_TRIAL]
    checks.report(in_trial and has_event(probe.frames[mark:], EV_TRIAL_STARTED),
                  "after the restart: EVENT 0x0006 trial started")
    if not trial:
        checks.report(False, "after the restart: a HEALTH report in TRIAL")
        return False
    h = trial[0]
    print("      " + lp.health_text(h))
    checks.report(up.SLOT_NAMES.get(h["slot"]) == free and h["failed_starts"] == 1 and
                  1 <= h["trial_left"] <= TRIAL_S and h["self_test"] == 0,
                  f"HEALTH: TRIAL, slot {free}, failed starts 1, trial time left 1-{TRIAL_S} s, "
                  f"last self-test 0x00 PASSED")
    return True


def send_test_image(probe: TrialProbe, good: bool) -> tuple:
    """The test image: (step that stopped it or "END", its answers).

    Args:
        probe: Probe used for the link.
        good: True for the right check value, False for a wrong one.

    Returns:
        The tuple described above.
    """
    label = "test image" + ("" if good else ", wrong check value")
    print(f"---- {label}")
    step, answers, _, _, _ = probe.upload(test_image(good), TEST_IMAGE_NUMBER, label)
    return step, answers


def wait_restart_to(probe: TrialProbe, checks: lp.Checks, mark: int, slot: str) -> None:
    """After a give-up: the restart, then HEALTH OPERATE from `slot` with failed starts 0.

    Args:
        probe: Probe used for the link.
        checks: Result counter.
        mark: Index in probe.frames where the frames of interest start.
        slot: Name of the slot expected to run, "A" or "B".
    """
    back = probe.wait_until(
        mark, WAIT_RESTART_S,
        lambda fs: any(h["mode"] == up.MODE_OPERATE and up.SLOT_NAMES.get(h["slot"]) == slot
                       for h in healths(fs)))
    operate = [h for h in healths(probe.frames[mark:]) if h["mode"] == up.MODE_OPERATE]
    if operate:
        print("      " + lp.health_text(operate[0]))
    checks.report(back and operate[0]["failed_starts"] == 0 and operate[0]["trial_left"] == 0,
                  f"after the restart: HEALTH OPERATE, slot {slot}, failed starts 0, trial time "
                  f"left 0")


def scenario_confirm(probe: TrialProbe, checks: lp.Checks, free: str) -> None:
    """Runs steps 6 to 11 of the confirm scenario.

    Args:
        probe: Probe used for the link.
        checks: Result counter.
        free: Name of the slot on trial, "A" or "B".
    """
    print("---- CONFIRM before the test image")
    _, answers, _ = probe.armed(CONFIRM, "CONFIRM before the test image")
    report_answers(checks, answers, [(lp.REFUSED, TESTS_NOT_PASSED)],
                   "ARM + CONFIRM before the test image")

    mark = len(probe.frames)
    step, answers = send_test_image(probe, good=True)
    checks.report(step == "END" and answers == up.DONE_OK,
                  f"test image, right check value: END DONE 0x00 (got at {step}: "
                  f"{up.code_text(answers)})")
    checks.report(probe.wait_until(mark, WAIT_EVENT_S,
                                   lambda fs: has_event(fs, EV_TESTS_PASSED)),
                  "EVENT 0x000A trial tests passed")

    report_answers(checks, probe.dangerous(CONFIRM, "CONFIRM without ARM"),
                   [(lp.REFUSED, NOT_ARMED)], "CONFIRM without ARM")
    named = probe.seq + 7
    report_answers(checks, probe.arm(CONFIRM, named), up.DONE_OK,
                   f"ARM naming CONFIRM with seq {named}")
    report_answers(checks, probe.dangerous(CONFIRM, "CONFIRM, not the ARM's sequence number"),
                   [(lp.REFUSED, NOT_ARMED)], "CONFIRM with another sequence number than the ARM's")

    print("---- ARM + CONFIRM")
    mark = len(probe.frames)
    _, answers, seq = probe.armed(CONFIRM, "CONFIRM")
    probe.wait_until(mark, WAIT_EVENT_S, lambda fs: has_event(fs, EV_CONFIRMED))
    after = probe.frames[mark:]
    event_at = next((i for i, f in enumerate(after)
                     if (lp.decode_event(f) or {}).get("code") == EV_CONFIRMED), None)
    done_at = next((i for i, f in enumerate(after)
                    if lp.decode_answer(f) == (seq, lp.DONE, 0x00)), None)
    report_answers(checks, answers, up.DONE_OK, "ARM + CONFIRM")
    checks.report(event_at is not None and done_at is not None and event_at < done_at,
                  "EVENT 0x0007 firmware confirmed, before CONFIRM's DONE")

    mark = len(probe.frames)
    probe.wait_until(mark, up.WAIT_HEALTH_S, lambda fs: bool(healths(fs)))
    reports = healths(probe.frames[mark:])
    if reports:
        print("      " + lp.health_text(reports[-1]))
    checks.report(bool(reports) and reports[-1]["mode"] == up.MODE_OPERATE and
                  up.SLOT_NAMES.get(reports[-1]["slot"]) == free and
                  reports[-1]["trial_left"] == 0 and reports[-1]["failed_starts"] == 0,
                  f"HEALTH: OPERATE, slot {free}, trial time left 0, failed starts 0")


def scenario_bad_image(probe: TrialProbe, checks: lp.Checks, running: str) -> None:
    """Runs steps 6 and 7 of the bad-image scenario.

    Args:
        probe: Probe used for the link.
        checks: Result counter.
        running: Name of the slot that ran before the trial, "A" or "B".
    """
    mark = len(probe.frames)
    step, answers = send_test_image(probe, good=False)
    checks.report(step == "END" and answers == [(lp.RECEIVED, 0x00), (lp.REFUSED, DAMAGED)],
                  f"test image, wrong check value: END RECEIVED, REFUSED 0x08 (got at {step}: "
                  f"{up.code_text(answers)})")
    given_up = probe.wait_until(mark, WAIT_EVENT_S, lambda fs: has_event(fs, EV_GIVEN_UP))
    found = [e["code"] for e in events(probe.frames[mark:])]
    checks.report(has_event(probe.frames[mark:], EV_TESTS_FAILED, DETAIL_TEST_IMAGE) and given_up
                  and found.index(EV_TESTS_FAILED) < found.index(EV_GIVEN_UP),
                  f"EVENT 0x000B detail 0x03, then EVENT 0x0008 (events: "
                  f"{', '.join(f'0x{c:04x}' for c in found)})")
    wait_restart_to(probe, checks, mark, running)


def scenario_timeout(probe: TrialProbe, checks: lp.Checks, running: str) -> None:
    """Runs steps 6 and 7 of the timeout scenario.

    Args:
        probe: Probe used for the link.
        checks: Result counter.
        running: Name of the slot that ran before the trial, "A" or "B".
    """
    started = [f["t"] for f in probe.frames
               if (lp.decode_event(f) or {}).get("code") == EV_TRIAL_STARTED]
    mark = len(probe.frames)
    step, answers = send_test_image(probe, good=True)
    checks.report(step == "END" and answers == up.DONE_OK,
                  f"test image, right check value: END DONE 0x00 (got at {step}: "
                  f"{up.code_text(answers)})")
    checks.report(probe.wait_until(mark, WAIT_EVENT_S,
                                   lambda fs: has_event(fs, EV_TESTS_PASSED)),
                  "EVENT 0x000A trial tests passed")

    print(f"---- no CONFIRM: waiting up to {WAIT_TIMEOUT_S:.0f} s for the trial time to run out")
    end = time.monotonic() + WAIT_TIMEOUT_S
    while time.monotonic() < end and not has_event(probe.frames[mark:], EV_GIVEN_UP):
        probe.wait_until(mark, min(PROGRESS_S, end - time.monotonic()),
                         lambda fs: has_event(fs, EV_GIVEN_UP))
        trial = [h for h in healths(probe.frames[mark:]) if h["mode"] == up.MODE_TRIAL]
        if trial:
            print(f"      trial time left {trial[-1]['trial_left']} s")
    given_up = [f["t"] for f in probe.frames[mark:]
                if (lp.decode_event(f) or {}).get("code") == EV_GIVEN_UP]
    span = (given_up[0] - started[0]) if (given_up and started) else None
    checks.report(span is not None and abs(span - TRIAL_S) <= TIMEOUT_TOLERANCE_S,
                  f"EVENT 0x0008 firmware given up {TRIAL_S} s after EVENT 0x0006 "
                  f"(measured: {'none' if span is None else f'{span:.1f} s'})")
    checks.report(not has_event(probe.frames[mark:], EV_TESTS_FAILED),
                  "no EVENT 0x000B: the trial ran out of time, no test failed")
    wait_restart_to(probe, checks, mark, running)


def main() -> int:
    """Parses the command line, runs the chosen scenario and prints the verdict.

    Returns:
        0 if every check passed, 1 otherwise.
    """
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("port", help="e.g. /dev/cu.usbmodem143203")
    p.add_argument("--images", required=True, type=Path)
    p.add_argument("--scenario", required=True, choices=("confirm", "bad-image", "timeout"))
    p.add_argument("--no-upload", action="store_true",
                   help="the other slot already holds its firmware: skip the upload")
    p.add_argument("--listen", type=float, default=0.0,
                   help="seconds to listen first (reset the board meanwhile to see BL: lines)")
    p.add_argument("--verbose", action="store_true", help="print every frame")
    a = p.parse_args()

    checks = lp.Checks()
    fd = lp.open_port(a.port)
    try:
        link = lp.Link(fd, verbose=a.verbose)
        probe = TrialProbe(link, checks)
        if a.listen > 0:
            print(f"listening {a.listen} s")
            link.collect(a.listen)
        running = start_health(probe, checks)
        if running is not None:
            free = "B" if running == "A" else "A"
            print(f"running slot {running}; trial of slot {free}; scenario {a.scenario}")
            if switch_to_trial(probe, checks, a.images, running, not a.no_upload):
                if a.scenario == "confirm":
                    scenario_confirm(probe, checks, free)
                elif a.scenario == "bad-image":
                    scenario_bad_image(probe, checks, running)
                else:
                    scenario_timeout(probe, checks, running)
    finally:
        os.close(fd)
    print("trial probe:", "all passed" if checks.failed == 0 else f"{checks.failed} FAILED")
    return 1 if checks.failed else 0


if __name__ == "__main__":
    sys.exit(main())
