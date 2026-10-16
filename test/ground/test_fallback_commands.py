#!/usr/bin/env python3
"""Tests of the ground side of owner decision 30 (15 Oct 2026), drafted from the contract.

Under test: subcommand `start-failed --copy A|B` (ARM naming 0x22 and its sequence number, then
0x22 and the copy byte 0x01 or 0x02, both to the controller), subcommand `leave-fallback` (one
frame, payload 0x23, to the controller), the text of refusal 0x13, and HEALTH mode 0x03 decoded
as FALLBACK. A fake onboard computer on localhost (fake_onboard.py) stands in for the board.

These tests were written from the contract while another agent wrote the code (rule E4).
Each test says in its docstring which defect makes it fail (rule E1).

Interfaces means docs/interfaces/main.tex: command table (Sec. 5.1) and refusal table (Sec. 4.2).
"""
import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_ground_checks import (  # noqa: E402
    DEST_CONTROLLER, ID_ARM, ID_LEAVE_FALLBACK, ID_START_FAILED, FakeCase, commands_received)
import console as c  # noqa: E402

COPY_A, COPY_B = 0x01, 0x02          # START FAILED copy byte (Interfaces Sec. 5.1, table row 0x22)
REFUSAL_GIVEN_UP = 0x13              # Interfaces Sec. 4.2 table: the image was given up (decision 30)
GIVEN_UP_WORDS = "the image was given up; start failed starts it again"   # compared lower case
MODE_FALLBACK = 0x03                 # HEALTH mode FALLBACK (Interfaces Sec. 5.4)
USAGE_ERROR = 2                      # exit status of an argparse usage error


class StartFailedTests(FakeCase):
    """The `start-failed` subcommand."""

    def run_start_failed(self, copy):
        """Run `start-failed --copy <copy>` against a fake that accepts 0x22.

        Args:
            self: the test case.
            copy: the --copy argument.

        Returns:
            (completed process, ARM frame, START FAILED frame); frames are None if not sent.
        """
        self.start_fake(accept={ID_START_FAILED})
        done = self.run_cli("start-failed", "--copy", copy, tool=None)
        frames = commands_received(self.fake)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertEqual(len(frames), 2, done.stdout + done.stderr)
        return done, frames[0], frames[1]

    def check_pair(self, copy, byte):
        """Check the two frames sent for one copy.

        Args:
            self: the test case.
            copy: the --copy argument, "A" or "B".
            byte: the copy byte the command must carry.

        Returns:
            None.
        """
        _, arm, cmd = self.run_start_failed(copy)
        self.assertEqual((arm["dest"], cmd["dest"]), (DEST_CONTROLLER, DEST_CONTROLLER))
        self.assertEqual(cmd["payload"], bytes([ID_START_FAILED, byte]))
        self.assertEqual(arm["payload"], struct.pack("<BBI", ID_ARM, ID_START_FAILED, cmd["seq"]))
        self.assertEqual(cmd["seq"], arm["seq"] + 1)

    def test_copy_a_sends_arm_naming_0x22_then_0x22_0x01(self):
        """Check the frames of `start-failed --copy A`: ARM (0x0D, 0x22, number of the next
        command), then payload 0x22 0x01, both to the controller, consecutive numbers.

        Why: decision 30; Interfaces command table row 0x22: dangerous, copy (1), 0x01 is A.
        Fails if: the ARM names another command (0x20 copied from `switch`), the copy byte for A
        is not 0x01, the destination is the onboard computer, the number in the ARM is off by
        one, the ARM is missing or comes second, or a third frame is sent.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.check_pair("A", COPY_A)

    def test_copy_b_sends_0x22_0x02(self):
        """Check that `start-failed --copy B` carries copy byte 0x02 in an otherwise identical pair.

        Why: decision 30; Interfaces command table row 0x22: 0x02 is B.
        Fails if: the copy is ignored and always A (0x01) is sent, or the bytes for A and B are
        swapped, or B is sent as ASCII 'B' (0x42).

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.check_pair("B", COPY_B)

    def test_copy_other_than_a_or_b_is_a_usage_error_and_nothing_is_sent(self):
        """Check that --copy with a value other than A or B exits 2 and sends no datagram, and
        that a missing --copy does the same.

        Why: decision 30; the copy byte is the safety of this command, so a guess is never sent.
        Fails if: C, AB, 1 or an empty value is sent as some byte (for example a default of A),
        the exit status is 0 or 1, or a traceback is printed instead of a usage message.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.start_fake(accept={ID_START_FAILED})
        for args in (("--copy", "C"), ("--copy", "AB"), ("--copy", "1"), ("--copy", ""), ()):
            done = self.run_cli("start-failed", *args, tool=None)
            text = done.stdout + done.stderr
            self.assertEqual(done.returncode, USAGE_ERROR, f"{args}: {text}")
            self.assertNotIn("Traceback", text, f"{args}")
        self.assert_nothing_sent()

    def test_refused_0x13_prints_the_text_and_exits_1(self):
        """Check that a controller REFUSED 0x13 to START FAILED prints the code and its words
        and ends with exit status 1.

        Why: decision 30; the operator must read what to do next ("START FAILED starts it again").
        Fails if: 0x13 is printed as "unknown code 0x13" (console table lacks it), the status is
        0, or the refusal becomes a traceback.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.start_fake(refuse={ID_START_FAILED: REFUSAL_GIVEN_UP})
        done = self.run_cli("start-failed", "--copy", "A", tool=None)
        text = done.stdout + done.stderr
        self.assertEqual(done.returncode, 1, text)
        self.assertIn("REFUSED 0x13", done.stdout)
        self.assertIn(GIVEN_UP_WORDS, done.stdout.lower())
        self.assertNotIn("unknown", done.stdout.lower())
        self.assertNotIn("Traceback", text)


class LeaveFallbackTests(FakeCase):
    """The `leave-fallback` subcommand."""

    def test_sends_exactly_one_frame_with_payload_0x23_to_the_controller(self):
        """Check that `leave-fallback` sends one command frame, payload 0x23, Destination 0x02,
        and no ARM.

        Why: decision 30; Interfaces command table row 0x23: not dangerous, no ARM.
        Fails if: an ARM is sent first (copied from `switch`), the destination is 0x01, a
        parameter byte is appended, or the command is sent twice.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.start_fake(accept={ID_LEAVE_FALLBACK})
        done = self.run_cli("leave-fallback", tool=None)
        text = done.stdout + done.stderr
        self.assertEqual(done.returncode, 0, text)
        self.assertNotIn("Traceback", text)
        frames = commands_received(self.fake)
        self.assertEqual(len(frames), 1, text)
        self.assertEqual(frames[0]["dest"], DEST_CONTROLLER)
        self.assertEqual(frames[0]["payload"], bytes([ID_LEAVE_FALLBACK]))

    def test_refused_by_the_controller_ends_as_failed(self):
        """Check that a REFUSED answer to LEAVE FALLBACK gives exit status 1 and the code.

        Why: decision 30; the operator must see that the controller stayed in FALLBACK.
        Fails if: the refusal is swallowed (status 0) or becomes a traceback.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.start_fake(refuse={ID_LEAVE_FALLBACK: 0x05})
        done = self.run_cli("leave-fallback", tool=None)
        text = done.stdout + done.stderr
        self.assertEqual(done.returncode, 1, text)
        self.assertIn("REFUSED 0x05", done.stdout)
        self.assertNotIn("Traceback", text)


class DecodeTests(unittest.TestCase):
    """The console's tables for decision 30."""

    def test_refusal_0x13_has_the_contract_words(self):
        """Check the console's refusal table: 0x13 reads "the image was given up; START FAILED
        starts it again" (case aside).

        Why: decision 30; REQ-100: ground and onboard computer use one refusal list.
        Fails if: 0x13 is missing (reads "unknown code 0x13"), or carries another text.

        Args:
            self: the test case.

        Returns:
            None.
        """
        self.assertIn(GIVEN_UP_WORDS, c.refusal_text(REFUSAL_GIVEN_UP).lower())

    def test_health_mode_0x03_decodes_as_fallback(self):
        """Check that a HEALTH report with mode 0x03 decodes to mode "FALLBACK", with and
        without the onboard computer's two added fields.

        Why: decision 30; the operator sees FALLBACK before sending LEAVE FALLBACK.
        Fails if: mode 0x03 reads "unknown code 0x03" or another mode name.

        Args:
            self: the test case.

        Returns:
            None.
        """
        body = c.HEALTH_BODY.pack(1, c.REPORT_HEALTH, MODE_FALLBACK, 1, 1, 1, 0, 0, 0)
        for label, payload in (("with added fields", c.ADDED.pack(1, 1) + body),
                               ("without", body)):
            decoded = c.decode_report(payload)
            self.assertIsNotNone(decoded, label)
            self.assertEqual(decoded["kind"], "HEALTH", label)
            self.assertEqual(decoded["mode"], "FALLBACK", label)


if __name__ == "__main__":
    unittest.main(verbosity=2)
