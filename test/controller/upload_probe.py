#!/usr/bin/env python3
"""Bench tool: upload firmware to the controller over data link A and check every answer.

UPLOAD BEGIN, CHUNK, END and CANCEL as in the Interfaces document, Sec. 5.1; answers as in
Sec. 4.2; HEALTH reports as in Sec. 5.4; images as in Sec. 5.5. Standard library only.

STOP-AND-WAIT: the next frame goes out only after the DONE or REFUSED of the previous one.
While the controller erases or writes its flash its receiver cannot run, and bytes that arrive
meanwhile are lost (src/controller/firmware/update-receiver/update_receiver.h).

  1. Waits for a HEALTH report: the running slot and the mode.
  2. Mode TRIAL: BEGIN, CHUNK, END and CANCEL must each be answered REFUSED 0x07 only. Stop.
  3. Mode OPERATE, into the slot that is not running (the "free" slot):
     a. CHUNK at offset 512 where 256 is next          REFUSED 0x06 only; CANCEL then DONE
     b. firmware-<free>.img, byte 0x600 inverted        END: RECEIVED, then REFUSED 0x08
     c. firmware-<free> signed with an unknown key A    END: RECEIVED, then REFUSED 0x09
     d. firmware-<running>.img (linked for the other)   END: RECEIVED, then REFUSED 0x0E
     e. firmware-<free>.img                             every command RECEIVED, then DONE 0x00
     The good image goes last, so the free slot holds it afterwards (make set-trial).
  4. HEALTH reports 1000 ms apart, measured while nothing is erased.
  5. The controller's sequence numbers go up by one over all its frames.
Prints PASS or FAIL per check; exit status 1 if any check fails.

Usage: upload_probe.py PORT --images DIR --test-images DIR [--listen SECONDS] [--verbose]
  --images       src/controller/build/images (firmware-A.img, firmware-B.img)
  --test-images  test/controller/build/images (firmware-<slot>-wrong-key-a.img)
"""
import argparse
import os
import struct
import sys
import time
from pathlib import Path

import link_probe as lp

BEGIN, CHUNK, END, CANCEL = 0x01, 0x02, 0x03, 0x04
CHUNK_BYTES = 256               # most data in one UPLOAD CHUNK (Sec. 5.1)
FLIP_OFFSET = 0x600             # same code byte as the bootloader bench tests (Makefile)
IMPOSSIBLE, TRIAL, DAMAGED, NOT_OPERATOR, OTHER_SLOT = 0x06, 0x07, 0x08, 0x09, 0x0E
DONE_OK = [(lp.RECEIVED, 0x00), (lp.DONE, 0x00)]
FINAL_KINDS = (lp.DONE, lp.REFUSED, 0x04)   # DONE, REFUSED, ALREADY DONE
MODE_OPERATE, MODE_TRIAL = 0x01, 0x02
SLOT_NAMES = {0x01: "A", 0x02: "B"}
FIRMWARE_KIND_MAIN = 0x02       # image kind "main firmware" (Sec. 5.5)
# Waits for the final answer. BEGIN erases 20 pages of up to 24.47 ms [DS12589 Rev 6, Table 46,
# p. 107]: 0.49 s. CHUNK writes 32 flash words of up to 83.35 us (same table): 2.7 ms. END runs
# the CRC-32 and two Ed25519 checks (about 40 ms at -O2, slower at the firmware's -Os). The
# margins cover the frames on the line (283 bytes: 25 ms) and the USB serial port.
WAIT_BEGIN_S = 2.0
WAIT_CHUNK_S = 1.0
WAIT_END_S = 3.0
WAIT_HEALTH_S = 2.5             # reports come every second
HEALTH_LISTEN_S = 3.5           # long enough for three reports
HEALTH_PERIOD_S = 1.0           # HEALTH_PERIOD_MS in health_monitor.h
HEALTH_TOLERANCE_S = 0.1        # USB serial delivery jitter


def begin(number: int, length: int) -> bytes:
    return struct.pack("<BHI", BEGIN, number, length)


def chunk(number: int, offset: int, data: bytes) -> bytes:
    return struct.pack("<BHI", CHUNK, number, offset) + data


def end(number: int) -> bytes:
    return struct.pack("<BH", END, number)


def cancel(number: int) -> bytes:
    return struct.pack("<BH", CANCEL, number)


def code_text(answers: list) -> str:
    return ", ".join(f"{lp.KINDS.get(k, hex(k))} 0x{c:02x}" for k, c in answers) or "no answer"


class Probe:
    def __init__(self, link: lp.Link, checks: lp.Checks):
        self.link = link
        self.checks = checks
        self.seq = 0
        self.frames = []

    def command(self, payload: bytes, wait: float, what: str) -> list:
        """Send one command and wait until its final answer arrives: stop-and-wait."""
        self.seq += 1
        seq = self.seq
        self.link.send(lp.build(lp.TYPE_COMMAND, lp.DEST_CONTROLLER, seq, payload, lp.EXPIRY),
                       f"{what} seq {seq}")
        frames = self.link.collect(
            wait, lambda f: any(k in FINAL_KINDS for k, _ in lp.answers_for(f, seq)))
        self.frames += frames
        return lp.answers_for(frames, seq)

    def health(self, seconds: float, first_only: bool = True) -> list:
        """(arrival time, fields) of the HEALTH reports within `seconds`; stops at the first
        one unless first_only is False."""
        stop = (lambda f: any(lp.decode_health(x) for x in f)) if first_only else None
        frames = self.link.collect(seconds, stop)
        self.frames += frames
        return [(f["t"], lp.decode_health(f)) for f in frames if lp.decode_health(f)]

    def upload(self, image: bytes, number: int, label: str):
        """BEGIN, every CHUNK, END. Returns (step that stopped it or "END", its answers,
        seconds for the whole upload, seconds for BEGIN, seconds for END)."""
        start = time.monotonic()
        answers = self.command(begin(number, len(image)), WAIT_BEGIN_S, f"UPLOAD BEGIN {label}")
        begin_s = time.monotonic() - start
        if answers != DONE_OK:
            return "BEGIN", answers, time.monotonic() - start, begin_s, 0.0
        for offset in range(0, len(image), CHUNK_BYTES):
            data = image[offset:offset + CHUNK_BYTES]
            answers = self.command(chunk(number, offset, data), WAIT_CHUNK_S, f"CHUNK {offset}")
            if answers != DONE_OK:
                return f"CHUNK at {offset}", answers, time.monotonic() - start, begin_s, 0.0
            if (offset // CHUNK_BYTES) % 16 == 15:
                print(f"      {offset + len(data)} of {len(image)} bytes")
        end_start = time.monotonic()
        answers = self.command(end(number), WAIT_END_S, "UPLOAD END")
        now = time.monotonic()
        return "END", answers, now - start, begin_s, now - end_start

    def expect_end(self, image: bytes, number: int, label: str, code: int) -> None:
        print(f"---- {label}")
        step, answers, _, _, _ = self.upload(image, number, label)
        self.checks.report(step == "END" and answers == [(lp.RECEIVED, 0x00), (lp.REFUSED, code)],
                           f"{label}: END RECEIVED, then REFUSED 0x{code:02x} "
                           f"(got at {step}: {code_text(answers)})")


def check_health(checks: lp.Checks, h: dict) -> None:
    print("      " + lp.health_text(h))
    checks.report(h["number"] == 0 and h["onboard_time"] == 0,
                  "HEALTH: report number and onboard computer time sent as 0")
    checks.report(h["mode"] in (MODE_OPERATE, MODE_TRIAL) and h["slot"] in SLOT_NAMES and
                  h["fw_kind"] == FIRMWARE_KIND_MAIN,
                  "HEALTH: mode OPERATE or TRIAL, running slot A or B, firmware kind 0x02")
    checks.report(h["trial_left"] == 0 and h["self_test"] == 0,
                  "HEALTH: trial time left 0 and self-test 0x00 (placeholders until 2d/2e)")


def trial_refusals(probe: Probe) -> None:
    for payload, name in ((begin(1, 1024), "BEGIN"), (chunk(1, 0, bytes(CHUNK_BYTES)), "CHUNK"),
                          (end(1), "END"), (cancel(1), "CANCEL")):
        answers = probe.command(payload, WAIT_CHUNK_S, f"UPLOAD {name} during a trial")
        probe.checks.report(answers == [(lp.REFUSED, TRIAL)],
                            f"TRIAL: UPLOAD {name} REFUSED 0x07 only (got {code_text(answers)})")


def wrong_offset(probe: Probe, image: bytes) -> None:
    print("---- CHUNK with a wrong offset")
    number = 1
    ok = probe.command(begin(number, len(image)), WAIT_BEGIN_S, "UPLOAD BEGIN") == DONE_OK
    ok = ok and probe.command(chunk(number, 0, image[:CHUNK_BYTES]), WAIT_CHUNK_S,
                              "CHUNK 0") == DONE_OK
    answers = probe.command(chunk(number, 2 * CHUNK_BYTES, image[2 * CHUNK_BYTES:3 * CHUNK_BYTES]),
                            WAIT_CHUNK_S, "CHUNK 512 (256 expected)")
    probe.checks.report(ok and answers == [(lp.REFUSED, IMPOSSIBLE)],
                        f"CHUNK at offset 512 where 256 is next: REFUSED 0x06 only "
                        f"(got {code_text(answers)})")
    answers = probe.command(cancel(number), WAIT_CHUNK_S, "UPLOAD CANCEL")
    probe.checks.report(answers == DONE_OK,
                        f"UPLOAD CANCEL: RECEIVED, then DONE 0x00 (got {code_text(answers)})")


def good_upload(probe: Probe, image: bytes, label: str) -> None:
    print(f"---- {label}")
    step, answers, total, begin_s, end_s = probe.upload(image, 5, label)
    chunks = (len(image) + CHUNK_BYTES - 1) // CHUNK_BYTES
    probe.checks.report(step == "END" and answers == DONE_OK,
                        f"{label}: {len(image)} bytes in {chunks} chunks, every command RECEIVED "
                        f"then DONE 0x00 (stopped at {step}: {code_text(answers)})")
    print(f"      total {total:.2f} s; BEGIN (erase) {begin_s:.3f} s; END (checks) {end_s:.3f} s")


def health_period(probe: Probe) -> None:
    reports = probe.health(HEALTH_LISTEN_S, first_only=False)
    times = [t for t, _ in reports]
    gaps = [b - a for a, b in zip(times, times[1:])]
    probe.checks.report(len(gaps) >= 1 and all(abs(g - HEALTH_PERIOD_S) <= HEALTH_TOLERANCE_S
                                               for g in gaps),
                        "HEALTH reports 1000 ms apart: " + ", ".join(f"{g * 1000:.0f} ms" for g in gaps))


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("port", help="e.g. /dev/cu.usbmodem143203")
    p.add_argument("--images", required=True, type=Path)
    p.add_argument("--test-images", required=True, type=Path)
    p.add_argument("--listen", type=float, default=0.0,
                   help="seconds to listen first (reset the board meanwhile to see BL: lines)")
    p.add_argument("--verbose", action="store_true", help="print every frame")
    a = p.parse_args()

    checks = lp.Checks()
    fd = lp.open_port(a.port)
    try:
        link = lp.Link(fd, verbose=a.verbose)
        probe = Probe(link, checks)
        if a.listen > 0:
            print(f"listening {a.listen} s")
            link.collect(a.listen)
        reports = probe.health(WAIT_HEALTH_S)
        checks.report(bool(reports), "a HEALTH report arrived")
        h = reports[-1][1] if reports else None
        if h is not None:
            check_health(checks, h)
        running = SLOT_NAMES.get(h["slot"]) if h is not None else None
        if running is None:
            print("running slot not known: no uploads sent")
        elif h["mode"] == MODE_TRIAL:
            trial_refusals(probe)
        else:
            free = "B" if running == "A" else "A"
            good = (a.images / f"firmware-{free}.img").read_bytes()
            flipped = bytearray(good)
            flipped[FLIP_OFFSET] ^= 0xFF
            wrong_key = (a.test_images / f"firmware-{free}-wrong-key-a.img").read_bytes()
            other_slot = (a.images / f"firmware-{running}.img").read_bytes()
            print(f"running slot {running}; uploading into slot {free}")
            wrong_offset(probe, good)
            probe.expect_end(bytes(flipped), 2, f"firmware-{free}.img, byte 0x600 inverted", DAMAGED)
            probe.expect_end(wrong_key, 3, f"firmware-{free} signed with an unknown key A",
                             NOT_OPERATOR)
            probe.expect_end(other_slot, 4, f"firmware-{running}.img (linked for slot {running})",
                             OTHER_SLOT)
            good_upload(probe, good, f"firmware-{free}.img")
        health_period(probe)
        seqs = [f["seq"] for f in probe.frames]
        checks.report(bool(seqs) and seqs == list(range(seqs[0], seqs[0] + len(seqs))),
                      f"controller's sequence numbers go up by one ({len(seqs)} frames, "
                      f"{seqs[0] if seqs else '-'} to {seqs[-1] if seqs else '-'})")
    finally:
        os.close(fd)
    print("upload probe:", "all passed" if checks.failed == 0 else f"{checks.failed} FAILED")
    return 1 if checks.failed else 0


if __name__ == "__main__":
    sys.exit(main())
