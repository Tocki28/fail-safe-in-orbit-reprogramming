#!/usr/bin/env python3
"""Tests of the bench frame sender send_frame.py against a local UDP responder (127.0.0.1 only).

No board and no network: every socket is bound to 127.0.0.1 on an ephemeral port (port 0). The
console is imported here only to compare bytes and to check the sequence-number rule; the tool
under test does not import it.
"""
import contextlib
import io
import os
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / "src" / "ground"))
import console as c  # noqa: E402
import send_frame as s  # noqa: E402

LOCAL = "127.0.0.1"             # tests never leave the machine (brief: never send to the board)
LISTEN_S = "0.4"                # listen window of the tests; the real default is 3 s
LISTEN_SLACK_S = 2.0            # allowed overrun of a listen window on a loaded machine
GAP_S = 0.2                     # gap between frames in the timing tests
GAP_TOLERANCE_S = 0.05          # a sleep may end a little early on the clock used here
GET_STATUS = 0x25               # Interfaces Sec. 5.1: GET STATUS, no fields
SWITCH = 0x20                   # Interfaces Sec. 5.1: SWITCH, dangerous
SEQ_START = 100                 # an arbitrary last-used number in the state file
EXPIRY_TEST = 1_900_000_000     # an arbitrary expiry value, Unix time
EXPIRY_IN_S = 60                # an arbitrary --expiry-in
EXPIRY_TOLERANCE_S = 5          # allowed clock difference between the call and the check
RESPONDER_POLL_S = 0.05         # responder receive timeout, so it can see the stop flag
JOIN_S = 2.0                    # bound on waiting for the responder thread to end
GARBAGE = b"\x00\x01\x02\x03\x04"   # a datagram that is no frame
UNRELATED_SEQ = 7               # a sequence number the tool never sent


class Responder:
    """A local UDP socket that records datagrams and answers with a chosen list of answers."""

    def __init__(self, answers=(), unrelated=False, garbage=False):
        """Bind to 127.0.0.1 on an ephemeral port and start the receive thread.

        Args:
            answers: kinds to answer with for each command received, in order, e.g. (0x01, 0x02).
            unrelated: True to answer sequence number UNRELATED_SEQ instead of the received one.
            garbage: True to send GARBAGE back instead of answers.
        """
        self.answers = answers
        self.unrelated = unrelated
        self.garbage = garbage
        self.received = []                  # datagrams as received
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((LOCAL, 0))
        self.sock.settimeout(RESPONDER_POLL_S)
        self.port = self.sock.getsockname()[1]
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        """Receive until stopped; record each datagram and send the configured reply."""
        while not self.stop.is_set():
            try:
                data, source = self.sock.recvfrom(s.RECV_BYTES)
            except socket.timeout:
                continue
            self.received.append(data)
            frame = c.parse(data)
            if self.garbage:
                self.sock.sendto(GARBAGE, source)
            elif frame:
                number = UNRELATED_SEQ if self.unrelated else frame["seq"]
                for kind in self.answers:
                    reply = c.build(c.TYPE_ANSWER, c.DEST_GROUND, 1, c.ANSWER.pack(number, kind, 0))
                    self.sock.sendto(reply, source)

    def close(self):
        """Stop the thread and close the socket."""
        self.stop.set()
        self.thread.join(JOIN_S)
        self.sock.close()


class SendFrameTests(unittest.TestCase):
    def setUp(self):
        """Make a temporary state file path, never the user's ~/.fsir.

        Why: a test run must not move the real console's sequence numbers.
        """
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.seq_file = str(Path(tmp.name) / "state" / "console-seq")

    def respond(self, **kwargs):
        """Start a Responder that is closed at the end of the test.

        Args:
            **kwargs: passed to Responder.

        Returns:
            The Responder.
        """
        r = Responder(**kwargs)
        self.addCleanup(r.close)
        return r

    def run_tool(self, responder, *args, seq_file=True, listen=LISTEN_S):
        """Run send_frame.main() against a responder and capture the printed lines.

        Args:
            responder: the Responder to talk to.
            *args: subcommand and its options.
            seq_file: True to use this test's state file; False to give no --seq-file option
                (the caller then passes --seq itself and a --seq-file of its own, if wanted).
            listen: the --listen value, seconds as text.

        Returns:
            (exit status, printed text).
        """
        out = io.StringIO()
        base = ["--host", LOCAL, "--port", str(responder.port), "--listen", listen]
        if seq_file:
            base += ["--seq-file", self.seq_file]
        code = s.main(base + list(args), out=out)
        return code, out.getvalue()

    def write_state(self, number):
        """Write the state file as the console leaves it.

        Args:
            number: the last-used sequence number.
        """
        c.save_seq(self.seq_file, number)

    def sent_frames(self, responder):
        """The command frames the responder received, parsed by the console.

        Args:
            responder: the Responder.

        Returns:
            list of frame dicts (type, dest, seq, payload).
        """
        return [c.parse(d) for d in responder.received]

    def test_get_status_bytes_equal_console_build(self):
        """Catches a wrong field order, width or byte order in the tool's own frame builder.

        The tool does not import the console, so its bytes for GET STATUS (dest 0x02, id 0x25)
        are compared with console.build() for the same fields (Interfaces Sec. 4.0).
        """
        mine = s.build_frame(s.VERSION, s.TYPE_COMMAND, c.DEST_CONTROLLER, 0, 5, EXPIRY_TEST,
                             bytes([GET_STATUS]))
        theirs = c.build(c.TYPE_COMMAND, c.DEST_CONTROLLER, 5, bytes([GET_STATUS]), EXPIRY_TEST)
        self.assertEqual(mine, theirs)
        responder = self.respond()
        self.run_tool(responder, "--expiry", str(EXPIRY_TEST), "--seq", "5", "raw", "--dest", "2",
                      "--id", "0x25")
        self.assertEqual(responder.received, [theirs])

    def test_crc_is_zlib_crc32_little_endian(self):
        """Catches a CRC over the wrong span, the wrong byte order, or a non-zlib polynomial.

        The Interfaces CRC-32 (Sec. 4.0) is the zlib one over every byte before the check value.
        """
        frame = s.build_frame(s.VERSION, s.TYPE_COMMAND, 1, 0, 9, 0, b"\x04\x01\x00")
        self.assertEqual(struct.unpack("<I", frame[-4:])[0], zlib.crc32(frame[:-4]))
        self.assertIsNotNone(s.parse_frame(frame))

    def test_flip_byte_changes_exactly_one_byte_and_breaks_the_frame(self):
        """Catches --flip-byte missing the CRC bytes, hitting two bytes, or leaving a valid frame.

        For every position K of a GET STATUS frame, the damaged frame differs from the good one
        in exactly byte K, by XOR 0x01, and the console's parser rejects it (Sec. 4.0).
        """
        good = s.build_frame(s.VERSION, s.TYPE_COMMAND, 2, 0, 5, 0, bytes([GET_STATUS]))
        self.assertIsNotNone(c.parse(good))
        for k in range(len(good)):
            bad = s.flip_byte(good, k)
            diff = [i for i in range(len(good)) if good[i] != bad[i]]
            self.assertEqual(diff, [k])
            self.assertEqual(good[k] ^ bad[k], 0x01)
            self.assertIsNone(c.parse(bad), f"flipping byte {k} still parses")

    def test_flip_byte_on_the_wire_and_out_of_range(self):
        """Catches --flip-byte applied before the CRC (a valid frame sent) or an unchecked index.

        The datagram that arrives must fail the console's parser; a position past the end of the
        frame is an argument error (exit 2), not a silent no-op.
        """
        responder = self.respond()
        self.run_tool(responder, "raw", "--dest", "2", "--id", "0x25", "--flip-byte", "20")
        self.assertEqual(len(responder.received), 1)
        self.assertIsNone(c.parse(responder.received[0]))
        code, _ = self.run_tool(responder, "raw", "--dest", "2", "--id", "0x25",
                                "--flip-byte", "21")
        self.assertEqual(code, s.EXIT_BAD_ARGS)     # frame is 21 bytes: positions 0 to 20

    def test_version_option_sends_a_wrong_version_with_a_good_crc(self):
        """Catches --version being ignored, or the CRC not covering the changed version byte.

        The receiver must see a wrong version with an otherwise valid check value, so that only
        the version check can refuse it (Sec. 4.0); the console's parser rejects it for the version.
        """
        responder = self.respond()
        self.run_tool(responder, "raw", "--dest", "2", "--id", "0x25", "--version", "2",
                      "--flags", "0x80")
        data = responder.received[0]
        self.assertEqual(data[2], 2)
        self.assertEqual(data[5], 0x80)     # Flags is byte 5: Sync (2), Version, Type, Dest
        self.assertEqual(struct.unpack("<I", data[-4:])[0], zlib.crc32(data[:-4]))
        self.assertIsNone(c.parse(data))

    def test_arm_payload_layout_and_named_sequence(self):
        """Catches a wrong ARM payload order or width, or an ARM naming a number the command lacks.

        ARM is 0x0D, command id (1), the sequence number the command will carry (4, little-endian);
        the command follows with the next number (Sec. 5.1, Sec. 5.2).
        """
        self.write_state(SEQ_START)
        responder = self.respond()
        self.run_tool(responder, "armed", "--dest", "2", "--cmd", "0x20")
        arm, cmd = self.sent_frames(responder)
        self.assertEqual(arm["seq"], SEQ_START + 1)
        self.assertEqual(cmd["seq"], SEQ_START + 2)
        self.assertEqual(arm["payload"], struct.pack("<BBI", 0x0D, SWITCH, cmd["seq"]))
        self.assertEqual(cmd["payload"], bytes([SWITCH]))
        self.assertEqual((arm["dest"], cmd["dest"]), (2, 2))

    def test_armed_variants_no_arm_names_and_id(self):
        """Catches --no-arm still sending an ARM, and --arm-names / --arm-id being ignored.

        These are the wrong-arming cases the console never sends (Sec. 5.2, REFUSED 0x01).
        """
        responder = self.respond()
        self.run_tool(responder, "armed", "--dest", "2", "--cmd", "0x20", "--no-arm")
        self.assertEqual(len(responder.received), 1)
        responder.received.clear()
        self.run_tool(responder, "armed", "--dest", "2", "--cmd", "0x20", "--arm-names", "4242",
                      "--arm-id", "0x26")
        arm, cmd = self.sent_frames(responder)
        self.assertEqual(arm["payload"], struct.pack("<BBI", 0x0D, 0x26, 4242))
        self.assertNotEqual(cmd["seq"], 4242)

    def test_armed_gap_is_waited_between_the_frames(self):
        """Catches --gap being ignored, or the ARM and the command going out back to back.

        The ARM expires after a time (Sec. 5.2, REQ-030), so the gap between the two is the test.
        """
        responder = self.respond()
        times = []
        real_send = s.Session.send

        def noting_send(session, frame):
            """Record the time of each send, then send.

            Args:
                session: the Session.
                frame: the frame bytes.
            """
            times.append(time.monotonic())
            real_send(session, frame)

        s.Session.send = noting_send
        try:
            self.run_tool(responder, "armed", "--dest", "2", "--cmd", "0x20", "--gap", str(GAP_S))
        finally:
            s.Session.send = real_send
        self.assertEqual(len(times), 2)
        self.assertGreaterEqual(times[1] - times[0], GAP_S - GAP_TOLERANCE_S)

    def test_repeat_sends_identical_bytes(self):
        """Catches repeat taking a new sequence number, or changing a byte, between sends.

        A command sent again with the same number is answered ALREADY DONE (Sec. 4.2, REQ-031);
        a new number would defeat the test. The default is two sends, and --times N gives N.
        """
        responder = self.respond()
        self.run_tool(responder, "repeat", "--dest", "2", "--id", "0x25", "--gap", "0.05")
        self.assertEqual(len(responder.received), s.TIMES_DEFAULT)
        self.assertEqual(len(set(responder.received)), 1)
        responder.received.clear()
        self.run_tool(responder, "repeat", "--dest", "2", "--id", "0x25", "--times", "3")
        self.assertEqual(len(responder.received), 3)
        self.assertEqual(len(set(responder.received)), 1)

    def test_repeat_gap_is_waited(self):
        """Catches the gap of repeat being ignored (the second send must come --gap later)."""
        responder = self.respond()
        out = io.StringIO()
        t0 = time.monotonic()
        s.main(["--host", LOCAL, "--port", str(responder.port), "--listen", "0",
                "--seq-file", self.seq_file, "repeat", "--dest", "2", "--id", "0x25",
                "--gap", str(GAP_S)], out=out)
        self.assertGreaterEqual(time.monotonic() - t0, GAP_S - GAP_TOLERANCE_S)

    def test_cancel_payload_and_destination(self):
        """Catches a wrong UPLOAD CANCEL layout (id 0x04, image number 2 bytes little-endian).

        UPLOAD CANCEL goes to the onboard computer, Destination 0x01 (Sec. 5.1).
        """
        responder = self.respond()
        self.run_tool(responder, "cancel", "--number", "0x1234")
        frame = self.sent_frames(responder)[0]
        self.assertEqual(frame["payload"], struct.pack("<BH", 0x04, 0x1234))
        self.assertEqual(frame["dest"], 1)
        self.assertEqual(frame["type"], 1)

    def test_sequence_file_numbers_are_never_reused_by_the_console(self):
        """Catches a tool run that leaves the state file at or below a number it used.

        After the tool takes numbers, the console's own rule (load_first_seq) must start above
        the last one, or the onboard computer would refuse the console's ARM with 0x04 (REQ-065).
        """
        self.write_state(SEQ_START)
        responder = self.respond()
        self.run_tool(responder, "armed", "--dest", "2", "--cmd", "0x20")
        arm, cmd = self.sent_frames(responder)
        self.assertEqual((arm["seq"], cmd["seq"]), (SEQ_START + 1, SEQ_START + 2))
        first, note = c.load_first_seq(self.seq_file)
        self.assertEqual(first, cmd["seq"] + 1)
        self.assertIsNone(note)
        self.assertEqual(s.first_seq(self.seq_file)[0], first)      # tool and console agree

    def test_sequence_file_missing_starts_from_the_clock_and_is_created(self):
        """Catches the console-rule fallback being wrong when there is no state file.

        With no file the console starts from the clock (load_first_seq); the tool must do the same
        and then leave the file for the console to continue from.
        """
        before = int(time.time())
        responder = self.respond()
        self.run_tool(responder, "cancel", "--number", "1")
        frame = self.sent_frames(responder)[0]
        self.assertGreaterEqual(frame["seq"], before)
        self.assertEqual(c.load_first_seq(self.seq_file)[0], frame["seq"] + 1)
        none = os.path.join(os.path.dirname(self.seq_file), "none")
        self.assertEqual(s.first_seq(none, 77)[0], c.load_first_seq(none, 77)[0])

    def test_explicit_seq_never_lowers_the_state_file(self):
        """Catches --seq N writing a low number over the file, which the console would then reuse.

        With the file at 100, --seq 50 sends 50 and leaves 100; --seq 200 moves the file to 200.
        """
        self.write_state(SEQ_START)
        responder = self.respond()
        out = io.StringIO()
        base = ["--host", LOCAL, "--port", str(responder.port), "--listen", "0",
                "--seq-file", self.seq_file]
        s.main(["--seq", "50"] + base + ["cancel", "--number", "1"], out=out)
        self.assertEqual(self.sent_frames(responder)[0]["seq"], 50)
        self.assertEqual(c.load_first_seq(self.seq_file)[0], SEQ_START + 1)
        s.main(["--seq", "200"] + base + ["cancel", "--number", "1"], out=out)
        self.assertEqual(c.load_first_seq(self.seq_file)[0], 201)

    def test_answers_give_exit_0_and_decoded_lines(self):
        """Catches wrong answer decoding (sequence, kind words, code hex) and a wrong exit status.

        A responder answers RECEIVED then DONE (Sec. 4.2): exit 0, one sent line and both answers.
        """
        self.write_state(SEQ_START)
        responder = self.respond(answers=(0x01, 0x02))
        code, text = self.run_tool(responder, "raw", "--dest", "2", "--id", "0x25")
        self.assertEqual(code, s.EXIT_ANSWERED)
        lines = text.splitlines()
        self.assertRegex(lines[0], r"^sent \d\d:\d\d:\d\d\.\d{3} seq=101 dest=0x02 payload=25$")
        self.assertRegex(lines[1],
                         r"^recv \d\d:\d\d:\d\d\.\d{3} answer seq=101 RECEIVED code=0x00$")
        self.assertRegex(lines[2], r"^recv \d\d:\d\d:\d\d\.\d{3} answer seq=101 DONE code=0x00$")

    def test_refused_answer_is_decoded_with_its_code(self):
        """Catches a REFUSED code being dropped or the kind shown as a number (Sec. 4.2)."""
        responder = self.respond(answers=(0x03, 0x04))
        _, text = self.run_tool(responder, "raw", "--dest", "2", "--id", "0x25")
        self.assertIn("REFUSED code=0x00", text)
        self.assertIn("ALREADY DONE code=0x00", text)

    def test_no_answer_gives_exit_1_within_the_listen_time(self):
        """Catches a missing or unbounded wait: silence must end the run with exit 1 in time."""
        responder = self.respond()
        t0 = time.monotonic()
        code, text = self.run_tool(responder, "raw", "--dest", "2", "--id", "0x25")
        elapsed = time.monotonic() - t0
        self.assertEqual(code, s.EXIT_SILENT)
        self.assertGreaterEqual(elapsed, float(LISTEN_S) - GAP_TOLERANCE_S)
        self.assertLess(elapsed, float(LISTEN_S) + LISTEN_SLACK_S)
        self.assertNotIn("recv", text)

    def test_answer_to_another_sequence_number_gives_exit_1(self):
        """Catches counting any answer as success, including one to a number the tool never sent."""
        responder = self.respond(answers=(0x02,), unrelated=True)
        code, text = self.run_tool(responder, "raw", "--dest", "2", "--id", "0x25")
        self.assertEqual(code, s.EXIT_SILENT)
        self.assertIn(f"answer seq={UNRELATED_SEQ} DONE", text)

    def test_undecodable_datagram_is_printed_with_its_length(self):
        """Catches a crash or a silent drop on a datagram that is no frame (exit 1, no answer)."""
        responder = self.respond(garbage=True)
        code, text = self.run_tool(responder, "raw", "--dest", "2", "--id", "0x25")
        self.assertEqual(code, s.EXIT_SILENT)
        self.assertIn(f"undecodable ({len(GARBAGE)} bytes)", text)

    def test_report_line_shows_length_and_kind(self):
        """Catches a report printed as undecodable, or with the wrong kind (Sec. 5.4).

        A 16-byte HEALTH body (kind byte 0x01 at offset 4) and a 24-byte one with the onboard
        computer's added fields; an EVENT with the added fields.
        """
        health = bytes(4) + b"\x01" + bytes(11)
        self.assertEqual(s.report_kind_text(health), "HEALTH")
        self.assertEqual(s.report_kind_text(bytes(8) + health), "HEALTH")
        event = bytes(8) + bytes(4) + b"\x02" + b"\x07\x00"
        self.assertEqual(s.report_kind_text(event), "EVENT")
        responder = self.respond()
        session = s.Session(LOCAL, responder.port, io.StringIO())
        self.addCleanup(session.close)
        session.show(c.build(c.TYPE_REPORT, c.DEST_GROUND, 1, health))
        self.assertIn("report 16 bytes, HEALTH", session.out.getvalue())

    def test_expiry_options(self):
        """Catches --expiry / --expiry-in being ignored, or a non-zero default.

        The Expiry field is 0 unless given (brief); --expiry-in S is now + S seconds, Unix time.
        """
        responder = self.respond()
        self.run_tool(responder, "raw", "--dest", "2", "--id", "0x25")
        self.run_tool(responder, "--expiry", str(EXPIRY_TEST), "raw", "--dest", "2", "--id", "0x25")
        now = time.time()
        self.run_tool(responder, "--expiry-in", str(EXPIRY_IN_S), "raw", "--dest", "2",
                      "--id", "0x25")
        expiries = [struct.unpack_from("<I", d, 12)[0] for d in responder.received]
        self.assertEqual(expiries[0], 0)
        self.assertEqual(expiries[1], EXPIRY_TEST)
        self.assertAlmostEqual(expiries[2], now + EXPIRY_IN_S, delta=EXPIRY_TOLERANCE_S)

    def test_bad_arguments_exit_2_and_send_nothing(self):
        """Catches unbounded waits and out-of-range values being accepted (rule B2).

        A listen time past the cap, a gap past the cap, zero repeats and both expiry options at
        once are refused by the parser (exit status 2) before anything is sent.
        """
        responder = self.respond()
        bad = [["--listen", "601", "raw", "--dest", "2", "--id", "1"],
               ["armed", "--dest", "2", "--cmd", "1", "--gap", "61"],
               ["repeat", "--dest", "2", "--id", "1", "--times", "0"],
               ["--expiry", "1", "--expiry-in", "1", "raw", "--dest", "2", "--id", "1"],
               ["raw", "--dest", "256", "--id", "1"]]
        for argv in bad:
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as caught:
                    s.main(["--host", LOCAL, "--port", str(responder.port),
                            "--seq-file", self.seq_file] + argv)
            self.assertEqual(caught.exception.code, s.EXIT_BAD_ARGS)
        self.assertEqual(responder.received, [])

    def test_source_stays_on_the_bench_rules(self):
        """Catches an import of the console, or a line over 100 characters, in the tool.

        The tool must carry its own frame code (the console refuses malformed commands) and keep
        to the code rules' line length.
        """
        text = (HERE / "send_frame.py").read_text(encoding="utf-8")
        self.assertNotRegex(text, r"(?m)^\s*(import|from)\s+console\b")
        long_lines = [i for i, line in enumerate(text.splitlines(), 1) if len(line) > 100]
        self.assertEqual(long_lines, [])


if __name__ == "__main__":
    unittest.main()
