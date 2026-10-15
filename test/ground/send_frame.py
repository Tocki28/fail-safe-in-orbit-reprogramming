#!/usr/bin/env python3
"""Bench test tool: sends one crafted command frame to the onboard computer and prints the answers.

TEST-ONLY CODE (code rules A10, A10b; REQ-101): it lives in test/ground/ and never in flight
software. The ground console (src/ground/console.py) builds only valid commands, so it cannot
check the onboard computer's refusals of damaged, mis-versioned, unarmed, wrongly armed, repeated
or out-of-place commands. This tool builds those frames. It carries its own frame builder and
parser and does not import the console, because the console refuses to build a malformed command,
which is exactly what this tool must be able to send.

Frames as in the Interfaces document, Sec. 4.0; answers as in Sec. 4.2; commands as in Sec. 5.1;
arming as in Sec. 5.2. Python standard library only. One frame per UDP datagram.

Sequence numbers come from the console's state file (default ~/.fsir/console-seq) by the console's
own rule, because the onboard computer refuses an ARM or a dangerous command whose ground sequence
number it has already seen since it started (REFUSED 0x04, Sec. 4.0, REQ-065). Every number taken
is saved to the file before its frame goes out, and the file is never lowered, so a later console
run continues above every number this tool used. Global options come before the subcommand:

    send_frame.py [--host H] [--port P] [--listen S] [--seq N | --seq-file F]
                  [--expiry E | --expiry-in S]  raw | armed | repeat | cancel  ...

Exit status: 0 when at least one answer to a sequence number the tool sent arrived, 1 when none
did, 2 for a bad argument or an unwritable state file.
"""
import argparse
import os
import socket
import struct
import sys
import time
import zlib

# ---- Frame, Sec. 4.0 -----------------------------------------------------------------
SYNC = b"\xa5\x5a"                      # Sec. 4.0, field table: start of a frame
VERSION = 0x01                          # Sec. 4.0, field table: frame format version
TYPE_COMMAND, TYPE_ANSWER, TYPE_REPORT = 0x01, 0x02, 0x03   # Sec. 4.0: Type field
DEST_ONBOARD, DEST_CONTROLLER = 0x01, 0x02                  # Sec. 4.0: Destination field
# Sec. 4.0: Sync, Version, Type, Dest, Flags, Length, Seq, Expiry
HEADER = struct.Struct("<2sBBBBHII")
CHECK = struct.Struct("<I")             # Sec. 4.0: CRC-32 at the end, little-endian
PAYLOAD_MAX = 1024                      # Sec. 4.0: Length is 0 to 1024 for a valid frame
SEQ_MAX = 2**32 - 1                     # Sec. 4.0: sequence numbers run 1 to 2^32-1
U8_MAX, U16_MAX = 0xFF, 0xFFFF          # largest value of a 1-byte and a 2-byte field
# A datagram of the largest size this tool sends: Ethernet MTU 1500 bytes minus IPv4 header 20 and
# UDP header 8 [RFC 791, Sec. 3.1; RFC 768], so one frame is never fragmented on the bench link.
DATAGRAM_MAX = 1472
# Largest UDP datagram over IPv4 [RFC 768; RFC 791, Sec. 3.1]
RECV_BYTES = 65507
# --flip-byte XORs this into one byte: one bit changed always damages the frame
FLIP_MASK = 0x01

# ---- Answers, Sec. 4.2 ---------------------------------------------------------------
ANSWER = struct.Struct("<IBB")          # Sec. 4.2: sequence answered (4), kind (1), code (1)
KINDS = {0x01: "RECEIVED", 0x02: "DONE", 0x03: "REFUSED", 0x04: "ALREADY DONE"}   # Sec. 4.2: kinds

# ---- Commands, Sec. 5.1 --------------------------------------------------------------
UPLOAD_CANCEL = 0x04                    # Sec. 5.1, command table: UPLOAD CANCEL, image number (2)
# Sec. 5.1, command table: ARM, command id (1), its sequence number (4)
ARM = 0x0D
ARM_FIELDS = struct.Struct("<BBI")      # Sec. 5.1: ARM payload = 0x0D, command id (1), sequence (4)
CANCEL_FIELDS = struct.Struct("<BH")    # Sec. 5.1: UPLOAD CANCEL payload = 0x04, image number (2)

# ---- Reports, Sec. 5.4 ---------------------------------------------------------------
REPORT_KINDS = {0x01: "HEALTH", 0x02: "EVENT"}   # Sec. 5.4: report kind byte
REPORT_KIND_OFFSET = 4          # Sec. 5.4: kind byte follows the controller clock (4)
ADDED_LEN = 8                   # Sec. 5.4: onboard computer's added report number (4) and time (4)
HEALTH_LEN = 16                 # Sec. 5.4: HEALTH body as the controller sends it
HEALTH_LEN_ADDED = ADDED_LEN + HEALTH_LEN   # Sec. 5.4: HEALTH once the two fields are added

# ---- Bench values --------------------------------------------------------------------
DEFAULT_HOST, DEFAULT_PORT = "192.168.7.1", 47001   # brief: onboard computer bench address and port
DEFAULT_LISTEN_S = 3.0          # brief: a command normally answers within a few ms; 3 s is lost
LISTEN_MAX_S = 600.0            # bench cap on any listen window, so no wait is unbounded (rule B2)
# Bench cap on --gap: the ARM lifetime is TBD (Sec. 5.2); 60 s only bounds the wait (rule B2).
# The 60 s bound is accepted by the owner, 15 Oct 2026 (decision 44).
GAP_MAX_S = 60.0
TIMES_DEFAULT, TIMES_MAX = 2, 100   # repeat: default is one resend; the cap bounds the loop (B2)
DEFAULT_SEQ_FILE = os.path.join("~", ".fsir", "console-seq")   # same file as the console
MS_PER_S = 1000                 # milliseconds per second, for the time stamp
SOCKET_POLL_S = 0.001           # smallest receive timeout, so a zero wait still polls once
EXIT_ANSWERED, EXIT_SILENT, EXIT_BAD_ARGS = 0, 1, 2   # exit statuses, see the module docstring


class ToolError(Exception):
    """An argument or a state file the tool cannot use; main() turns it into exit status 2."""


def build_frame(version, ftype, dest, flags, seq, expiry, payload):
    """One frame of bytes with the CRC-32 computed over every byte before it (Sec. 4.0).

    Why: the console's builder cannot make a frame with a wrong version or flags, so this tool
    builds its own. The Length field is always the payload's real length; the damaged frames come
    from flip_byte(), after the CRC is in place.

    Args:
        version: Version field, 0 to 255 (the valid value is 0x01).
        ftype: Type field, 0 to 255 (0x01 command).
        dest: Destination field, 0 to 255 (0x01 onboard computer, 0x02 controller).
        flags: Flags field, 0 to 255 (0 for a ground command).
        seq: sequence number, 0 to 2^32-1.
        expiry: expiry time, seconds of Unix time on the bench; 0 for none.
        payload: payload bytes.

    Returns:
        The frame: 16-byte header, payload, 4-byte CRC-32 (like zlib.crc32).
    """
    head = HEADER.pack(SYNC, version, ftype, dest, flags, len(payload), seq, expiry) + payload
    return head + CHECK.pack(zlib.crc32(head))


def parse_frame(data):
    """One datagram -> frame dict, or None when it is not a good frame (Sec. 4.0).

    Why: the answers have to be decoded to tell whether the onboard computer answered a number
    this tool sent. A receiver drops a frame whose check value or version it does not accept
    (Sec. 4.0), so the tool does the same and prints the rest as undecodable.

    Args:
        data: the datagram bytes.

    Returns:
        dict with type, dest, seq and payload, or None.
    """
    if len(data) < HEADER.size + CHECK.size:
        return None
    sync, version, ftype, dest, _, length, seq, _ = HEADER.unpack_from(data)
    if sync != SYNC or version != VERSION or length > PAYLOAD_MAX:
        return None
    if len(data) != HEADER.size + length + CHECK.size:
        return None
    if CHECK.unpack_from(data, len(data) - CHECK.size)[0] != zlib.crc32(data[:-CHECK.size]):
        return None
    return dict(type=ftype, dest=dest, seq=seq, payload=data[HEADER.size:HEADER.size + length])


def flip_byte(frame, index):
    """Damage a finished frame by XOR of one byte with FLIP_MASK (CRC bytes included).

    Why: the onboard computer must drop a damaged frame (Sec. 4.0, REQ-107). Flipping a byte after
    the CRC is computed makes the check value disagree with the content, whichever byte is hit.

    Args:
        frame: the finished frame bytes.
        index: byte position, 0 to len(frame) - 1.

    Returns:
        A copy of the frame with that byte changed.

    Raises:
        ToolError: the position is outside the frame.
    """
    if not 0 <= index < len(frame):
        raise ToolError(f"--flip-byte {index} is outside the frame (0 to {len(frame) - 1})")
    damaged = bytearray(frame)
    damaged[index] ^= FLIP_MASK
    return bytes(damaged)


def read_last_seq(path):
    """The last sequence number the state file holds, or None when it cannot be used.

    Why: the console's rule accepts the file only when it is a decimal integer from 1 to 2^32-2
    (load_first_seq in src/ground/console.py), so this tool applies the same test.

    Args:
        path: state file path.

    Returns:
        The number, or None when the file is absent, unreadable or out of range.
    """
    try:
        with open(path, "r", encoding="ascii") as f:
            last = int(f.read().strip(), 10)
    except (OSError, ValueError, UnicodeDecodeError):
        return None
    return last if 1 <= last < SEQ_MAX else None


def first_seq(path, now=None):
    """Choose the first sequence number of this run by the console's rule (Sec. 4.0, REQ-065).

    Why: the onboard computer refuses a number it has seen since it started, so a run must start
    above every earlier run. The state file holds the last number used: start one above it. When
    the file is missing or unusable, start from the clock (seconds since the Unix epoch), which
    only grows; this is the console's rule and carries the console's assumption (an earlier run
    sent fewer frames than seconds passed), owner to accept.

    Args:
        path: state file path.
        now: current time, seconds since the Unix epoch; the clock if None.

    Returns:
        (first, note): the first number to use; a note for the operator when the file was not
        usable, else None.
    """
    last = read_last_seq(path)
    if last is not None:
        return last + 1, None
    now = int(time.time()) if now is None else int(now)
    return now, f"no usable sequence state file {path}: starting from clock time {now}"


def save_seq(path, seq):
    """Write max(file, seq) to the state file, atomically (Sec. 4.0, REQ-065).

    Why: the console continues from the file, so the file must never fall below a number any tool
    has used. A temporary file in the same folder is written, forced to disk and renamed over the
    state file, as the console does, so a crash leaves either the old or the new content.

    Args:
        path: state file path; its folder is created if missing.
        seq: the sequence number just taken, 1 to 2^32-1.

    Raises:
        ToolError: the file could not be written (a number not saved could be reused).
    """
    seq = max(seq, read_last_seq(path) or 0)
    folder = os.path.dirname(path) or "."
    tmp = f"{path}.tmp{os.getpid()}"
    try:
        os.makedirs(folder, exist_ok=True)
        # mode 0600, owner only, as the console writes it
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        try:
            os.write(fd, f"{seq}\n".encode("ascii"))
            os.fsync(fd)
        finally:
            os.close(fd)
        os.replace(tmp, path)
    except OSError as e:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise ToolError(f"cannot save the sequence number to {path}: {e}") from None


class SeqSource:
    """Hands out sequence numbers one at a time and saves each before it is used."""

    def __init__(self, first, path):
        """Start the counter.

        Args:
            first: the first number to hand out, 1 to 2^32-1.
            path: state file where every number is saved.
        """
        self.next = first
        self.path = path

    def take(self):
        """Take the next number and save it (Sec. 4.0: numbers run 1 to 2^32-1, REQ-065).

        Why: the number is saved before its frame goes out, so a crash after the send can never
        lead to the same number again.

        Returns:
            The number.

        Raises:
            ToolError: the numbers are used up, or the state file could not be written.
        """
        if not 1 <= self.next <= SEQ_MAX:
            raise ToolError(f"sequence number {self.next} is outside 1 to {SEQ_MAX}")
        save_seq(self.path, self.next)
        taken = self.next
        self.next += 1
        return taken


def stamp(t=None):
    """Clock time of day with milliseconds, for the printed lines.

    Args:
        t: seconds since the Unix epoch; the clock if None.

    Returns:
        Text like "14:03:07.512" (local time).
    """
    t = time.time() if t is None else t
    return time.strftime("%H:%M:%S", time.localtime(t)) + f".{int((t % 1) * MS_PER_S):03d}"


def report_kind_text(payload):
    """Name of a report's kind from its payload (Sec. 5.4).

    Why: a report arrives with the onboard computer's two added fields in front, or without them.
    A HEALTH report is a fixed length, so with or without is told by length; an EVENT report is
    told by its kind byte after the added fields (the console's rule, decode_report()).

    Args:
        payload: the report frame's payload bytes.

    Returns:
        "HEALTH", "EVENT", or "kind 0x.." for a kind byte in no table, or "no kind" if too short.
    """
    offset = REPORT_KIND_OFFSET
    if len(payload) == HEALTH_LEN_ADDED or (
            len(payload) > ADDED_LEN + REPORT_KIND_OFFSET
            and payload[ADDED_LEN + REPORT_KIND_OFFSET] in REPORT_KINDS):
        offset += ADDED_LEN
    if len(payload) <= offset:
        return "no kind"
    kind = payload[offset]
    return REPORT_KINDS.get(kind, f"kind 0x{kind:02x}")


class Session:
    """One UDP socket to the onboard computer; sends frames, prints what comes back."""

    def __init__(self, host, port, out=None):
        """Open the socket, bound to an ephemeral local port.

        Why: the onboard computer answers to the source address of the datagram (brief), so all
        frames of one run leave from one socket and the answers return to it.

        Args:
            host: onboard computer address.
            port: onboard computer UDP port.
            out: file the lines are printed to; sys.stdout if None.
        """
        self.addr = (host, port)
        self.out = out
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("", 0))
        self.sent = set()                   # sequence numbers sent
        self.answered = set()               # sent numbers an answer arrived for

    def say(self, text):
        """Print one line.

        Args:
            text: the line.
        """
        print(text, file=self.out or sys.stdout, flush=True)

    def send(self, frame):
        """Send one frame and print its line: time, sequence number, destination, payload hex.

        Why: the line is the record of what the onboard computer was given. It is read back from
        the bytes sent, not from the arguments, so a damaged frame prints what really went out.
        A frame too damaged to read its header from is printed as raw bytes.

        Args:
            frame: the finished frame bytes.
        """
        self.sock.sendto(frame, self.addr)
        if len(frame) >= HEADER.size:
            _, _, _, dest, _, length, seq, _ = HEADER.unpack_from(frame)
            payload = frame[HEADER.size:HEADER.size + length]
            self.sent.add(seq)
            self.say(f"sent {stamp()} seq={seq} dest=0x{dest:02x} payload={payload.hex()}")
        else:
            self.say(f"sent {stamp()} unreadable frame of {len(frame)} bytes")

    def show(self, data):
        """Print one received datagram: an answer decoded, a report by length and kind, else
        "undecodable (N bytes)". An answer to a number this tool sent is remembered.

        Args:
            data: the datagram bytes.
        """
        frame = parse_frame(data)
        if frame and frame["type"] == TYPE_ANSWER and len(frame["payload"]) == ANSWER.size:
            answered, kind, code = ANSWER.unpack(frame["payload"])
            words = KINDS.get(kind, f"unknown kind 0x{kind:02x}")
            if answered in self.sent:
                self.answered.add(answered)
            self.say(f"recv {stamp()} answer seq={answered} {words} code=0x{code:02x}")
        elif frame and frame["type"] == TYPE_REPORT:
            self.say(f"recv {stamp()} report {len(frame['payload'])} bytes, "
                     f"{report_kind_text(frame['payload'])}")
        else:
            self.say(f"recv {stamp()} undecodable ({len(data)} bytes)")

    def wait(self, seconds):
        """Listen for `seconds`, printing every datagram that arrives, then return.

        Why: every wait has a fixed upper bound (rule B2): the end time is fixed before the first
        receive, and each receive waits at most until then.

        Args:
            seconds: listening time, 0 to LISTEN_MAX_S, seconds.
        """
        end = time.monotonic() + seconds
        while True:
            left = end - time.monotonic()
            self.sock.settimeout(max(left, SOCKET_POLL_S))
            try:
                data, _ = self.sock.recvfrom(RECV_BYTES)
            except (socket.timeout, BlockingIOError):
                return
            except ConnectionResetError:    # an ICMP "port unreachable" from an earlier send
                data = None
            if data is not None:
                self.show(data)
            if time.monotonic() >= end:
                return

    def close(self):
        """Close the socket."""
        self.sock.close()


def command_frame(args, dest, seq, payload):
    """A command frame (Type 0x01) with the run's expiry and the version and flags in `args`.

    Args:
        args: parsed options; args.expiry (seconds of Unix time, 0 for none), and optionally
            args.version and args.flags (0 to 255), else the valid version and flags 0.
        dest: Destination field, 0 to 255.
        seq: sequence number.
        payload: payload bytes.

    Returns:
        The finished frame bytes.
    """
    return build_frame(getattr(args, "version", VERSION), TYPE_COMMAND, dest,
                       getattr(args, "flags", 0), seq, args.expiry, payload)


def cmd_raw(args, seqs, sess):
    """raw: one command frame, optionally with a wrong version, flags or a damaged byte.

    Args:
        args: parsed options: dest, id, fields (bytes), flags, version, flip_byte or None.
        seqs: the SeqSource.
        sess: the Session.
    """
    frame = command_frame(args, args.dest, seqs.take(), bytes([args.id]) + args.fields)
    if args.flip_byte is not None:
        frame = flip_byte(frame, args.flip_byte)
    sess.send(frame)


def cmd_armed(args, seqs, sess):
    """armed: ARM naming the command, a wait of --gap seconds, then the command (Sec. 5.2).

    Why: the ARM names a command identifier and the sequence number the command will carry
    (Sec. 5.1, REQ-029, REQ-112). The command takes the number after the ARM's, as the console
    does. --arm-names and --arm-id make the ARM name something else; --no-arm leaves it out.

    Args:
        args: parsed options: dest, cmd, fields, gap, no_arm, arm_names or None, arm_id or None.
        seqs: the SeqSource.
        sess: the Session.
    """
    arm_frame = None
    if not args.no_arm:
        arm_seq = seqs.take()
        named = args.arm_names if args.arm_names is not None else seqs.next
        ident = args.arm_id if args.arm_id is not None else args.cmd
        arm_frame = command_frame(args, args.dest, arm_seq, ARM_FIELDS.pack(ARM, ident, named))
    cmd_frame = command_frame(args, args.dest, seqs.take(), bytes([args.cmd]) + args.fields)
    if arm_frame is not None:
        sess.send(arm_frame)
        sess.wait(args.gap)
    sess.send(cmd_frame)


def cmd_repeat(args, seqs, sess):
    """repeat: the same frame, same sequence number, sent --times times --gap seconds apart.

    Why: a command sent again with a sequence number already run is answered ALREADY DONE and is
    not run twice (Sec. 4.2, REQ-031); a repeated ARM or dangerous command is refused 0x04
    (Sec. 4.0, REQ-065).

    Args:
        args: parsed options: dest, id, fields, gap, times.
        seqs: the SeqSource.
        sess: the Session.
    """
    frame = command_frame(args, args.dest, seqs.take(), bytes([args.id]) + args.fields)
    for count in range(args.times):
        if count:
            sess.wait(args.gap)
        sess.send(frame)


def cmd_cancel(args, seqs, sess):
    """cancel: UPLOAD CANCEL (0x04, image number (2)) to the onboard computer (Sec. 5.1, REQ-051).

    Args:
        args: parsed options: number (image number, 0 to 65535).
        seqs: the SeqSource.
        sess: the Session.
    """
    payload = CANCEL_FIELDS.pack(UPLOAD_CANCEL, args.number)
    sess.send(command_frame(args, DEST_ONBOARD, seqs.take(), payload))


def int_in(low, high):
    """An argparse type: an integer (decimal or 0x hex) from `low` to `high`.

    Args:
        low: smallest allowed value.
        high: largest allowed value.

    Returns:
        A function from text to int that raises argparse.ArgumentTypeError when out of range.
    """
    def convert(text):
        """Convert one argument text.

        Args:
            text: the argument as typed.

        Returns:
            The integer.
        """
        try:
            value = int(text, 0)
        except ValueError:
            raise argparse.ArgumentTypeError(f"{text!r} is not an integer") from None
        if not low <= value <= high:
            raise argparse.ArgumentTypeError(f"{value} is outside {low} to {high}")
        return value
    return convert


def seconds_in(high):
    """An argparse type: a number of seconds from 0 to `high` (every wait is bounded, rule B2).

    Args:
        high: largest allowed value, seconds.

    Returns:
        A function from text to float that raises argparse.ArgumentTypeError when out of range.
    """
    def convert(text):
        """Convert one argument text.

        Args:
            text: the argument as typed.

        Returns:
            The seconds.
        """
        try:
            value = float(text)
        except ValueError:
            raise argparse.ArgumentTypeError(f"{text!r} is not a number") from None
        if not 0.0 <= value <= high:
            raise argparse.ArgumentTypeError(f"{value} is outside 0 to {high:g} s")
        return value
    return convert


def hex_bytes(text):
    """An argparse type: hexadecimal text (spaces allowed) -> bytes.

    Args:
        text: such as "0a 0b" or "0a0b"; empty for no bytes.

    Returns:
        The bytes, at most DATAGRAM_MAX - header - CRC - 1 so the datagram fits DATAGRAM_MAX.
    """
    try:
        data = bytes.fromhex(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"{text!r} is not hexadecimal") from None
    if len(data) > DATAGRAM_MAX - HEADER.size - CHECK.size - 1:
        raise argparse.ArgumentTypeError("fields too long for one datagram")
    return data


def make_parser():
    """The command-line parser: global options and the four subcommands.

    Returns:
        The argparse.ArgumentParser.
    """
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=DEFAULT_HOST,
                   help="onboard computer address (default %(default)s)")
    p.add_argument("--port", type=int_in(1, U16_MAX), default=DEFAULT_PORT,
                   help="onboard computer UDP port (default %(default)s)")
    p.add_argument("--listen", type=seconds_in(LISTEN_MAX_S), default=DEFAULT_LISTEN_S, metavar="S",
                   help="seconds to listen for answers after the last frame (default %(default)s)")
    p.add_argument("--seq", type=int_in(1, SEQ_MAX), default=None, metavar="N",
                   help="first sequence number to use (default: one above the state file)")
    p.add_argument("--seq-file", default=DEFAULT_SEQ_FILE,
                   help="the console's sequence state file (default %(default)s); every number "
                        "used is saved there, never lowering it")
    p.add_argument("--expiry", type=int_in(0, SEQ_MAX), default=None, metavar="E",
                   help="Expiry field, seconds of Unix time (default 0: none)")
    p.add_argument("--expiry-in", type=seconds_in(SEQ_MAX), default=None, metavar="S",
                   help="Expiry field = now + S seconds, Unix time, as the console does")
    sub = p.add_subparsers(dest="cmd", required=True)
    dest_help = "Destination field: 0x01 onboard computer, 0x02 controller (any 0..255 is sent)"
    s = sub.add_parser("raw", help="one command frame, optionally damaged or mis-versioned")
    s.add_argument("--dest", type=int_in(0, U8_MAX), required=True, help=dest_help)
    s.add_argument("--id", type=int_in(0, U8_MAX), required=True, help="command identifier byte")
    s.add_argument("--fields", type=hex_bytes, default=b"", help="field bytes after the id, hex")
    s.add_argument("--flags", type=int_in(0, U8_MAX), default=0, help="Flags field (default 0)")
    s.add_argument("--version", type=int_in(0, U8_MAX), default=VERSION,
                   help="Version field (default 0x01, the valid one)")
    s.add_argument("--flip-byte", type=int_in(0, DATAGRAM_MAX), default=None, metavar="K",
                   help="XOR byte K of the finished frame (CRC included) with 0x01: damaged")
    s.set_defaults(run=cmd_raw)
    s = sub.add_parser("armed", help="ARM, wait --gap, then the command")
    s.add_argument("--dest", type=int_in(0, U8_MAX), required=True, help=dest_help)
    s.add_argument("--cmd", type=int_in(0, U8_MAX), required=True, help="command identifier byte")
    s.add_argument("--fields", type=hex_bytes, default=b"", help="field bytes after the id, hex")
    s.add_argument("--gap", type=seconds_in(GAP_MAX_S), default=0.0, metavar="S",
                   help="seconds between the ARM and the command (default 0)")
    s.add_argument("--no-arm", action="store_true", help="send only the command, no ARM")
    s.add_argument("--arm-names", type=int_in(0, SEQ_MAX), default=None, metavar="SEQ",
                   help="make the ARM name this sequence number instead of the command's")
    s.add_argument("--arm-id", type=int_in(0, U8_MAX), default=None, metavar="Y",
                   help="make the ARM name this command id instead of --cmd")
    s.set_defaults(run=cmd_armed)
    s = sub.add_parser("repeat", help="the same frame (same sequence number) sent N times")
    s.add_argument("--dest", type=int_in(0, U8_MAX), required=True, help=dest_help)
    s.add_argument("--id", type=int_in(0, U8_MAX), required=True, help="command identifier byte")
    s.add_argument("--fields", type=hex_bytes, default=b"", help="field bytes after the id, hex")
    s.add_argument("--gap", type=seconds_in(GAP_MAX_S), default=0.0, metavar="S",
                   help="seconds between the sends (default 0)")
    s.add_argument("--times", type=int_in(1, TIMES_MAX), default=TIMES_DEFAULT, metavar="N",
                   help="how many times to send it (default %(default)s)")
    s.set_defaults(run=cmd_repeat)
    s = sub.add_parser("cancel", help="UPLOAD CANCEL (0x04, image number) to the onboard computer")
    s.add_argument("--number", type=int_in(0, U16_MAX), required=True,
                   help="image number, 0..65535")
    s.set_defaults(run=cmd_cancel)
    return p


def main(argv=None, out=None):
    """Parse the command line, send the frames, listen, and return the exit status.

    Why: the exit status says whether the onboard computer answered at all, so a script can tell a
    silent drop (1) from an answer (0, then read the printed kinds and codes).

    Args:
        argv: argument list, or None for sys.argv.
        out: file the lines are printed to; sys.stdout if None.

    Returns:
        0 when an answer to a sequence number the tool sent arrived within the listen time, 1 when
        none did, 2 for a bad argument or an unwritable state file.
    """
    p = make_parser()
    args = p.parse_args(argv)
    if args.expiry is not None and args.expiry_in is not None:
        p.error("--expiry and --expiry-in cannot be used together")
    args.expiry = (args.expiry if args.expiry is not None
                   else int(time.time() + args.expiry_in) if args.expiry_in is not None else 0)
    seq_file = os.path.expanduser(args.seq_file)
    sess = Session(args.host, args.port, out)
    try:
        if args.seq is not None:
            first = args.seq
        else:
            first, note = first_seq(seq_file)
            if note:
                sess.say(f"seq  {note}")
        args.run(args, SeqSource(first, seq_file), sess)
        sess.wait(args.listen)
    except ToolError as e:
        print(f"error: {e}", file=sys.stderr)
        return EXIT_BAD_ARGS
    finally:
        sess.close()
    return EXIT_ANSWERED if sess.answered else EXIT_SILENT


if __name__ == "__main__":
    sys.exit(main())
