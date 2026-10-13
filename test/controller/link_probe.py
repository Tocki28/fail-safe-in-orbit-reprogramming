#!/usr/bin/env python3
"""Bench tool: talk to the controller firmware over data link A and check its answers.

Frames as in the Interfaces document, Sec. 4.0 (Figure 2); answers as in Sec. 4.2; the link
at 115200 baud, 8 data bits, no parity, 1 stop bit (Sec. 6.2). On this bench data link A is
the ST-LINK's virtual COM port. Standard library only (termios), no pyserial.

Sends, to Destination 0x02 (controller), with sequence numbers 1, 2, 3, ...:
  1. GET STATUS (0x25) COUNT times      expect RECEIVED, then DONE code 0x00
  2. GET STATUS with one check-value bit flipped   expect nothing (frame dropped, REQ-107)
  3. an unknown command id (0x3F)       expect REFUSED code 0x06 only
  4. GET STATUS to Destination 0x01     expect nothing (not addressed to the controller)
Prints every frame received, decoded, and any other bytes as text (the bootloader's "BL:"
lines, when the board is reset while the tool listens). From build step 2c the firmware also
sends a HEALTH report every second (Sec. 5.4), and from step 2d EVENT reports; reports are
printed and left out of the "no answer" checks. Prints PASS or FAIL per check; exit status 1 if
any check fails.

Usage: link_probe.py PORT [--count N] [--listen SECONDS]
"""
import argparse
import os
import select
import struct
import sys
import termios
import time
import zlib

SYNC = b"\xa5\x5a"
VERSION = 0x01
TYPE_COMMAND, TYPE_ANSWER, TYPE_REPORT = 0x01, 0x02, 0x03
DEST_ONBOARD, DEST_CONTROLLER, DEST_GROUND = 0x01, 0x02, 0x03
HEADER = struct.Struct("<2sBBBBHII")   # Sync, Version, Type, Destination, Flags, Length, Sequence, Expiry
CHECK_BYTES = 4
PAYLOAD_MAX = 1024
ANSWER = struct.Struct("<IBB")          # sequence answered, kind, code
KINDS = {0x01: "RECEIVED", 0x02: "DONE", 0x03: "REFUSED", 0x04: "ALREADY DONE"}
RECEIVED, DONE, REFUSED = 0x01, 0x02, 0x03

# HEALTH report payload, fields in the order of the Interfaces Sec. 5.4 table: report number,
# onboard computer clock time, controller clock time, report kind, mode, firmware kind,
# firmware version, running slot, failed starts, trial time left, last self-test.
HEALTH = struct.Struct("<IIIBBBIBBHB")
REPORT_HEALTH = 0x01
MODES = {0x01: "OPERATE", 0x02: "TRIAL", 0x03: "FALLBACK"}
SLOTS = {0x01: "A", 0x02: "B", 0x03: "last-resort"}
HEALTH_FIELDS = ("number", "onboard_time", "time", "kind", "mode", "fw_kind", "fw_version",
                 "slot", "failed_starts", "trial_left", "self_test")

# EVENT report payload (Sec. 5.4 table, from step 2d): report number, onboard computer clock
# time, controller clock time, report kind 0x02, event code (2); then up to 32 detail bytes.
EVENT_HEAD = struct.Struct("<IIIBH")
REPORT_EVENT = 0x02
EVENT_NAMES = {0x0006: "trial started", 0x0007: "firmware confirmed", 0x0008: "firmware given up",
               0x000A: "trial tests passed", 0x000B: "trial tests failed"}

GET_STATUS = 0x25
UNKNOWN_COMMAND = 0x3F       # inside the controller's range 0x20-0x3F (Sec. 5.1), not assigned
IMPOSSIBLE = 0x06            # "Impossible values, or an unknown command" (Sec. 4.2)
# Far future. The firmware does not check expiry before step 2b.
EXPIRY = 0xFFFFFFFF
# Answers come within a few milliseconds (two 26-byte frames take 4.5 ms at 115200 baud);
# 0.5 s without one counts as none.
WAIT_S = 0.5
READ_BYTES = 4096


def open_port(path: str) -> int:
    """Raw 115200 8N1: no echo, no line editing, no character translation, no flow control.

    Opens the port non-blocking and clears both its queues. Reads return at once with whatever
    bytes are there (VMIN and VTIME are 0).

    Args:
        path: Device path of the serial port, e.g. /dev/cu.usbmodem143203.

    Returns:
        The open file descriptor.
    """
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    cc = termios.tcgetattr(fd)[6]
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0
    # CS8: 8 data bits; PARENB and CSTOPB left clear: no parity, 1 stop bit.
    cflag = termios.CS8 | termios.CREAD | termios.CLOCAL
    termios.tcsetattr(fd, termios.TCSANOW,
                      [0, 0, cflag, 0, termios.B115200, termios.B115200, cc])
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def build(ftype: int, dest: int, sequence: int, payload: bytes, expiry: int = 0) -> bytes:
    """Builds one frame: header, payload and the little-endian CRC-32 of both.

    Flags are 0. The check value covers every byte before it (Interfaces Sec. 4.0).

    Args:
        ftype: Frame type, TYPE_COMMAND, TYPE_ANSWER or TYPE_REPORT.
        dest: Destination byte, DEST_ONBOARD, DEST_CONTROLLER or DEST_GROUND.
        sequence: Sequence number of the frame.
        payload: Payload bytes.
        expiry: Expiry field; 0 by default.

    Returns:
        The complete frame as bytes.
    """
    head = HEADER.pack(SYNC, VERSION, ftype, dest, 0, len(payload), sequence, expiry) + payload
    return head + struct.pack("<I", zlib.crc32(head))


class Link:
    """Reads the port, splits frames from other bytes, prints both."""

    def __init__(self, fd: int, verbose: bool = True):
        """Stores the port and empty receive buffers.

        Args:
            fd: File descriptor of the open serial port.
            verbose: If False, frames are not printed (text lines still are).
        """
        self.fd = fd
        self.verbose = verbose   # False: frames are not printed (text lines still are)
        self.buf = bytearray()
        self.text = bytearray()

    def send(self, frame: bytes, what: str) -> None:
        """Writes one frame to the port, printing it first when verbose.

        Args:
            frame: Complete frame bytes.
            what: Short description printed with the frame.
        """
        if self.verbose:
            print(f"sent  {what}: {frame.hex(' ')}")
        os.write(self.fd, frame)

    def collect(self, seconds: float, stop=None) -> list:
        """Frames received within `seconds`; returns early once stop(frames) is true.

        Reads the port with select, splits what arrives into frames and text, and prints the text.

        Args:
            seconds: Longest time to listen.
            stop: Optional function taking the frames received so far; listening ends as
                soon as it returns true.

        Returns:
            The list of frames received, in arrival order. Each is a dict with the keys type, dest,
            flags, seq, expiry, payload, raw and t (arrival time, time.monotonic).
        """
        frames = []
        end = time.monotonic() + seconds
        while time.monotonic() < end and not (stop and stop(frames)):
            ready, _, _ = select.select([self.fd], [], [], max(0.0, end - time.monotonic()))
            if ready:
                self.buf += os.read(self.fd, READ_BYTES)
                frames += self._split()
        self._flush_text()
        return frames

    def _split(self) -> list:
        """Splits the receive buffer into frames and plain text.

        Bytes before a Sync are text. A header with a wrong version or a Length above PAYLOAD_MAX is
        skipped one byte at a time. A frame with a wrong check value is reported and dropped. An
        incomplete frame stays in the buffer for the next call.

        Returns:
            The complete frames with a correct check value, as dicts (see collect).
        """
        frames = []
        while True:
            start = self.buf.find(SYNC)
            if start < 0:
                keep = 1 if self.buf.endswith(SYNC[:1]) else 0
                self._text(self.buf[:len(self.buf) - keep])
                del self.buf[:len(self.buf) - keep]
                return frames
            self._text(self.buf[:start])
            del self.buf[:start]
            if len(self.buf) < HEADER.size:
                return frames
            _, version, ftype, dest, flags, length, seq, expiry = HEADER.unpack_from(self.buf)
            total = HEADER.size + length + CHECK_BYTES
            if version != VERSION or length > PAYLOAD_MAX:
                self._text(self.buf[:1])
                del self.buf[:1]
                continue
            if len(self.buf) < total:
                return frames
            raw = bytes(self.buf[:total])
            del self.buf[:total]
            check = struct.unpack_from("<I", raw, total - CHECK_BYTES)[0]
            if check != zlib.crc32(raw[:-CHECK_BYTES]):
                print(f"recv  frame with a bad check value, dropped: {raw.hex(' ')}")
                continue
            frame = dict(type=ftype, dest=dest, flags=flags, seq=seq, expiry=expiry,
                         payload=raw[HEADER.size:total - CHECK_BYTES], raw=raw,
                         t=time.monotonic())
            if self.verbose:
                print_frame(frame)
            frames.append(frame)

    def _text(self, data: bytes) -> None:
        """Adds bytes to the text buffer and prints every complete line.

        Args:
            data: Bytes that are not part of a frame.
        """
        self.text += data
        while b"\n" in self.text:
            line, _, rest = self.text.partition(b"\n")
            print("text  " + line.decode("ascii", "replace").rstrip("\r"))
            self.text = bytearray(rest)

    def _flush_text(self) -> None:
        """Prints what is left in the text buffer, if anything, and empties it.
        """
        if self.text.strip():
            print("text  " + self.text.decode("ascii", "replace").rstrip())
        self.text.clear()


def decode_answer(frame: dict):
    """Decodes an ANSWER frame's payload.

    Args:
        frame: A frame dict from collect.

    Returns:
        The tuple (sequence answered, kind, code), or None if the frame is not an answer of the
        right size.
    """
    if frame["type"] != TYPE_ANSWER or len(frame["payload"]) != ANSWER.size:
        return None
    return ANSWER.unpack(frame["payload"])


def decode_health(frame: dict):
    """The HEALTH report's fields as a dict, or None for any other frame.

    Args:
        frame: A frame dict from collect.

    Returns:
        A dict with the keys in HEALTH_FIELDS, or None.
    """
    p = frame["payload"]
    if frame["type"] != TYPE_REPORT or len(p) != HEALTH.size or p[12] != REPORT_HEALTH:
        return None
    return dict(zip(HEALTH_FIELDS, HEALTH.unpack(p)))


def decode_event(frame: dict):
    """The EVENT report's fields as a dict (number, onboard_time, time, code, detail), or None
    for any other frame.

    Args:
        frame: A frame dict from collect.

    Returns:
        The dict described above, with detail as bytes (up to 32), or None.
    """
    p = frame["payload"]
    if (frame["type"] != TYPE_REPORT or len(p) < EVENT_HEAD.size or
            len(p) > EVENT_HEAD.size + 32 or p[12] != REPORT_EVENT):
        return None
    number, onboard_time, time_s, _, code = EVENT_HEAD.unpack_from(p)
    return dict(number=number, onboard_time=onboard_time, time=time_s, code=code,
                detail=bytes(p[EVENT_HEAD.size:]))


def event_text(e: dict) -> str:
    """Formats an EVENT report as one line of text.

    Args:
        e: Dict from decode_event.

    Returns:
        The event code with its name, the detail bytes if any, and the controller time.
    """
    detail = f", detail {e['detail'].hex(' ')}" if e["detail"] else ""
    return (f"EVENT 0x{e['code']:04x} ({EVENT_NAMES.get(e['code'], 'unknown')}){detail}, "
            f"controller time {e['time']} s")


def health_text(h: dict) -> str:
    """Formats a HEALTH report as one line of text.

    Args:
        h: Dict from decode_health.

    Returns:
        Mode, running slot, firmware kind and version, failed starts, trial time left, last
        self-test and controller time.
    """
    return (f"HEALTH: mode {MODES.get(h['mode'], hex(h['mode']))}, running slot "
            f"{SLOTS.get(h['slot'], hex(h['slot']))}, firmware kind 0x{h['fw_kind']:02x} "
            f"v{h['fw_version']}, failed starts {h['failed_starts']}, trial time left "
            f"{h['trial_left']} s, self-test 0x{h['self_test']:02x}, controller time {h['time']} s")


def print_frame(frame: dict) -> None:
    """Prints one received frame, decoded as HEALTH, EVENT or answer, else as raw payload.

    Args:
        frame: A frame dict from collect.
    """
    health = decode_health(frame)
    if health is not None:
        print(f"recv  report seq {frame['seq']} to 0x{frame['dest']:02x}: {health_text(health)}")
        return
    event = decode_event(frame)
    if event is not None:
        print(f"recv  report seq {frame['seq']} to 0x{frame['dest']:02x}: {event_text(event)}")
        return
    answer = decode_answer(frame)
    if answer is None:
        print(f"recv  type 0x{frame['type']:02x} to 0x{frame['dest']:02x}, seq {frame['seq']}, "
              f"payload {frame['payload'].hex(' ')}")
        return
    answered, kind, code = answer
    print(f"recv  answer seq {frame['seq']} to 0x{frame['dest']:02x}: "
          f"{KINDS.get(kind, f'kind 0x{kind:02x}')} for seq {answered}, code 0x{code:02x}"
          f"   [{frame['raw'].hex(' ')}]")


def answer_frames(frames: list) -> list:
    """Answers only: the HEALTH reports that arrive every second are left out.

    Args:
        frames: Frame dicts from collect.

    Returns:
        The frames of type ANSWER.
    """
    return [f for f in frames if f["type"] == TYPE_ANSWER]


def answers_for(frames: list, seq: int) -> list:
    """Collects the answers to one command.

    Args:
        frames: Frame dicts from collect.
        seq: Sequence number of the command.

    Returns:
        A list of (kind, code) pairs, in arrival order, for the answers that name seq.
    """
    out = []
    for frame in frames:
        answer = decode_answer(frame)
        if answer is not None and answer[0] == seq:
            out.append((answer[1], answer[2]))
    return out


class Checks:
    def __init__(self):
        """Starts with no failed checks.
        """
        self.failed = 0

    def report(self, ok: bool, name: str) -> None:
        """Counts a check and prints PASS or FAIL for it.

        Args:
            ok: True if the check passed.
            name: Text printed after PASS or FAIL.
        """
        self.failed += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}")


def run(link: Link, count: int, checks: Checks) -> list:
    """Runs the four link checks: GET STATUS count times, a flipped check bit, an unknown command
    and a frame for another destination.

    Args:
        link: Link to the controller.
        count: Number of GET STATUS commands to send first.
        checks: Result counter that each check is reported to.

    Returns:
        All frames received during the checks.
    """
    seq = 0
    all_frames = []

    for _ in range(count):
        seq += 1
        link.send(build(TYPE_COMMAND, DEST_CONTROLLER, seq, bytes([GET_STATUS]), EXPIRY),
                  f"GET STATUS seq {seq}")
        frames = link.collect(WAIT_S, lambda f, s=seq: len(answers_for(f, s)) >= 2)
        all_frames += frames
        checks.report(answers_for(frames, seq) == [(RECEIVED, 0x00), (DONE, 0x00)],
                      f"GET STATUS seq {seq}: RECEIVED, then DONE 0x00")

    seq += 1
    bad = bytearray(build(TYPE_COMMAND, DEST_CONTROLLER, seq, bytes([GET_STATUS]), EXPIRY))
    bad[-1] ^= 0x01
    link.send(bytes(bad), f"GET STATUS seq {seq}, check value bit flipped")
    frames = link.collect(WAIT_S)
    all_frames += frames
    checks.report(answer_frames(frames) == [], f"seq {seq} with a bad check value: no answer (dropped)")

    seq += 1
    link.send(build(TYPE_COMMAND, DEST_CONTROLLER, seq, bytes([UNKNOWN_COMMAND]), EXPIRY),
              f"unknown command 0x{UNKNOWN_COMMAND:02x} seq {seq}")
    frames = link.collect(WAIT_S)
    all_frames += frames
    checks.report(answers_for(frames, seq) == [(REFUSED, IMPOSSIBLE)],
                  f"unknown command seq {seq}: REFUSED 0x06 only")

    seq += 1
    link.send(build(TYPE_COMMAND, DEST_ONBOARD, seq, bytes([GET_STATUS]), EXPIRY),
              f"GET STATUS seq {seq} to Destination 0x01")
    frames = link.collect(WAIT_S)
    all_frames += frames
    checks.report(answer_frames(frames) == [], f"seq {seq} not addressed to the controller: no answer")
    return all_frames


def main() -> int:
    """Parses the command line, runs the checks and prints the verdict.

    Returns:
        0 if every check passed, 1 otherwise.
    """
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("port", help="e.g. /dev/cu.usbmodem143203")
    p.add_argument("--count", type=int, default=3, help="GET STATUS commands to send")
    p.add_argument("--listen", type=float, default=0.0,
                   help="seconds to listen first (reset the board meanwhile to see BL: lines)")
    a = p.parse_args()

    checks = Checks()
    fd = open_port(a.port)
    try:
        link = Link(fd)
        if a.listen > 0:
            print(f"listening {a.listen} s")
            link.collect(a.listen)
        frames = run(link, a.count, checks)
        # The controller's own frames: one higher than its last (Interfaces Sec. 4.0).
        seqs = [f["seq"] for f in frames]
        checks.report(bool(seqs) and seqs == list(range(seqs[0], seqs[0] + len(seqs))),
                      f"controller's sequence numbers go up by one: {seqs}")
        checks.report(all(f["dest"] == DEST_ONBOARD and f["expiry"] == 0 for f in frames),
                      "answers and reports go to Destination 0x01 with Expiry 0")
    finally:
        os.close(fd)
    print("link probe:", "all passed" if checks.failed == 0 else f"{checks.failed} FAILED")
    return 1 if checks.failed else 0


if __name__ == "__main__":
    sys.exit(main())
