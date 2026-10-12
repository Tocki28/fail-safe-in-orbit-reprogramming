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
lines, when the board is reset while the tool listens). Prints PASS or FAIL per check; exit
status 1 if any check fails.

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
    """Raw 115200 8N1: no echo, no line editing, no character translation, no flow control."""
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
    head = HEADER.pack(SYNC, VERSION, ftype, dest, 0, len(payload), sequence, expiry) + payload
    return head + struct.pack("<I", zlib.crc32(head))


class Link:
    """Reads the port, splits frames from other bytes, prints both."""

    def __init__(self, fd: int):
        self.fd = fd
        self.buf = bytearray()
        self.text = bytearray()

    def send(self, frame: bytes, what: str) -> None:
        print(f"sent  {what}: {frame.hex(' ')}")
        os.write(self.fd, frame)

    def collect(self, seconds: float, stop=None) -> list:
        """Frames received within `seconds`; returns early once stop(frames) is true."""
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
                         payload=raw[HEADER.size:total - CHECK_BYTES], raw=raw)
            print_frame(frame)
            frames.append(frame)

    def _text(self, data: bytes) -> None:
        self.text += data
        while b"\n" in self.text:
            line, _, rest = self.text.partition(b"\n")
            print("text  " + line.decode("ascii", "replace").rstrip("\r"))
            self.text = bytearray(rest)

    def _flush_text(self) -> None:
        if self.text.strip():
            print("text  " + self.text.decode("ascii", "replace").rstrip())
        self.text.clear()


def decode_answer(frame: dict):
    if frame["type"] != TYPE_ANSWER or len(frame["payload"]) != ANSWER.size:
        return None
    return ANSWER.unpack(frame["payload"])


def print_frame(frame: dict) -> None:
    answer = decode_answer(frame)
    if answer is None:
        print(f"recv  type 0x{frame['type']:02x} to 0x{frame['dest']:02x}, seq {frame['seq']}, "
              f"payload {frame['payload'].hex(' ')}")
        return
    answered, kind, code = answer
    print(f"recv  answer seq {frame['seq']} to 0x{frame['dest']:02x}: "
          f"{KINDS.get(kind, f'kind 0x{kind:02x}')} for seq {answered}, code 0x{code:02x}"
          f"   [{frame['raw'].hex(' ')}]")


def answers_for(frames: list, seq: int) -> list:
    out = []
    for frame in frames:
        answer = decode_answer(frame)
        if answer is not None and answer[0] == seq:
            out.append((answer[1], answer[2]))
    return out


class Checks:
    def __init__(self):
        self.failed = 0

    def report(self, ok: bool, name: str) -> None:
        self.failed += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}")


def run(link: Link, count: int, checks: Checks) -> list:
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
    checks.report(frames == [], f"seq {seq} with a bad check value: no answer (dropped)")

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
    checks.report(frames == [], f"seq {seq} not addressed to the controller: no answer")
    return all_frames


def main() -> int:
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
                      "answers go to Destination 0x01 with Expiry 0")
    finally:
        os.close(fd)
    print("link probe:", "all passed" if checks.failed == 0 else f"{checks.failed} FAILED")
    return 1 if checks.failed else 0


if __name__ == "__main__":
    sys.exit(main())
