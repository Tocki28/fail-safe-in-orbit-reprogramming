#!/usr/bin/env python3
"""Flood sender: sends UDP datagrams to the host fsir-updater at a fixed rate (stress audit A.5).

Bench tool, not part of the operating software (REQ-101). One process sends one kind of flood:

  valid    GET STATUS to the onboard computer, a fresh sequence number each (the frame layout is
           Interfaces Sec. 4.0; the command id 0x25 is protocol.hpp command::kGetStatus);
  repeat   a small set of valid frames sent again and again, same sequence number, same bytes
           (REQ-031, REQ-065: the updater answers a resend from its memory);
  garbage  random bytes, bad check value, truncated frames, datagrams longer than
           frame::kMaxBytes (frame.hpp);
  mix      the three in turn, one datagram each.

The socket is either inherited (--fd: the same source address and port as the ground session of
the rig, because the updater sends answers to the source of the most recent good command,
command_gate.hpp) or a new one (foreign source). The send time of every fresh valid frame is
written to a file (--times) so the rig can compute the answer latency. At the end, or on SIGTERM,
the sender writes its statistics as JSON (--stats).

Usage: flood_sender.py --port N --kind K --rate R --seconds S --seed X --stats FILE
                       [--fd N | --foreign] [--times FILE]
Standard library only.
"""
import argparse
import json
import mmap
import os
import random
import signal
import socket
import struct
import sys
import time

sys.dont_write_bytecode = True   # the source tree must stay as it is: no __pycache__ in test/
HERE = os.path.dirname(os.path.abspath(__file__))
# bench_test.py is imported read-only for its frame builder (lines 86-102, build()).
sys.path.insert(0, os.path.join(HERE, "..", ".."))
import bench_test as B  # noqa: E402

# Sequence number ranges of the flood. The rig's own session counts up from 1 and stays far below
# them, so a flood number never collides with the rig's (bench choice, no source).
VALID_BASE = 0x40000000
REPEAT_BASE = 0x50000000
REPEAT_SET = 8              # distinct frames in the repeat flood (bench choice)
GARBAGE_POOL = 4096         # datagrams made before the run and sent in turn (bench choice)
RANDOM_MAX_BYTES = 1100     # longest random datagram, a little over frame::kMaxBytes = 1044
FRAME_MAX_BYTES = 1044      # frame::kMaxBytes = 16 + 1024 + 4 [frame.hpp:46-47]
OVERSIZE_MAX_BYTES = 9000   # below net.inet.udp.maxdgram = 9216 of this Mac (sysctl, measured)
HEADER_LENGTH_MAX = 1024    # frame::kPayloadMax [frame.hpp:44]
BATCH_S = 0.002             # pause between two sending passes (bench choice)
BURST_MAX = 4000            # most datagrams one pass sends, so a stall does not become a burst
DRAIN_MAX = 2000            # most answers a foreign socket drains per pass
SOCKET_BUFFER = 4 * 1024 * 1024   # below kern.ipc.maxsockbuf = 8388608 (sysctl, measured)
TIME_SLACK = 1.3            # room in the times file beyond rate x seconds
LAST_KEPT = 20              # last datagrams kept in the statistics, to reproduce a failure
KINDS = ("valid", "repeat", "garbage", "mix")

stop_flag = False


def on_term(signum, frame):
    """Asks the sending loop to stop (SIGTERM from the rig that started this process).

    A handler may only set a flag; the loop reads it every pass.

    Args:
        signum: signal number (unused)
        frame: interrupted stack frame (unused)

    Returns:
        None.
    """
    global stop_flag
    stop_flag = True


def times_bytes(rate, seconds):
    """Gives the size of the send-times file for a flood: one 8-byte time per fresh valid frame.

    The rig makes the file this big and the sender maps it, so both use this one function.

    Args:
        rate: target datagrams per second
        seconds: time to run, seconds

    Returns:
        The size in bytes.
    """
    return int(rate * seconds * TIME_SLACK) * 8 + 8


def valid_frame(sequence):
    """Builds one valid GET STATUS command to the onboard computer.

    The expiry is 0xFFFFFFFF as bench_test.py uses (bench_test.py:EXPIRY), so no expiry check
    refuses it.

    Args:
        sequence: ground sequence number (counts)

    Returns:
        The frame as bytes.
    """
    return B.build(B.COMMAND, B.ONBOARD, sequence, bytes([B.GET_STATUS]), B.EXPIRY)


def garbage_pool(rng):
    """Makes the pool of bad datagrams: four classes in turn.

    Classes: random bytes (half start with Sync and Version), a valid frame with one bit of the
    check value flipped, a valid frame cut short, and a datagram longer than frame::kMaxBytes.
    A seeded generator makes the pool reproducible from the seed in the log.

    Args:
        rng: a random.Random, seeded by the caller

    Returns:
        A list of GARBAGE_POOL datagrams (bytes).
    """
    pool = []
    for i in range(GARBAGE_POOL):
        kind = i % 4
        if kind == 0:
            data = bytes(rng.randrange(256) for _ in range(rng.randint(1, RANDOM_MAX_BYTES)))
            if i % 8 == 0:
                data = B.SYNC + bytes([B.VERSION]) + data
        elif kind == 1:
            frame = bytearray(valid_frame(rng.randrange(1, VALID_BASE)))
            frame[-1 - rng.randrange(4)] ^= 1 << rng.randrange(8)
            data = bytes(frame)
        elif kind == 2:
            frame = valid_frame(rng.randrange(1, VALID_BASE))
            data = frame[:rng.randint(1, len(frame) - 1)]
        else:
            size = rng.randint(FRAME_MAX_BYTES + 1, OVERSIZE_MAX_BYTES)
            head = B.HEADER.pack(B.SYNC, B.VERSION, B.COMMAND, B.ONBOARD, 0, HEADER_LENGTH_MAX,
                                 rng.randrange(1, VALID_BASE), B.EXPIRY)
            data = head + bytes(rng.randrange(256) for _ in range(size - len(head)))
        pool.append(data)
    return pool


class Source:
    """Makes the next datagram of the chosen flood kind."""

    def __init__(self, kind, seed, times):
        """Prepares the pools and counters of one flood kind.

        Args:
            kind: one of KINDS
            seed: seed of the garbage generator
            times: writable mmap for the send times of fresh valid frames, or None

        Returns:
            None.
        """
        self.kind = kind
        self.times = times
        self.count = 0           # datagrams made
        self.valid_count = 0     # fresh valid frames made
        self.pool = garbage_pool(random.Random(seed)) if kind in ("garbage", "mix") else []
        self.repeats = [valid_frame(REPEAT_BASE + k) for k in range(REPEAT_SET)]

    def fresh(self):
        """Makes a valid frame with a new sequence number and notes its send time.

        The time is written before the frame is sent, so an answer never arrives before it.

        Returns:
            The frame as bytes.
        """
        index = self.valid_count
        self.valid_count += 1
        if self.times is not None and (index + 1) * 8 <= len(self.times):
            struct.pack_into("<d", self.times, index * 8, time.monotonic())
        return valid_frame(VALID_BASE + index)

    def next(self):
        """Makes the next datagram.

        Returns:
            The datagram as bytes.
        """
        i = self.count
        self.count += 1
        kind = self.kind
        if kind == "mix":
            kind = ("valid", "repeat", "garbage")[i % 3]
        if kind == "valid":
            return self.fresh()
        if kind == "repeat":
            return self.repeats[i % REPEAT_SET]
        return self.pool[i % GARBAGE_POOL]


def drain(sock):
    """Reads and counts the answers waiting on a foreign socket, without blocking.

    Only a foreign socket is drained here; an inherited one is read by the rig.

    Args:
        sock: the foreign UDP socket (non-blocking)

    Returns:
        The number of datagrams read.
    """
    count = 0
    while count < DRAIN_MAX:
        try:
            sock.recv(4096)
        except OSError:
            break
        count += 1
    return count


def run(sock, source, rate, seconds, port, foreign):
    """Sends at the given rate for the given time, or until SIGTERM.

    Each pass sends the datagrams that are due by the schedule rate x elapsed, at most
    BURST_MAX; the pass count per second is kept to show the rate reached. The loop is bounded by
    seconds (B2).

    Args:
        sock: UDP socket to send from
        source: the Source that makes datagrams
        rate: target datagrams per second
        seconds: time to run, seconds
        port: the updater's UDP port on 127.0.0.1
        foreign: True if sock is the sender's own, so its answers are drained and counted

    Returns:
        A dict of statistics.
    """
    addr = ("127.0.0.1", port)
    start = time.monotonic()
    per_second = [0] * (int(seconds) + 3)
    attempted = errors = answers = 0
    last = []
    while not stop_flag:
        now = time.monotonic()
        elapsed = now - start
        if elapsed >= seconds:
            break
        due = min(int(elapsed * rate), attempted + BURST_MAX)
        while attempted < due:
            data = source.next()
            attempted += 1
            try:
                sock.sendto(data, addr)
                per_second[int(elapsed)] += 1
            except OSError:
                errors += 1
            last.append(data.hex())
            if len(last) > LAST_KEPT:
                del last[0]
        if foreign:
            answers += drain(sock)
        time.sleep(BATCH_S)
    elapsed = time.monotonic() - start
    sent = attempted - errors
    return dict(kind=source.kind, rate_target=rate, seconds_target=seconds, elapsed_s=elapsed,
                attempted=attempted, sent=sent, send_errors=errors,
                rate_reached=sent / elapsed if elapsed > 0 else 0.0, per_second=per_second,
                foreign_answers=answers, valid_frames=source.valid_count, last_hex=last)


def main():
    """Reads the command line, runs one flood and writes the statistics.

    Returns:
        Exit status: 0 done, 2 bad command line.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--kind", choices=KINDS, required=True)
    parser.add_argument("--rate", type=int, default=10000)
    parser.add_argument("--seconds", type=float, required=True)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--stats", required=True)
    parser.add_argument("--times", default=None)
    parser.add_argument("--fd", type=int, default=None)
    parser.add_argument("--foreign", action="store_true")
    args = parser.parse_args()
    if (args.fd is None) == (not args.foreign):
        print("exactly one of --fd and --foreign", file=sys.stderr)
        return 2
    signal.signal(signal.SIGTERM, on_term)
    if args.foreign:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, SOCKET_BUFFER)
        sock.setblocking(False)
    else:
        sock = socket.socket(fileno=args.fd)
    times = None
    if args.times is not None:
        handle = open(args.times, "r+b")
        times = mmap.mmap(handle.fileno(), times_bytes(args.rate, args.seconds))
    stats = run(sock, Source(args.kind, args.seed, times), args.rate, args.seconds, args.port,
                args.foreign)
    stats["seed"] = args.seed
    with open(args.stats, "w") as out:
        json.dump(stats, out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
