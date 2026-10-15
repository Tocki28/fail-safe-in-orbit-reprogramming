#!/usr/bin/env python3
"""Tests of four ground console checks and the `return` subcommand (drafted from the contract).

Checks under test: REQ-114 image check before upload, REQ-089 malformed commands refused before
they are sent, REQ-093 damaged frames marked, REQ-094 commands still waiting shown to the
operator. Subcommand under test: `return` (REQ-113). A fake onboard computer on localhost
(fake_onboard.py) stands in for the board; no network, no board.

These tests were written from the specification. The functions they test did not exist when
they were written, and the console at HEAD was read only for its harness (code rule E4).
Each test says in its docstring which defect makes it fail (code rule E1).

Interfaces means docs/interfaces/main.tex; "line" is a line of that file.
"""
import contextlib
import io
import json
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "src" / "ground"))
import console as c  # noqa: E402
from fake_onboard import FakeOnboard  # noqa: E402

# ---- Files ----------------------------------------------------------------------------
CONSOLE_PY = ROOT / "src" / "ground" / "console.py"
# The host build of the updater: `make -C src/onboard-computer/updater` (updater README, "Check-only
# mode"). It is the console's default check tool.
HOST_TOOL = ROOT / "src" / "onboard-computer" / "updater" / "build" / "host" / "fsir-updater"
SRC_IMAGES = ROOT / "src" / "controller" / "build" / "images"
TEST_IMAGES = ROOT / "test" / "controller" / "build" / "images"
GOOD_IMAGE = SRC_IMAGES / "firmware-B.img"          # the image `update` sends to slot B
GOOD_SMALL_IMAGE = TEST_IMAGES / "slotA-v1.img"     # 3032 bytes, passes the check: fewer chunks
FLIPPED_IMAGE = TEST_IMAGES / "slotA-v1-flipped.img"     # one bit flipped: body check value fails
WRONG_KEY_IMAGE = TEST_IMAGES / "firmware-B-wrong-key-a.img"    # signature A fails
IMAGE_KIND = "main"                 # contract item 1: the console runs the tool with --kind main

# ---- Timing of the tests (test choices, no source) ---------------------------------------
FAST_S = "0.3"                      # command timeout of the CLI runs; the real value is 3 s
PENDING_TIMEOUT_S = 0.3             # command timeout where a test looks at pending() mid-wait
GIVE_UP_TIMEOUT_S = 0.05            # command timeout where a test lets the console give up
SETTLE_S = 0.3                      # time for a datagram that was already sent to reach the fake
WAIT_S = 10.0                       # longest wait for a condition in a test
CLI_TIMEOUT_S = 60.0                # longest a console run may take in a test
CHECK_TIMEOUT_S = 30                # contract item 1: bounded wait for the check tool, seconds
HANG_S = 120                        # the hanging stub tool sleeps this long, more than the bound
HANG_CLI_TIMEOUT_S = 90.0           # console run that includes the 30 s wait of the check tool
FIRST_SEQ = 100                     # first sequence number in tests, any value of 1 or more
ARM_SEQ = 1234                      # sequence number named in a test ARM, any value
MAX_QUOTED_AGE_S = 5.0              # an age shown for a command in these tests is below this

# ---- Frame, Interfaces lines 184-229 -----------------------------------------------------
OTHER_VERSION = c.VERSION + 1       # a version number the receiver does not accept (line 219)
CRC_FLIP = 1                        # XOR on the check value to make it wrong (line 227)
DEST_ONBOARD, DEST_CONTROLLER, DEST_GROUND = 0x01, 0x02, 0x03    # line 221
BAD_DESTINATIONS = (0x00, 0x03, 0x04, 0xFF)      # not 0x01 or 0x02 (line 221; 0x03 is ground)

# ---- Commands, Interfaces command table, lines 345-363 -----------------------------------
ID_UPLOAD_BEGIN, ID_UPLOAD_CHUNK, ID_UPLOAD_END = 0x01, 0x02, 0x03
ID_UPLOAD_CANCEL, ID_DELIVER, ID_SET_LIMITS = 0x04, 0x05, 0x06
ID_RESTORE, ID_REPROGRAM, ID_SET_ONBOARD_TIME = 0x07, 0x08, 0x09
ID_SCHEDULE, ID_SEND_REPORTS, ID_REPLACE_KEY, ID_ARM = 0x0A, 0x0B, 0x0C, 0x0D
ID_SWITCH, ID_RETURN, ID_START_FAILED, ID_LEAVE_FALLBACK = 0x20, 0x21, 0x22, 0x23
ID_SET_CONTROLLER_TIME, ID_GET_STATUS, ID_CONFIRM = 0x24, 0x25, 0x26
ONBOARD_IDS = tuple(range(ID_UPLOAD_BEGIN, ID_ARM + 1))          # lines 345-357
CONTROLLER_IDS = tuple(range(ID_SWITCH, ID_CONFIRM + 1))         # lines 358-364
UNKNOWN_IDS = (0x00, 0x0E, 0x1F, 0x27, 0x3F, 0xFF)  # in neither range (lines 321-373)
# "Dangerous" column of the command table: the commands an ARM may name (lines 345-363).
DANGEROUS = {
    DEST_ONBOARD: {ID_SET_LIMITS, ID_RESTORE, ID_REPROGRAM, ID_SET_ONBOARD_TIME, ID_REPLACE_KEY},
    DEST_CONTROLLER: {ID_SWITCH, ID_RETURN, ID_START_FAILED, ID_SET_CONTROLLER_TIME, ID_CONFIRM},
}
BYTE_VALUES = 0x100                 # every value of the identifier byte

# Field sizes of the command table (lines 345-363). Values below are test inputs, no meaning.
IMAGE_NUMBER = 0x0102               # any image number except 0 and 0xFFFF
IMAGE_NUMBER_TEST_IMAGE = 0xFFFF    # the test image's number (lines 334-335): refused from ground
IMAGE_NUMBER_LOWEST, IMAGE_NUMBER_JUST_BELOW = 0x0000, 0xFFFE   # both accepted
IMAGE_LENGTH = 1000                 # any length of 1 or more
CHUNK_DATA = 16                     # bytes of data in a test chunk
CHUNK_DATA_MAX = 256                # UPLOAD CHUNK carries up to 256 data bytes (line 346)
CHUNK_HEAD = 7                      # identifier (1), image number (2), offset (4)
MIN_VERSION, CONTROLLER_IDENTITY = 1, 1
REGION_MAIN, KEY_SIGNING_A, KEY_BYTES, COPY_B = 0x02, 0x01, 32, 0x02
GROUND_TIME, RUN_TIME, FIRST_REPORT = 1000000, 2000000, 1
SCHEDULE_MIN = 25                   # identifier (1) + run time (4) + a complete frame (20 or more)
EMPTY_FRAME_BYTES = 20              # a frame with no payload: 16-byte header + 4-byte check value


def raw_frame(ftype=c.TYPE_REPORT, version=c.VERSION, sync=c.SYNC, length=None, payload=b"",
              crc_ok=True):
    """Build one datagram with exactly one thing wrong, if asked, and a matching check value.

    Each damage test needs a datagram that fails one frame check only (sync, version, length
    or check value), so a code that skips that one check is caught (REQ-093, Interfaces
    lines 184-229). The check value is computed after the other fields are set, so only a
    deliberate flip makes it wrong.

    Args:
        ftype: Type field.
        version: Version field.
        sync: the two Sync bytes.
        length: Length field; None for the real payload length.
        payload: payload bytes.
        crc_ok: False to flip the check value.

    Returns:
        The datagram as bytes.
    """
    declared = len(payload) if length is None else length
    head = c.HEADER.pack(sync, version, ftype, DEST_GROUND, 0, declared, 0, 0) + payload
    check = zlib.crc32(head) if crc_ok else zlib.crc32(head) ^ CRC_FLIP
    return head + struct.pack("<I", check)


def health_payload():
    """A HEALTH report payload as the onboard computer forwards it (Interfaces Sec. 5.4).

    Why: the damage tests need one report that is good, to show that a good report is not
    counted as damaged (REQ-093).

    Returns:
        The 24-byte payload: the two added fields, then the HEALTH body, OPERATE on slot A.
    """
    body = c.HEALTH_BODY.pack(1, c.REPORT_HEALTH, 1, 1, 1, 1, 0, 0, 0)
    return c.ADDED.pack(1, 1) + body


def event_payload():
    """An EVENT report payload (trial started) as the onboard computer forwards it.

    Returns:
        The payload: the two added fields, then controller clock, kind EVENT, event 0x0006.
    """
    return c.ADDED.pack(1, 1) + struct.pack("<IBH", 1, c.REPORT_EVENT, c.EVENT_TRIAL_STARTED)


def undecodable_payloads():
    """Report payloads that make a good frame but are no report (REQ-093, Interfaces Sec. 5.4).

    Returns:
        List of (label, payload): too short for any report, the HEALTH length with an unknown
        kind byte, and an EVENT cut short.
    """
    unknown_kind = bytearray(c.HEALTH_LEN)
    unknown_kind[4] = 0x09              # kind byte (offset 4) is neither HEALTH nor EVENT
    cut_event = bytes(4) + bytes([c.REPORT_EVENT]) + b"\x01"     # 6 bytes: code is cut short
    return [("5 bytes", bytes(5)), ("HEALTH length, unknown kind", bytes(unknown_kind)),
            ("EVENT cut short", cut_event)]


def damaged_datagrams():
    """Datagrams that each fail one frame check (REQ-093, Interfaces lines 184-229).

    Returns:
        List of (label, datagram): wrong sync, wrong version, length field above the datagram,
        length field above 1024, wrong check value, shorter than a header.
    """
    payload = b"abcde"                  # any five bytes
    return [
        ("wrong sync", raw_frame(sync=b"\x00\x00", payload=payload)),
        ("wrong version", raw_frame(version=OTHER_VERSION, payload=payload)),
        ("length above the datagram", raw_frame(length=len(payload) + 1, payload=payload)),
        ("length above 1024", raw_frame(length=c.PAYLOAD_MAX + 1, payload=payload)),
        ("wrong check value", raw_frame(payload=payload, crc_ok=False)),
        ("shorter than a header", c.SYNC + bytes([c.VERSION])),
    ]


def arm_payload(named):
    """An ARM payload naming a command (Interfaces line 357).

    Args:
        named: identifier of the command the ARM names.

    Returns:
        identifier 0x0D, the named identifier, and ARM_SEQ as 4 bytes, little-endian.
    """
    return struct.pack("<BBI", ID_ARM, named, ARM_SEQ)


def good_payload(cid, number=IMAGE_NUMBER, length=IMAGE_LENGTH):
    """A well-formed payload for a command other than ARM (Interfaces lines 345-364).

    Args:
        cid: command identifier, 0x01 to 0x0C or 0x20 to 0x26.
        number: image number for the upload commands, 0 to 65535.
        length: image length for UPLOAD BEGIN, bytes.

    Returns:
        The payload with exactly the fields of the command table.
    """
    complete_frame = c.build(c.TYPE_COMMAND, DEST_CONTROLLER, 1, bytes([ID_GET_STATUS]))
    table = {
        ID_UPLOAD_BEGIN: struct.pack("<BHI", cid, number, length),
        ID_UPLOAD_CHUNK: struct.pack("<BHI", cid, number, 0) + bytes(CHUNK_DATA),
        ID_UPLOAD_END: struct.pack("<BH", cid, number),
        ID_UPLOAD_CANCEL: struct.pack("<BH", cid, number),
        ID_DELIVER: struct.pack("<BH", cid, number),
        ID_SET_LIMITS: struct.pack("<BIH", cid, MIN_VERSION, CONTROLLER_IDENTITY),
        ID_RESTORE: bytes([cid]),
        ID_REPROGRAM: struct.pack("<BHB", cid, number, REGION_MAIN),
        ID_SET_ONBOARD_TIME: struct.pack("<BI", cid, GROUND_TIME),
        ID_SCHEDULE: struct.pack("<BI", cid, RUN_TIME) + complete_frame,
        ID_SEND_REPORTS: struct.pack("<BI", cid, FIRST_REPORT),
        ID_REPLACE_KEY: bytes([cid, KEY_SIGNING_A]) + bytes(KEY_BYTES),
        ID_START_FAILED: bytes([cid, COPY_B]),
        ID_SET_CONTROLLER_TIME: struct.pack("<BI", cid, GROUND_TIME),
    }
    return table.get(cid, bytes([cid]))


def good_commands():
    """Every well-formed command the console can send, with its destination.

    Why: REQ-089 says the console checks every command before it is sent, so every command it
    legitimately sends must pass (Interfaces lines 321-373).

    Returns:
        List of (label, destination, payload): each of 0x01 to 0x0C to the onboard computer,
        each of 0x20 to 0x26 to the controller, and an ARM naming each dangerous command of
        either destination, sent to that destination.
    """
    out = []
    for cid in ONBOARD_IDS:
        if cid != ID_ARM:
            out.append((f"id 0x{cid:02x} to onboard", DEST_ONBOARD, good_payload(cid)))
    for cid in CONTROLLER_IDS:
        out.append((f"id 0x{cid:02x} to controller", DEST_CONTROLLER, good_payload(cid)))
    for dest, named_ids in DANGEROUS.items():
        for named in sorted(named_ids):
            out.append((f"ARM naming 0x{named:02x} to dest 0x{dest:02x}", dest,
                        arm_payload(named)))
    return out


def captured():
    """Context manager that collects what is printed to stdout and stderr.

    Why: the REQ-093 and REQ-094 lines are printed by the console; the tests read them back.

    Returns:
        A context manager that gives the io.StringIO holding the printed text.
    """
    @contextlib.contextmanager
    def manager():
        """Redirect stdout and stderr into one buffer for the length of the with block.

        Yields:
            The io.StringIO that receives the text.
        """
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            yield buf
    return manager()


def commands_received(fake):
    """The command frames the fake received, parsed, in order.

    Args:
        fake: the FakeOnboard.

    Returns:
        List of frame dicts from console.parse(), commands only.
    """
    frames = [c.parse(d) for d in list(fake.datagrams)]
    return [f for f in frames if f and f["type"] == c.TYPE_COMMAND]


class FakeCase(unittest.TestCase):
    """Base of the tests that talk to a fake onboard computer."""

    def setUp(self):
        """Make a private temporary folder; the fake is started by start_fake().

        Args:
            self: the test case.

        Returns:
            None.
        """
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.fake = None

    def start_fake(self, **options):
        """Start a fake onboard computer that is closed when the test ends.

        Args:
            self: the test case.
            **options: FakeOnboard options (damaged, ignore, no_done, accept, refuse).

        Returns:
            The FakeOnboard, also kept as self.fake.
        """
        self.fake = FakeOnboard(**options)
        self.addCleanup(self.fake.close)
        return self.fake

    def make_console(self, timeout=PENDING_TIMEOUT_S, quiet=True, **options):
        """A Console that talks to the fake and is closed when the test ends.

        Args:
            self: the test case.
            timeout: command timeout, seconds.
            quiet: True to print nothing from say().
            **options: further Console options, such as first_seq or state_file.

        Returns:
            The Console.
        """
        first_seq = options.pop("first_seq", FIRST_SEQ)
        con = c.Console("127.0.0.1", self.fake.port, timeout=timeout, quiet=quiet,
                        first_seq=first_seq, **options)
        self.addCleanup(con.sock.close)
        return con

    def settle(self):
        """Wait until a datagram that was already sent has reached the fake.

        Args:
            self: the test case.

        Returns:
            None.
        """
        time.sleep(SETTLE_S)

    def assert_nothing_sent(self):
        """Fail if the fake received any datagram at all.

        Args:
            self: the test case.

        Returns:
            None. Fails the test when a datagram arrived.
        """
        self.settle()
        self.assertEqual(self.fake.datagrams, [], "a datagram reached the onboard computer")

    def wait_for(self, condition, seconds=WAIT_S):
        """Poll until a condition holds or the time is up.

        Args:
            self: the test case.
            condition: function returning True when done.
            seconds: longest wait, seconds.

        Returns:
            The condition's last value.
        """
        end = time.monotonic() + seconds
        while time.monotonic() < end and not condition():
            time.sleep(0.02)
        return condition()

    def run_cli(self, *args, tool=HOST_TOOL, cwd=HERE, timeout=CLI_TIMEOUT_S):
        """Run console.py as a program against the fake.

        Args:
            self: the test case.
            *args: the subcommand and its arguments.
            tool: path given as --check-tool; None to leave the option out.
            cwd: working folder of the run (the Makefile runs from test/ground).
            timeout: longest the run may take, seconds.

        Returns:
            subprocess.CompletedProcess with text stdout and stderr.
        """
        cmd = [sys.executable, str(CONSOLE_PY), "--host", "127.0.0.1", "--port",
               str(self.fake.port), "--timeout", FAST_S, "--seq-file", str(self.tmp / "seq")]
        if tool is not None:
            cmd += ["--check-tool", str(tool)]
        cmd += [str(a) for a in args]
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=cwd)

    def write_stub_tool(self, exit_code, line, log=None, hang=False):
        """Write a stand-in for the check tool: a small program with a chosen result.

        Why: the contract fixes how the console reacts to the tool's exit code and output, so
        the tests need a tool whose answer they choose (REQ-114).

        Args:
            self: the test case.
            exit_code: exit status of the stub.
            line: the one line the stub prints to stdout.
            log: file where the stub appends its arguments as one JSON list per call, or None.
            hang: True for a stub that sleeps HANG_S seconds and prints nothing.

        Returns:
            Path of the stub.
        """
        path = self.tmp / "stub-check-tool"
        body = [f"#!{sys.executable}", "import json, sys, time"]
        if log:
            body.append(f"open({str(log)!r}, 'a').write(json.dumps(sys.argv[1:]) + '\\n')")
        if hang:
            body.append(f"time.sleep({HANG_S})")
        body.append(f"print({line!r})")
        body.append(f"sys.exit({exit_code})")
        path.write_text("\n".join(body) + "\n")
        path.chmod(0o755)
        return path

    def image_folder(self, image):
        """A folder for `update` holding the given image as firmware-A.img and firmware-B.img.

        Args:
            self: the test case.
            image: Path of the image to copy under both names.

        Returns:
            Path of the folder.
        """
        folder = self.tmp / "images"
        folder.mkdir(exist_ok=True)
        for name in ("firmware-A.img", "firmware-B.img"):
            (folder / name).write_bytes(image.read_bytes())
        return folder


class CheckCommandTests(unittest.TestCase):
    """REQ-089: console.check_command(dest, payload) against the command table."""

    def assert_rejected(self, dest, payload, label):
        """Fail unless check_command refuses the command with a reason text.

        Args:
            self: the test case.
            dest: destination byte.
            payload: payload bytes.
            label: text naming the case, for the failure message.

        Returns:
            None.
        """
        reason = c.check_command(dest, payload)
        self.assertIsInstance(reason, str, f"{label}: accepted")
        self.assertTrue(reason.strip(), f"{label}: reason is empty")

    def test_accepts_every_well_formed_command(self):
        """Check that every command the console sends passes check_command.

        Why: REQ-089; Interfaces lines 345-364 give each command's fields.
        Fails if: a rule is too strict, for example ARM refused to the onboard computer, a
        dangerous command not allowed in ARM, or a field length counted wrong.

        Args:
            self: the test case.

        Returns:
            None.
        """
        for label, dest, payload in good_commands():
            with self.subTest(label):
                self.assertIsNone(c.check_command(dest, payload))

    def test_accepts_the_edges_of_every_range(self):
        """Check the allowed edges: chunk of 1 and 256 data bytes, schedule of 25 bytes and
        more, image numbers 0 and 0xFFFE, upload length 1.

        Why: REQ-089; Interfaces lines 346 (chunk up to 256), 354 (a complete frame), 345.
        Fails if: a bound is off by one, or an image number of 0 or just under 0xFFFF is
        refused.

        Args:
            self: the test case.

        Returns:
            None.
        """
        chunk_head = struct.pack("<BHI", ID_UPLOAD_CHUNK, IMAGE_NUMBER, 0)
        empty = c.build(c.TYPE_COMMAND, DEST_CONTROLLER, 1, b"")
        self.assertEqual(len(empty), EMPTY_FRAME_BYTES)
        big = c.build(c.TYPE_COMMAND, DEST_CONTROLLER, 1, bytes(CHUNK_DATA_MAX))
        schedule_head = struct.pack("<BI", ID_SCHEDULE, RUN_TIME)
        cases = [
            ("chunk, 1 data byte", chunk_head + b"\x00"),
            ("chunk, 256 data bytes", chunk_head + bytes(CHUNK_DATA_MAX)),
            ("schedule, 25 bytes", schedule_head + empty),
            ("schedule, longer", schedule_head + big),
            ("upload begin, length 1", good_payload(ID_UPLOAD_BEGIN, length=1)),
        ]
        for number in (IMAGE_NUMBER_LOWEST, IMAGE_NUMBER_JUST_BELOW):
            for cid in range(ID_UPLOAD_BEGIN, ID_DELIVER + 1):
                cases.append((f"id 0x{cid:02x}, image number 0x{number:04x}",
                              good_payload(cid, number=number)))
        self.assertEqual(len(cases[2][1]), SCHEDULE_MIN)
        self.assertEqual(len(cases[1][1]), CHUNK_HEAD + CHUNK_DATA_MAX)
        for label, payload in cases:
            with self.subTest(label):
                self.assertIsNone(c.check_command(DEST_ONBOARD, payload))

    def test_rejects_a_destination_that_is_not_onboard_or_controller(self):
        """Check that destinations other than 0x01 and 0x02 are refused for GET STATUS.

        Why: REQ-089; Interfaces line 221 lists 0x03 as ground, which only receives.
        Fails if: the destination is not checked, or ground (0x03) is accepted.

        Args:
            self: the test case.

        Returns:
            None.
        """
        for dest in BAD_DESTINATIONS:
            with self.subTest(dest=dest):
                self.assert_rejected(dest, bytes([ID_GET_STATUS]), f"dest 0x{dest:02x}")

    def test_rejects_onboard_commands_sent_to_the_controller(self):
        """Check that 0x01 to 0x0C, well formed, are refused with destination 0x02.

        Why: REQ-089; Interfaces lines 321-326 (0x01 to 0x1F are for the onboard computer; the
        onboard computer alone passes the upload steps on) and 345-357.
        Fails if: the identifier range is not tied to the destination.

        Args:
            self: the test case.

        Returns:
            None.
        """
        for cid in ONBOARD_IDS:
            if cid != ID_ARM:
                with self.subTest(cid=cid):
                    self.assert_rejected(DEST_CONTROLLER, good_payload(cid), f"0x{cid:02x}")

    def test_rejects_controller_commands_sent_to_the_onboard_computer(self):
        """Check that 0x20 to 0x26, well formed, are refused with destination 0x01.

        Why: REQ-089; Interfaces lines 321-323 (0x20 to 0x3F are commands to the controller).
        Fails if: the identifier range is not tied to the destination.

        Args:
            self: the test case.

        Returns:
            None.
        """
        for cid in CONTROLLER_IDS:
            with self.subTest(cid=cid):
                self.assert_rejected(DEST_ONBOARD, good_payload(cid), f"0x{cid:02x}")

    def test_rejects_identifiers_outside_the_table(self):
        """Check that identifiers in neither range are refused for both destinations.

        Why: REQ-089; Interfaces lines 345-364 list the only identifiers the console may send.
        Fails if: an unknown identifier is accepted, or looking it up raises an exception.

        Args:
            self: the test case.

        Returns:
            None.
        """
        padding = bytes(CHUNK_DATA)
        for dest in (DEST_ONBOARD, DEST_CONTROLLER):
            for cid in UNKNOWN_IDS:
                for payload in (bytes([cid]), bytes([cid]) + padding):
                    with self.subTest(dest=dest, cid=cid, size=len(payload)):
                        self.assert_rejected(dest, payload, f"0x{cid:02x}")

    def test_rejects_an_empty_payload_without_raising(self):
        """Check that an empty payload gives a reason, not an exception.

        Why: REQ-089; a payload has at least its identifier byte (Interfaces line 321).
        Fails if: check_command reads payload[0] of an empty payload and raises IndexError.

        Args:
            self: the test case.

        Returns:
            None.
        """
        for dest in (DEST_ONBOARD, DEST_CONTROLLER):
            with self.subTest(dest=dest):
                self.assert_rejected(dest, b"", "empty payload")

    def test_rejects_every_payload_one_byte_too_long_or_too_short(self):
        """Check the exact payload lengths: each good payload with a byte added or removed.

        Why: REQ-089; the Fields column of Interfaces lines 345-364 fixes every length. For
        UPLOAD CHUNK the allowed range is 8 to 263 and for SCHEDULE 25 or more, so those two
        are tested at the edge on the side that is refused.
        Fails if: any length is not checked, or a bound is off by one.

        Args:
            self: the test case.

        Returns:
            None.
        """
        for label, dest, payload in good_commands():
            cid = payload[0]
            if cid == ID_UPLOAD_CHUNK:
                variants = [("no data", payload[:CHUNK_HEAD]),
                            ("257 data bytes", payload[:CHUNK_HEAD] + bytes(CHUNK_DATA_MAX + 1))]
            elif cid == ID_SCHEDULE:
                variants = [("24 bytes", payload[:SCHEDULE_MIN - 1])]
            else:
                variants = [("one byte short", payload[:-1]), ("one byte long", payload + b"\0")]
            for what, bad in variants:
                with self.subTest(f"{label}, {what}"):
                    self.assert_rejected(dest, bad, f"{label}, {what}")

    def test_arm_may_name_only_a_dangerous_command_of_its_destination(self):
        """Check ARM against all 256 identifiers it could name, for each destination.

        Why: REQ-089, REQ-029, REQ-112; the Dangerous column of Interfaces lines 345-363 says
        which commands need an ARM. An ARM that names another command, or one of the other
        destination, can never be matched and would only be refused by the target.
        Fails if: ARM may name any command, names are not tied to the destination (0x06 to the
        controller, 0x20 to the onboard computer), or a dangerous command is missing.

        Args:
            self: the test case.

        Returns:
            None.
        """
        for dest, dangerous in DANGEROUS.items():
            for named in range(BYTE_VALUES):
                with self.subTest(dest=dest, named=named):
                    reason = c.check_command(dest, arm_payload(named))
                    if named in dangerous:
                        self.assertIsNone(reason)
                    else:
                        self.assert_rejected(dest, arm_payload(named), f"naming 0x{named:02x}")

    def test_rejects_image_number_ffff_in_the_upload_commands(self):
        """Check that image number 0xFFFF is refused in 0x01 to 0x05 sent to the onboard computer.

        Why: REQ-089; Interfaces lines 334-335: 0xFFFF is the test image, which only the
        onboard computer's own trial tests send.
        Fails if: the number is not checked, or only some of the five commands are checked.

        Args:
            self: the test case.

        Returns:
            None.
        """
        for cid in range(ID_UPLOAD_BEGIN, ID_DELIVER + 1):
            with self.subTest(cid=cid):
                payload = good_payload(cid, number=IMAGE_NUMBER_TEST_IMAGE)
                self.assert_rejected(DEST_ONBOARD, payload, f"0x{cid:02x} with 0xFFFF")

    def test_rejects_upload_begin_with_length_zero(self):
        """Check that UPLOAD BEGIN with an image length of 0 is refused.

        Why: REQ-089; an image of no bytes cannot be an image (Interfaces line 345).
        Fails if: the length field is not checked.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.assert_rejected(DEST_ONBOARD, good_payload(ID_UPLOAD_BEGIN, length=0), "length 0")


class ConsoleCommandTests(FakeCase):
    """REQ-089: Console.command() refuses a malformed command before it takes a number."""

    def test_malformed_command_is_not_sent_and_takes_no_sequence_number(self):
        """Check that a malformed command raises Refused "command not sent: <reason>" and that
        nothing is sent, no number is taken, no state file is written; the next good command
        then carries the first number.

        Why: REQ-089; sequence numbers are never reused (Interfaces line 224), so a refused
        command must not spend one.
        Fails if: the check runs after next_seq() or after the send, the message differs from
        check_command's reason, or the exception is another type.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.start_fake()
        state = self.tmp / "seq-state"
        con = self.make_console(first_seq=FIRST_SEQ, state_file=str(state))
        bad = [(DEST_ONBOARD, good_payload(ID_UPLOAD_BEGIN, length=0)),
               (DEST_GROUND, bytes([ID_GET_STATUS])), (DEST_CONTROLLER, b"")]
        for dest, payload in bad:
            with self.subTest(dest=dest, size=len(payload)):
                reason = c.check_command(dest, payload)
                self.assertIsInstance(reason, str)
                with self.assertRaises(c.Refused) as ctx:
                    con.command(dest, payload, what="TEST")
                self.assertEqual(str(ctx.exception), f"command not sent: {reason}")
        self.assertEqual(con.seq, FIRST_SEQ - 1)
        self.assertFalse(state.exists())
        self.assert_nothing_sent()
        kind = con.command(DEST_CONTROLLER, bytes([ID_GET_STATUS]), what="GET STATUS")
        self.assertEqual(kind, c.DONE)
        self.assertEqual(commands_received(self.fake)[0]["seq"], FIRST_SEQ)

    def test_well_formed_command_is_sent_and_answered(self):
        """Check that a well-formed command still goes out and ends DONE.

        Why: REQ-089 must not stop the commands the console exists to send (Interfaces
        line 359 ff.).
        Fails if: the check refuses a good command, or returns before sending.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.start_fake()
        con = self.make_console()
        self.assertEqual(con.command(DEST_CONTROLLER, bytes([ID_GET_STATUS])), c.DONE)
        self.assertEqual(len(commands_received(self.fake)), 1)


class NumberTests(FakeCase):
    """REQ-089: a typed value that cannot be packed ends as a refusal, not a traceback."""

    def test_unpackable_numbers_exit_1_without_a_traceback(self):
        """Check `upload --number 70000`, `--number -1`, `deliver --number 70000` and
        `update --number 70000`: exit status 1, a "FAILED: command not sent" line, no traceback,
        and no upload command sent.

        Why: REQ-089; the image number is 2 bytes (Interfaces lines 345-349), so 70000 and -1
        do not fit. One defect found means the whole class is swept (rule B10): every
        subcommand that packs the number is run.
        Fails if: struct.error escapes (traceback, status 1 from Python but no FAILED line), or
        the check happens after a send, or a subcommand is left unchecked.

        Args:
            self: the test case.

        Returns:
            None.
        """
        folder = self.image_folder(GOOD_SMALL_IMAGE)
        cases = [("upload", ["upload", GOOD_SMALL_IMAGE, "--number", "70000"], 0),
                 ("upload, negative", ["upload", GOOD_SMALL_IMAGE, "--number=-1"], 0),
                 ("deliver", ["deliver", "--number", "70000"], 0),
                 ("update", ["update", "--image-dir", folder, "--yes", "--number", "70000"], 1)]
        for label, args, allowed in cases:
            with self.subTest(label):
                fake = self.start_fake()
                done = self.run_cli(*args)
                text = done.stdout + done.stderr
                self.assertNotIn("Traceback", text)
                self.assertEqual(done.returncode, 1, text)
                self.assertIn("FAILED: command not sent:", done.stdout)
                self.settle()
                ids = [f["payload"][0] for f in commands_received(fake)]
                self.assertLessEqual(len(ids), allowed)    # update reads one HEALTH report first
                self.assertTrue(all(i == ID_GET_STATUS for i in ids), ids)


class ImageCheckTests(FakeCase):
    """REQ-114: the console runs the check tool on an image before the first UPLOAD BEGIN."""

    def setUp(self):
        """Start the fake and require the host updater and the images to exist.

        Args:
            self: the test case.

        Returns:
            None. Fails the test when a file is missing: run `make -C
            src/onboard-computer/updater` and `make -C src/controller` first.
        """
        super().setUp()
        for needed in (HOST_TOOL, GOOD_IMAGE, GOOD_SMALL_IMAGE, FLIPPED_IMAGE, WRONG_KEY_IMAGE):
            self.assertTrue(needed.exists(), f"missing {needed}: build it first")
        self.start_fake()

    def tool_line(self, image):
        """Run the real tool on an image, as the console should, and read its answer.

        Args:
            self: the test case.
            image: Path of the image.

        Returns:
            (exit status, the one output line).
        """
        done = subprocess.run([str(HOST_TOOL), "--check-image", str(image), "--kind", IMAGE_KIND],
                              capture_output=True, text=True, timeout=CLI_TIMEOUT_S)
        return done.returncode, done.stdout.strip()

    def assert_refused_before_upload(self, done, line):
        """Fail unless a run ended as refused before upload with the reason, and sent nothing.

        Args:
            self: the test case.
            done: the CompletedProcess of the console run.
            line: the reason the console must show.

        Returns:
            None.
        """
        self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
        self.assertIn(f"FAILED: image refused before upload: {line}", done.stdout)
        self.assertNotIn("Traceback", done.stderr)
        self.assert_nothing_sent()

    def test_good_image_passes_the_check_and_uploads(self):
        """Check that the firmware-B image passes the real tool and arrives complete.

        Why: REQ-114; the check must not stop a good signed image (updater README, check-only
        mode: exit 0 passed).
        Fails if: exit 0 is treated as a refusal, or the upload does not follow the check.

        Args:
            self: the test case.

        Returns:
            None.
        """
        status, _ = self.tool_line(GOOD_IMAGE)
        self.assertEqual(status, 0)
        done = self.run_cli("upload", GOOD_IMAGE, "--number", "1")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertTrue(self.fake.ended)
        self.assertEqual(bytes(self.fake.upload), GOOD_IMAGE.read_bytes())

    def test_flipped_image_is_refused_and_nothing_is_sent(self):
        """Check that an image with a flipped bit is refused with the tool's own line.

        Why: REQ-114; Capability-4: a damaged image must not reach the onboard computer.
        Fails if: the check is missing or runs after UPLOAD BEGIN (the fake would receive a
        datagram), the exit status is not 1, or the reason is not the tool's output line.

        Args:
            self: the test case.

        Returns:
            None.
        """
        status, line = self.tool_line(FLIPPED_IMAGE)
        self.assertEqual(status, 1)
        self.assert_refused_before_upload(self.run_cli("upload", FLIPPED_IMAGE, "--number", "1"),
                                          line)

    def test_wrong_key_image_is_refused_and_nothing_is_sent(self):
        """Check that an image signed with the wrong key A is refused with the tool's line.

        Why: REQ-114; an image not from the operator must not be sent (Interfaces refusal 0x09).
        Fails if: only the damage check is done and not the signatures, or the reason is
        replaced by a text of the console's own.

        Args:
            self: the test case.

        Returns:
            None.
        """
        status, line = self.tool_line(WRONG_KEY_IMAGE)
        self.assertEqual(status, 1)
        done = self.run_cli("upload", WRONG_KEY_IMAGE, "--number", "1")
        self.assert_refused_before_upload(done, line)

    def test_resumed_upload_is_checked_too(self):
        """Check `upload --offset N`: a flipped image is refused with nothing sent, a good one
        is checked and resumed.

        Why: REQ-114 names `upload --offset N` as a path that must check, though it skips
        UPLOAD BEGIN.
        Fails if: the check sits only where BEGIN is sent, so a resumed upload goes unchecked.

        Args:
            self: the test case.

        Returns:
            None.
        """
        _, line = self.tool_line(FLIPPED_IMAGE)
        done = self.run_cli("upload", FLIPPED_IMAGE, "--number", "1", "--offset",
                            str(CHUNK_DATA_MAX))
        self.assert_refused_before_upload(done, line)
        data = GOOD_SMALL_IMAGE.read_bytes()
        self.fake.upload_len = len(data)
        self.fake.upload = bytearray(data[:CHUNK_DATA_MAX])
        done = self.run_cli("upload", GOOD_SMALL_IMAGE, "--number", "1", "--offset",
                            str(CHUNK_DATA_MAX))
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertEqual(bytes(self.fake.upload), data)

    def test_update_checks_the_image_before_any_upload(self):
        """Check `update` with a flipped image: refused, exit status 1, and the only command
        the fake sees is GET STATUS.

        Why: REQ-114 names `update` as a path that must check.
        Fails if: `update` skips the check, or checks after UPLOAD BEGIN.

        Args:
            self: the test case.

        Returns:
            None.
        """
        folder = self.image_folder(FLIPPED_IMAGE)
        # The tool's line names the file it checked. `update` checks the free slot's image
        # from --image-dir (firmware-B.img, the copy), so the reference run uses that copy.
        _, line = self.tool_line(folder / "firmware-B.img")
        done = self.run_cli("update", "--image-dir", folder, "--yes")
        self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
        self.assertIn(f"image refused before upload: {line}", done.stdout)
        self.settle()
        ids = [f["payload"][0] for f in commands_received(self.fake)]
        self.assertTrue(ids and all(i == ID_GET_STATUS for i in ids), ids)

    def test_missing_tool_is_refused_and_nothing_is_sent(self):
        """Check `--check-tool` with a path that does not exist.

        Why: REQ-114; an image that cannot be checked is not sent.
        Fails if: the missing tool is treated as a pass, or raises FileNotFoundError.

        Args:
            self: the test case.

        Returns:
            None.
        """
        missing = self.tmp / "no-such-tool"
        done = self.run_cli("upload", GOOD_SMALL_IMAGE, "--number", "1", tool=missing)
        self.assert_refused_before_upload(done, f"check tool not found: {missing}")

    def test_default_check_tool_is_the_host_build_whatever_the_working_folder(self):
        """Check that without --check-tool, run from test/ground, the host build is used.

        Why: REQ-114; the other test files run the console without the option from test/ground,
        so the default must not depend on the working folder.
        Fails if: the default path is relative to the working folder (not found), or there is
        no default (the check is skipped and the flipped image is sent).

        Args:
            self: the test case.

        Returns:
            None.
        """
        _, line = self.tool_line(FLIPPED_IMAGE)
        done = self.run_cli("upload", FLIPPED_IMAGE, "--number", "1", tool=None, cwd=HERE)
        self.assert_refused_before_upload(done, line)

    def test_every_non_zero_exit_status_of_the_tool_refuses(self):
        """Check exit statuses 1, 2 and 3 of a stub tool: each refuses, with the stub's line.

        Why: REQ-114; "otherwise nothing is sent": 2 is usage, 3 is keys unusable (updater
        README, check-only mode). A tool that cannot judge must not let an image through.
        Fails if: only status 1 is treated as a refusal.

        Args:
            self: the test case.

        Returns:
            None.
        """
        for status in (1, 2, 3):
            with self.subTest(status=status):
                fake = self.start_fake()
                line = f"stub says no {status}"
                stub = self.write_stub_tool(status, line)
                done = self.run_cli("upload", GOOD_SMALL_IMAGE, "--number", "1", tool=stub)
                self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
                self.assertIn(f"image refused before upload: {line}", done.stdout)
                self.settle()
                self.assertEqual(fake.datagrams, [])

    def test_tool_gets_the_documented_arguments_once_per_run(self):
        """Check the stub's arguments for `upload` and for `update`: exactly
        `--check-image FILE --kind main`, one call per run, then the run goes on.

        Why: REQ-114; the tool's command line is fixed by the updater README (check-only mode).
        Fails if: the arguments differ, the tool is called once per chunk, or exit 0 does not
        continue.

        Args:
            self: the test case.

        Returns:
            None.
        """
        log = self.tmp / "calls.log"
        stub = self.write_stub_tool(0, "stub says yes", log=log)
        done = self.run_cli("upload", GOOD_SMALL_IMAGE, "--number", "1", tool=stub)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][0], "--check-image")
        self.assertEqual(Path(calls[0][1]).resolve(), GOOD_SMALL_IMAGE.resolve())
        self.assertEqual(calls[0][2:], ["--kind", IMAGE_KIND])
        log.unlink()
        folder = self.image_folder(GOOD_SMALL_IMAGE)
        done = self.run_cli("update", "--image-dir", folder, "--yes", tool=stub)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        self.assertEqual(len(calls), 1)
        self.assertEqual(Path(calls[0][1]).resolve(), (folder / "firmware-B.img").resolve())

    def test_a_tool_that_hangs_is_given_up_after_30_seconds(self):
        """Check a stub that never answers: the run ends after about 30 s as refused, with
        "check tool timed out after 30 s", and nothing is sent. This test takes about 30 s.

        Why: REQ-114; a bounded wait (rule B2) so a stuck tool cannot hang the console.
        Fails if: the wait is unbounded (the run exceeds HANG_CLI_TIMEOUT_S), shorter than 30 s
        (elapsed time too small), or the text differs.

        Args:
            self: the test case.

        Returns:
            None.
        """
        stub = self.write_stub_tool(0, "never printed", hang=True)
        t0 = time.monotonic()
        done = self.run_cli("upload", GOOD_SMALL_IMAGE, "--number", "1", tool=stub,
                            timeout=HANG_CLI_TIMEOUT_S)
        elapsed = time.monotonic() - t0
        self.assert_refused_before_upload(
            done, f"check tool timed out after {CHECK_TIMEOUT_S} s")
        self.assertGreaterEqual(elapsed, CHECK_TIMEOUT_S - 1)
        self.assertLess(elapsed, CHECK_TIMEOUT_S * 2)


class DamagedFrameTests(FakeCase):
    """REQ-093: a datagram that is no good frame, or no report, is counted and shown."""

    def console_and_sender(self):
        """A Console that prints, and a socket that sends datagrams to it.

        Args:
            self: the test case.

        Returns:
            (Console, send function taking the datagram bytes).
        """
        con = c.Console("127.0.0.1", 9, timeout=PENDING_TIMEOUT_S, first_seq=FIRST_SEQ)
        self.addCleanup(con.sock.close)
        sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.addCleanup(sender.close)
        target = ("127.0.0.1", con.sock.getsockname()[1])
        return con, lambda data: sender.sendto(data, target)

    def pump_one(self, con, send, data):
        """Send one datagram to the console, pump it, and return what was printed.

        Args:
            self: the test case.
            con: the Console.
            send: function that sends the datagram to it.
            data: the datagram bytes.

        Returns:
            (printed text, the value pump() returned).
        """
        send(data)
        with captured() as buf:
            result = con.pump(WAIT_S)
        return buf.getvalue(), result

    def test_damaged_count_starts_at_zero(self):
        """Check that a new Console has damaged == 0.

        Why: REQ-093; the counter is part of the contract.
        Fails if: the attribute is missing or starts above 0.

        Args:
            self: the test case.

        Returns:
            None.
        """
        con, _ = self.console_and_sender()
        self.assertEqual(con.damaged, 0)

    def test_each_frame_check_failure_is_counted_and_shown(self):
        """Check six damaged datagrams, each failing one check: counted once, one line with
        the datagram's size and a reason, and the report list stays empty.

        Why: REQ-093; Interfaces lines 188-190 and 218-227 (sync, version, length, check value).
        Fails if: a check is skipped, a size is wrong in the line, the line is missing or
        printed twice, or the damaged datagram is stored as a report.

        Args:
            self: the test case.

        Returns:
            None.
        """
        con, send = self.console_and_sender()
        for number, (label, data) in enumerate(damaged_datagrams(), start=1):
            with self.subTest(label):
                text, result = self.pump_one(con, send, data)
                self.assertIsNone(result)
                self.assertEqual(con.damaged, number)
                lines = re.findall(rf"^damaged frame dropped \({len(data)} bytes\): \S.*$", text,
                                   re.MULTILINE)
                self.assertEqual(len(lines), 1, text)
        self.assertEqual(con.reports, [])

    def test_report_that_does_not_decode_is_counted_and_shown(self):
        """Check good frames of type report whose payload is no report.

        Why: REQ-093; Interfaces Sec. 5.4 fixes the report layouts. A good check value does not
        make a report good.
        Fails if: only the frame checks are counted, so an undecodable report is dropped
        silently as before.

        Args:
            self: the test case.

        Returns:
            None.
        """
        con, send = self.console_and_sender()
        for number, (label, payload) in enumerate(undecodable_payloads(), start=1):
            with self.subTest(label):
                data = raw_frame(payload=payload)
                self.assertIsNotNone(c.parse(data), "test datagram must be a good frame")
                text, _ = self.pump_one(con, send, data)
                self.assertEqual(con.damaged, number)
                self.assertRegex(text, rf"damaged frame dropped \({len(data)} bytes\): \S")
                self.assertEqual(con.reports, [])

    def test_good_frames_are_not_counted(self):
        """Check a good HEALTH report, a good EVENT report and a good answer.

        Why: REQ-093; a mark on every datagram would hide the real damage.
        Fails if: the counter or the line fires for good frames (for example for every
        datagram, or for answers, which pump() returns instead of storing).

        Args:
            self: the test case.

        Returns:
            None.
        """
        con, send = self.console_and_sender()
        answer = c.build(c.TYPE_ANSWER, DEST_GROUND, FIRST_SEQ, c.ANSWER.pack(FIRST_SEQ, c.DONE, 0))
        for label, data in (("HEALTH", raw_frame(payload=health_payload())),
                            ("EVENT", raw_frame(payload=event_payload())), ("answer", answer)):
            with self.subTest(label):
                text, _ = self.pump_one(con, send, data)
                self.assertEqual(con.damaged, 0)
                self.assertNotIn("damaged", text)
        self.assertEqual(len(con.reports), 2)

    def test_a_command_still_finishes_when_frames_around_its_answer_are_damaged(self):
        """Check that two damaged datagrams, sent between RECEIVED and DONE, are counted and
        the command still ends DONE.

        Why: REQ-093; marking damage must not change how commands run.
        Fails if: a damaged datagram raises, ends the wait, or is mistaken for an answer.

        Args:
            self: the test case.

        Returns:
            None.
        """
        data = [d for _, d in damaged_datagrams()[:2]]
        self.start_fake(damaged=data)
        con = self.make_console()
        self.assertEqual(con.command(DEST_CONTROLLER, bytes([ID_GET_STATUS])), c.DONE)
        self.assertEqual(con.damaged, len(data))

    def test_run_ends_with_the_number_of_damaged_frames(self):
        """Check that a run during which two datagrams were damaged prints
        `damaged frames dropped: 2` at its end and still exits 0, and that a clean run prints
        no such line.

        Why: REQ-093; the operator reads the total at the end of the run (contract item 3).
        Fails if: the summary line is missing, shows another number, or is printed when nothing
        was damaged.

        Args:
            self: the test case.

        Returns:
            None.
        """
        data = [d for _, d in damaged_datagrams()[:2]]
        self.start_fake(damaged=data)
        done = self.run_cli("upload", GOOD_SMALL_IMAGE, "--number", "1")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn(f"damaged frames dropped: {len(data)}", done.stdout)
        self.assertEqual(len(re.findall(r"damaged frame dropped", done.stdout)), len(data))
        self.start_fake()
        done = self.run_cli("upload", GOOD_SMALL_IMAGE, "--number", "1")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertNotIn("damaged", done.stdout)


class PendingTests(FakeCase):
    """REQ-094: Console.pending() and the `waiting:` lines."""

    WAITING = re.compile(r"^waiting: seq (\d+) (.+), sent ([0-9.]+) s ago, RECEIVED (yes|no)$",
                         re.MULTILINE)

    def start_command(self, con, what="GET STATUS"):
        """Run GET STATUS to the controller in a thread, so the test can look while it waits.

        Args:
            self: the test case.
            con: the Console.
            what: the command's name for messages.

        Returns:
            (thread, result dict that gets "kind" on DONE or "refused" on Refused).
        """
        result = {}

        def run():
            """Send the command and keep how it ended.

            Returns:
                None.
            """
            try:
                result["kind"] = con.command(DEST_CONTROLLER, bytes([ID_GET_STATUS]), what=what)
            except c.Refused as e:
                result["refused"] = str(e)
        thread = threading.Thread(target=run, daemon=True)
        thread.start()
        self.addCleanup(thread.join, WAIT_S)
        return thread, result

    def arrivals_of(self, seq):
        """How many times the fake received a command with this sequence number.

        Args:
            self: the test case.
            seq: sequence number.

        Returns:
            The count.
        """
        return sum(1 for _, s, _ in list(self.fake.arrivals) if s == seq)

    def test_nothing_is_pending_when_nothing_was_sent(self):
        """Check that a new Console has an empty list.

        Why: REQ-094; the list shows only commands whose answers have not arrived.
        Fails if: pending() is missing or does not return a list.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.start_fake()
        self.assertEqual(self.make_console().pending(), [])

    def test_unanswered_command_is_listed_then_leaves_on_done(self):
        """Check a command the fake does not answer: after one resend it is listed with its
        number, name, an age and received == False; once the fake answers, it ends DONE and
        the list is empty.

        Why: REQ-094; the operator must see a command whose answers have not arrived.
        Fails if: the entry lacks a key or has the wrong type, `received` is True before
        RECEIVED came, or the entry stays after DONE.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.start_fake(ignore={ID_GET_STATUS})
        con = self.make_console()
        thread, result = self.start_command(con)
        self.assertTrue(self.wait_for(lambda: self.arrivals_of(FIRST_SEQ) >= 2))
        entries = con.pending()
        self.assertEqual(len(entries), 1)
        entry = entries[0]
        self.assertTrue({"seq", "what", "age_s", "received"} <= set(entry), entry)
        self.assertEqual(entry["seq"], FIRST_SEQ)
        self.assertEqual(entry["what"], "GET STATUS")
        self.assertIsInstance(entry["age_s"], float)
        self.assertTrue(0 <= entry["age_s"] < MAX_QUOTED_AGE_S, entry["age_s"])
        self.assertIs(entry["received"], False)
        self.fake.ignore.discard(ID_GET_STATUS)
        thread.join(WAIT_S)
        self.assertEqual(result.get("kind"), c.DONE, result)
        self.assertEqual(con.pending(), [])

    def test_received_is_true_once_the_onboard_computer_said_received(self):
        """Check a command the fake answers with RECEIVED only: it is listed with
        received == True.

        Why: REQ-094; "received but not done" and "not even received" are different faults
        (Interfaces Sec. 4.2, RECEIVED).
        Fails if: `received` is never set, or is set for a command that got no answer.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.start_fake(no_done={ID_GET_STATUS})
        con = self.make_console()
        thread, result = self.start_command(con)
        self.assertTrue(self.wait_for(lambda: self.arrivals_of(FIRST_SEQ) >= 2))
        entries = con.pending()
        self.assertEqual(len(entries), 1)
        self.assertIs(entries[0]["received"], True)
        self.fake.no_done.discard(ID_GET_STATUS)
        thread.join(WAIT_S)
        self.assertEqual(result.get("kind"), c.DONE, result)
        self.assertEqual(con.pending(), [])

    def test_command_leaves_the_list_on_refused(self):
        """Check that a REFUSED command is not pending afterwards.

        Why: REQ-094; REFUSED is an answer (Interfaces Sec. 4.2).
        Fails if: the entry is removed only on DONE.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.start_fake(refuse={ID_GET_STATUS: 0x06})
        con = self.make_console()
        with self.assertRaises(c.Refused) as ctx:
            con.command(DEST_CONTROLLER, bytes([ID_GET_STATUS]))
        self.assertIn("REFUSED 0x06", str(ctx.exception))
        self.assertEqual(con.pending(), [])

    def test_command_leaves_the_list_on_already_done(self):
        """Check that a command answered ALREADY DONE is not pending afterwards.

        Why: REQ-094; ALREADY DONE ends a command like DONE (Interfaces Sec. 4.2, REQ-031).
        Fails if: the entry is removed only on DONE.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.start_fake()
        self.fake.seen[FIRST_SEQ] = c.DONE      # the fake has run this number before
        con = self.make_console()
        self.assertEqual(con.command(DEST_CONTROLLER, bytes([ID_GET_STATUS])), c.ALREADY_DONE)
        self.assertEqual(con.pending(), [])

    def test_command_given_up_leaves_the_list(self):
        """Check a command that never gets an answer: after all tries it raises Refused and
        the list is empty.

        Why: REQ-094; a command given up is no longer waiting, the operator is told by the
        refusal instead.
        Fails if: the entry stays after the last try.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.start_fake(ignore={ID_GET_STATUS})
        con = self.make_console(timeout=GIVE_UP_TIMEOUT_S)
        with self.assertRaises(c.Refused) as ctx:
            con.command(DEST_CONTROLLER, bytes([ID_GET_STATUS]), what="GET STATUS")
        self.assertIn("no answer after", str(ctx.exception))
        self.assertEqual(con.pending(), [])

    def test_waiting_line_on_every_resend_and_at_give_up_not_received(self):
        """Check the printed `waiting:` lines for a command the fake ignores: at least one per
        resend, each in the fixed format with the command's number and name and RECEIVED no.

        Why: REQ-094; the lines are what the operator sees (contract item 4).
        Fails if: lines are printed only at the end, or never, the format differs, or the
        received flag shows yes for a command that got no answer.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.start_fake(ignore={ID_GET_STATUS})
        con = self.make_console(timeout=GIVE_UP_TIMEOUT_S, quiet=False)
        with captured() as buf, self.assertRaises(c.Refused):
            con.command(DEST_CONTROLLER, bytes([ID_GET_STATUS]), what="GET STATUS")
        lines = self.WAITING.findall(buf.getvalue())
        self.assertGreaterEqual(len(lines), c.RETRIES, buf.getvalue())
        for seq, what, age, received in lines:
            self.assertEqual((int(seq), what, received), (FIRST_SEQ, "GET STATUS", "no"))
            self.assertGreaterEqual(float(age), 0.0)

    def test_waiting_line_says_received_yes_after_received(self):
        """Check the `waiting:` lines for a command the fake answers with RECEIVED only: all
        say RECEIVED yes.

        Why: REQ-094; the line tells the operator whether the onboard computer has the command.
        Fails if: the flag in the line is fixed to no, or is not taken from the answers.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.start_fake(no_done={ID_GET_STATUS})
        con = self.make_console(timeout=PENDING_TIMEOUT_S / 3, quiet=False)
        with captured() as buf, self.assertRaises(c.Refused):
            con.command(DEST_CONTROLLER, bytes([ID_GET_STATUS]), what="GET STATUS")
        lines = self.WAITING.findall(buf.getvalue())
        self.assertGreaterEqual(len(lines), c.RETRIES, buf.getvalue())
        self.assertEqual({received for *_, received in lines}, {"yes"})

    def test_no_waiting_line_for_a_command_that_is_answered(self):
        """Check that a command answered at once prints no `waiting:` line.

        Why: REQ-094; the line is a warning, not a progress report.
        Fails if: the line is printed for every command.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.start_fake()
        con = self.make_console(quiet=False)
        with captured() as buf:
            con.command(DEST_CONTROLLER, bytes([ID_GET_STATUS]))
        self.assertNotIn("waiting:", buf.getvalue())


class ReturnTests(FakeCase):
    """REQ-113: the `return` subcommand."""

    def test_return_sends_arm_naming_0x21_then_return_to_previous(self):
        """Check the two frames of `return`: ARM (0x0D, 0x21, number of the next command) and
        then payload 0x21, both to the controller, with consecutive sequence numbers.

        Why: REQ-113, REQ-112; Interfaces lines 357 and 359: RETURN TO PREVIOUS is dangerous,
        so its ARM names it and the number it will carry.
        Fails if: the identifier is another one (for example CONFIRM 0x26 copied), the
        destination is 0x01, the number in the ARM is off by one, the order is swapped, or a
        third command is sent.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.start_fake(accept={ID_RETURN})
        done = self.run_cli("return", tool=None)
        text = done.stdout + done.stderr
        self.assertNotIn("invalid choice", text)
        self.assertNotIn("Traceback", text)
        frames = commands_received(self.fake)
        self.assertEqual(len(frames), 2, text)
        arm, ret = frames
        self.assertEqual((arm["dest"], ret["dest"]), (DEST_CONTROLLER, DEST_CONTROLLER))
        self.assertEqual(ret["payload"], bytes([ID_RETURN]))
        self.assertEqual(arm["payload"], struct.pack("<BBI", ID_ARM, ID_RETURN, ret["seq"]))
        self.assertEqual(ret["seq"], arm["seq"] + 1)

    def test_return_refused_by_the_controller_ends_as_failed(self):
        """Check that a REFUSED answer to RETURN TO PREVIOUS gives exit status 1 and the code.

        Why: REQ-113; the operator must see that the controller did not return.
        Fails if: the refusal is swallowed (status 0) or becomes a traceback.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.start_fake(refuse={ID_RETURN: 0x05})
        done = self.run_cli("return", tool=None)
        text = done.stdout + done.stderr
        self.assertEqual(done.returncode, 1, text)
        self.assertIn("REFUSED 0x05", done.stdout)
        self.assertNotIn("Traceback", text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
