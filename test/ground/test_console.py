#!/usr/bin/env python3
"""Ground console tests against a fake onboard computer on localhost (no board, no network)."""
import contextlib
import io
import random
import re
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / "src" / "ground"))
import console as c  # noqa: E402
from fake_onboard import FakeOnboard  # noqa: E402

FAULT_CODES_H = HERE.parents[1] / "src" / "controller" / "common" / "fault_codes.h"
IMAGES = HERE.parents[1] / "src" / "controller" / "build" / "images"
FAST_S = "0.3"           # command timeout in tests; real value is 3 s
SEQ_FILE = None          # sequence state file of the running test; set in setUp, never the user's ~/.fsir


def run_console(fake, *args):
    """Run console.main() against the fake and capture its output.

    Args:
        fake: the FakeOnboard to talk to.
        *args: console arguments after the common --host, --port and --timeout.

    Returns:
        (exit code, text printed to stdout). The sequence state file is the test's own (SEQ_FILE),
        unless the arguments give --seq-file themselves.
    """
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = c.main(["--host", "127.0.0.1", "--port", str(fake.port), "--timeout", FAST_S,
                       "--seq-file", SEQ_FILE, *args])
    return code, out.getvalue()


class ConsoleTests(unittest.TestCase):
    def setUp(self):
        """Start a fake onboard computer and find the slot B image; fails if the image is not built."""
        global SEQ_FILE
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        SEQ_FILE = str(self.tmp / "state" / "console-seq")
        self.fake = FakeOnboard()
        self.addCleanup(self.fake.close)
        self.image = IMAGES / "firmware-B.img"
        self.assertTrue(self.image.exists(), "run `make -C src/controller` first")

    def update_args(self, *extra):
        """Arguments of the `update` subcommand with the image directory set.

        Args:
            *extra: further arguments, such as --yes.

        Returns:
            The argument list.
        """
        return ["update", "--image-dir", str(IMAGES), *extra]

    def wait_for(self, condition, seconds=5.0):
        """Poll until a condition holds or the time is up.

        Args:
            condition: function returning True when done.
            seconds: longest wait, seconds.

        Returns:
            The condition's last value.
        """
        end = time.monotonic() + seconds
        while time.monotonic() < end and not condition():
            time.sleep(0.05)
        return condition()

    def test_frame_roundtrip_and_bad_crc(self):
        """Check that a built frame parses back with its sequence number and a flipped CRC bit is
        dropped (Sec. 4.0). Passes if parse() returns the frame, then None.
        """
        f = c.build(c.TYPE_COMMAND, 2, 7, b"\x25", 99)
        self.assertEqual(c.parse(f)["seq"], 7)
        self.assertIsNone(c.parse(f[:-1] + bytes([f[-1] ^ 1])))

    def test_upload_bytes_equal_image_and_stop_and_wait(self):
        """Check that upload sends the image byte for byte and every command waits for the previous
        answer (Sec. 4.2). Passes if the received bytes equal the file and stop-and-wait held.
        """
        code, out = run_console(self.fake, "upload", str(self.image), "--number", "1")
        self.assertEqual(code, 0, out)
        self.assertEqual(bytes(self.fake.upload), self.image.read_bytes())
        self.assertTrue(self.fake.ended)
        self.assertTrue(self.fake.stop_and_wait_held())

    def test_update_confirmed(self):
        """Check that `update` with the answer y sends ARM and CONFIRM and ends confirmed (Sec. 5.1,
        REQ-012). Passes if exit code is 0, the prompt was asked once, CONFIRM was the last
        command and its ARM named its own sequence number.
        """
        with mock.patch.object(c, "read_answer", return_value="y") as ask:
            code, out = run_console(self.fake, *self.update_args())
        self.assertEqual(code, 0, out)
        self.assertIn("UPDATE CONFIRMED: slot B version 2", out)
        ask.assert_called_once_with("Confirm? [y/N] ")
        self.assertIn("trial time left", out)
        self.assertEqual(bytes(self.fake.upload), self.image.read_bytes())  # free slot was B
        self.assertTrue(self.fake.stop_and_wait_held())
        self.assertIn((c.DEST_CONTROLLER, c.ARM), self.fake.log)
        self.assertEqual(self.fake.log[-1], (c.DEST_CONTROLLER, c.CONFIRM))
        self.assertEqual(self.fake.armed[0], c.CONFIRM)
        self.assertEqual(self.fake.armed[1], self.fake.arrivals[-1][1])   # ARM named CONFIRM's own number
        self.assertIn((c.DEST_CONTROLLER, c.SWITCH), self.fake.log)
        self.assertTrue(self.fake.confirmed.is_set())

    def test_update_rolled_back(self):
        """Check that `update` reports a rollback when the controller gives the firmware up without a
        test result. Passes if exit code is 1 and the old slot A version 1 is reported.
        """
        self.fake.rollback = True
        code, out = run_console(self.fake, *self.update_args())
        self.assertEqual(code, 1, out)
        self.assertIn("UPDATE ROLLED BACK: running slot A version 1", out)

    def test_update_with_radio_loss(self):
        """Check that `update --yes` still completes with 30 % of outgoing datagrams dropped (Sec.
        4.2, REQ-031). Passes if confirmed, the image arrived intact and stop-and-wait held.
        """
        code, out = run_console(self.fake, "--radio-loss", "0.3", "--seed", "5", *self.update_args("--yes"))
        self.assertEqual(code, 0, out)
        self.assertIn("UPDATE CONFIRMED: slot B version 2", out)
        self.assertIn("radio simulator: dropped", out)
        self.assertEqual(bytes(self.fake.upload), self.image.read_bytes())
        self.assertTrue(self.fake.stop_and_wait_held())

    def test_update_yes_does_not_ask(self):
        """Check that --yes answers the prompt without reading the terminal. Passes if read_answer is
        never called and the update is confirmed.
        """
        with mock.patch.object(c, "read_answer", side_effect=AssertionError("asked")):
            code, out = run_console(self.fake, *self.update_args("--yes"))
        self.assertEqual(code, 0, out)
        self.assertIn("UPDATE CONFIRMED: slot B version 2", out)
        self.assertTrue(self.fake.confirmed.is_set())

    def test_update_answer_n_sends_nothing(self):
        """Check that the answer N sends no CONFIRM. Passes if exit code is 3, the output says
        nothing was sent and the controller returns to the previous copy, and the fake gave the
        firmware up itself.
        """
        self.fake.trial_s = 1.0
        with mock.patch.object(c, "read_answer", return_value="n"):
            code, out = run_console(self.fake, *self.update_args())
        self.assertEqual(code, 3, out)
        self.assertIn("nothing sent", out)
        self.assertIn("returns to the previous copy when its trial time ends", out)
        self.assertNotIn((c.DEST_CONTROLLER, c.CONFIRM), self.fake.log)
        self.assertEqual(self.fake.log[-1], (c.DEST_CONTROLLER, c.GET_STATUS))  # the trial report; no ARM for CONFIRM
        self.assertTrue(self.wait_for(lambda: self.fake.state["slot"] == 1))   # fake gave the firmware up

    def test_update_no_terminal_means_no_answer(self):
        """Check that input that is not a terminal counts as no answer. Passes if exit code is 3, the
        output says "no answer" and no CONFIRM was sent.
        """
        self.fake.trial_s = 1.0
        with mock.patch.object(c.sys, "stdin", io.StringIO("y\n")):    # piped, not a terminal
            code, out = run_console(self.fake, *self.update_args())
        self.assertEqual(code, 3, out)
        self.assertIn("no answer", out)
        self.assertNotIn((c.DEST_CONTROLLER, c.CONFIRM), self.fake.log)

    def test_update_trial_test_failed(self):
        """Check the failed-test path: the failed test is named and the old slot is reported running
        again. Passes if exit code is 1, the output names the data-link answer test and slot A in
        OPERATE, no CONFIRM was sent and the operator was not asked.
        """
        self.fake.fail_test = 0x02
        with mock.patch.object(c, "read_answer", side_effect=AssertionError("asked")):
            code, out = run_console(self.fake, *self.update_args("--yes"))
        self.assertEqual(code, 1, out)
        self.assertIn("TRIAL TESTS FAILED: trial tests failed: data-link answer", out)
        self.assertIn("returns to the previous copy", out)
        self.assertIn("UPDATE REJECTED: running slot A version 1 in OPERATE", out)
        self.assertNotIn((c.DEST_CONTROLLER, c.CONFIRM), self.fake.log)

    def test_confirm_subcommand(self):
        """Check that `confirm` sends ARM and CONFIRM to a controller whose trial tests passed (Sec.
        5.1). Passes if exit code is 0, EVENT 0x0007 and OPERATE on slot B are reported and the
        ARM named CONFIRM's own sequence number.
        """
        self.fake.state = dict(mode=2, slot=2, version=2)
        self.fake.tests_passed = True
        self.fake.armed = None
        code, out = run_console(self.fake, "confirm")
        self.assertEqual(code, 0, out)
        self.assertIn("CONFIRMED: EVENT 0x0007 seen; slot B version 2 in OPERATE", out)
        self.assertIn("answered DONE", out)
        self.assertEqual(self.fake.armed, (c.CONFIRM, self.fake.arrivals[-1][1]))

    def test_confirm_refused_when_tests_not_passed(self):
        """Check that CONFIRM before the trial tests passed is refused with 0x0F (Sec. 4.2). Passes
        if exit code is 1 and the refusal text is printed.
        """
        self.fake.state = dict(mode=2, slot=2, version=2)
        code, out = run_console(self.fake, "confirm")
        self.assertEqual(code, 1, out)
        self.assertIn("REFUSED 0x0f: Trial tests not passed", out)

    def test_failed_event_detail_decoding(self):
        """Check that EVENT 0x000B decodes its detail byte into the test name, with and without the
        onboard computer's added fields (Sec. 5.4), and that 0x000A decodes. Passes if the texts
        match.
        """
        body = c.struct.pack("<IBH", 5, c.REPORT_EVENT, c.EVENT_TESTS_FAILED) + b"\x03"
        r = c.decode_report(c.ADDED.pack(9, 8) + body)
        self.assertEqual((r["kind"], r["code"], r["report_number"]), ("EVENT", 0x000B, 9))
        self.assertEqual(r["text"], "trial tests failed: test image")
        self.assertEqual(c.decode_report(body)["text"], "trial tests failed: test image")
        r = c.decode_report(c.struct.pack("<IBH", 5, c.REPORT_EVENT, c.EVENT_TESTS_PASSED))
        self.assertEqual(r["text"], "trial tests passed")

    def test_upload_with_radio_loss_larger_image(self):
        """Check that a 13-chunk upload survives 30 % radio loss (Sec. 4.2, REQ-031). Passes if exit
        code is 0, the bytes are intact and some datagrams were dropped.
        """
        big = Path(tempfile.mkdtemp()) / "big.img"
        big.write_bytes(random.Random(1).randbytes(256 * 12 + 77))   # 13 chunks
        code, out = run_console(self.fake, "--radio-loss", "0.3", "--seed", "3",
                                "upload", str(big), "--number", "4")
        self.assertEqual(code, 0, out)
        self.assertEqual(bytes(self.fake.upload), big.read_bytes())
        self.assertRegex(out, r"dropped [1-9]\d* of")

    def test_refusal_printed_with_text(self):
        """Check that a REFUSED answer is printed with its code and reason text (Sec. 4.2). Passes if
        exit code is 1 and "Image not from the operator" appears for 0x09.
        """
        self.fake.refuse = {c.UPLOAD_END: 0x09}
        code, out = run_console(self.fake, "upload", str(self.image), "--number", "1")
        self.assertEqual(code, 1)
        self.assertIn("REFUSED 0x09: Image not from the operator", out)

    def test_refusal_0e_text(self):
        """Check the text of REFUSED 0x0E (Sec. 4.2). Passes if "Image built for the other slot" is
        printed.
        """
        self.fake.refuse = {c.DELIVER: 0x0E}
        code, out = run_console(self.fake, "deliver", "--number", "1")
        self.assertIn("REFUSED 0x0e: Image built for the other slot", out)

    def test_switch_arms_the_switch_sequence_number(self):
        """Check that `switch` sends an ARM naming SWITCH and SWITCH's own sequence number (Sec.
        5.2). Passes if exit code is 0, the fake was armed for SWITCH with the last sequence
        number seen and the controller is on slot B.
        """
        self.fake.delivered = True
        code, out = run_console(self.fake, "switch")
        self.assertEqual(code, 0, out)
        self.assertEqual(self.fake.armed[0], c.SWITCH)
        self.assertEqual(self.fake.armed[1], self.fake.arrivals[-1][1])   # ARM named SWITCH's own number
        self.assertEqual(self.fake.state["slot"], 2)

    def test_help_states_bench_limit(self):
        """Check that --help states the bench limit of no envelope encryption (Sec. 4.1 left out).
        Passes if the help text contains it.
        """
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertRaises(SystemExit):
            c.main(["--help"])
        self.assertIn("no envelope encryption", out.getvalue())

    def seqs_since(self, index):
        """Sequence numbers of the datagrams the fake received from position `index` on.

        Args:
            index: position in fake.arrivals to start from.

        Returns:
            List of sequence numbers.
        """
        return [a[1] for a in self.fake.arrivals[index:]]

    def test_two_runs_use_increasing_numbers(self):
        """Check that a second console run continues above the first (Sec. 4.0, REQ-065). Passes if
        every number of run 2 is higher than every number of run 1, the numbers run without a gap
        and the state file holds the last number.
        """
        self.assertEqual(run_console(self.fake, "status", "--seconds", "0.1")[0], 0)
        n1 = len(self.fake.arrivals)
        first_run = self.seqs_since(0)
        self.assertEqual(run_console(self.fake, "status", "--seconds", "0.1")[0], 0)
        second_run = self.seqs_since(n1)
        self.assertGreater(min(second_run), max(first_run))
        self.assertEqual(min(second_run), max(first_run) + 1)
        self.assertEqual(int(Path(SEQ_FILE).read_text()), max(second_run))
        self.assertEqual([p.name for p in Path(SEQ_FILE).parent.iterdir()], ["console-seq"])   # no temp file left

    def test_second_confirm_run_gets_new_numbers(self):
        """Check that `switch` run twice in one onboard session sends ARM and SWITCH with fresh
        numbers each time (Sec. 4.0, REQ-065). Passes if the four numbers of the two runs are all
        different and rise.
        """
        self.fake.delivered = True
        self.assertEqual(run_console(self.fake, "switch")[0], 0)
        self.assertEqual(run_console(self.fake, "switch")[0], 0)
        seqs = self.seqs_since(0)
        self.assertEqual(seqs, sorted(set(seqs)))
        self.assertGreaterEqual(len(seqs), 4)

    def test_damaged_state_file_starts_from_the_clock(self):
        """Check that an absent, empty, garbage, zero and too-large state file each make the run
        start from the clock, say so, and rewrite the file (Sec. 4.0). Passes if the first number is
        at least the clock time read before the run, and the note is printed.
        """
        state = Path(SEQ_FILE)
        state.parent.mkdir(parents=True)
        for damage in (None, b"", b"garbage\n", b"0\n", b"99999999999\n", b"\xff\xfe", b"-5\n"):
            if damage is None:
                state.unlink(missing_ok=True)
            else:
                state.write_bytes(damage)
            before = int(time.time())
            index = len(self.fake.arrivals)
            code, out = run_console(self.fake, "status", "--seconds", "0.1")
            self.assertEqual(code, 0, (damage, out))
            self.assertRegex(out, r"seq +.*(no sequence state file|unreadable).*starting from clock time")
            first = self.seqs_since(index)[0]
            self.assertGreaterEqual(first, before, damage)
            self.assertLessEqual(first, int(time.time()), damage)
            self.assertEqual(int(state.read_text()), self.seqs_since(index)[-1])

    def test_load_first_seq_reads_last_plus_one(self):
        """Check load_first_seq on a good file and on the largest value (Sec. 4.0). Passes if a file
        holding N gives N+1 with no note, and 2^32-1 in the file is treated as used up (clock).
        """
        state = self.tmp / "s"
        state.write_text("41\n")
        self.assertEqual(c.load_first_seq(str(state), now=1000), (42, None))
        state.write_text(f"{c.SEQ_MAX}\n")
        first, note = c.load_first_seq(str(state), now=1000)
        self.assertEqual(first, 1000)
        self.assertIsNotNone(note)

    def test_first_seq_overrides_the_state_file(self):
        """Check that --first-seq wins over the state file and the file then follows the run
        (Sec. 4.0). Passes if the first number is the one given and the file holds the last one.
        """
        Path(SEQ_FILE).parent.mkdir(parents=True)
        Path(SEQ_FILE).write_text("5000\n")
        code, out = run_console(self.fake, "--first-seq", "7", "status", "--seconds", "0.1")
        self.assertEqual(code, 0, out)
        seqs = self.seqs_since(0)
        self.assertEqual(seqs[0], 7)
        self.assertEqual(int(Path(SEQ_FILE).read_text()), max(seqs))

    def test_number_is_saved_before_the_frame_goes_out(self):
        """Check that the state file holds a number before its frame is sent (REQ-065), so a crash
        cannot reuse it. Passes if, when transmit() runs, the file already holds the frame's number.
        """
        seen = []
        original = c.Console.transmit

        def spy(con, frame):
            """Record the file content next to the frame's sequence number, then send."""
            seen.append((int(Path(SEQ_FILE).read_text()), c.parse(frame)["seq"]))
            original(con, frame)

        with mock.patch.object(c.Console, "transmit", spy):
            run_console(self.fake, "--first-seq", "10", "status", "--seconds", "0.1")
        self.assertTrue(seen)
        for saved, seq in seen:
            self.assertEqual(saved, seq)

    def test_unwritable_state_file_stops_the_run(self):
        """Check that a state file that cannot be written stops the run before any frame is sent
        (REQ-065: an unsaved number could be reused). Passes if exit code is 1, FAILED names the
        file and the fake saw nothing.
        """
        blocker = self.tmp / "blocker"
        blocker.write_text("a file, not a directory")
        code, out = run_console(self.fake, "--seq-file", str(blocker / "console-seq"), "status")
        self.assertEqual(code, 1)
        self.assertIn("FAILED: cannot save the sequence number", out)
        self.assertEqual(self.fake.arrivals, [])

    def test_resume_hint_names_the_failed_stage(self):
        """Check the resume advice after a refusal (Sec. 5.1). Passes if a refused BEGIN says nothing
        is stored and gives no --offset, a refused END says all bytes were accepted and gives
        --offset = image size, and a refused CHUNK names its own offset.
        """
        image = str(self.image)
        size = self.image.stat().st_size
        self.fake.refuse = {c.UPLOAD_BEGIN: 0x06}
        code, out = run_console(self.fake, "upload", image, "--number", "1")
        self.assertEqual(code, 1)
        self.assertIn("nothing is stored", out)
        self.assertNotIn("--offset", out.split("nothing is stored")[1].split("(no --offset)")[0])
        self.assertNotIn("has bytes up to", out)
        self.fake.refuse = {c.UPLOAD_END: 0x08}
        code, out = run_console(self.fake, "upload", image, "--number", "1")
        self.assertIn(f"all {size} bytes were accepted", out)
        self.assertIn(f"--offset {size}`", out)
        self.fake.refuse = {c.UPLOAD_CHUNK: 0x06}
        code, out = run_console(self.fake, "upload", image, "--number", "1")
        self.assertIn("UPLOAD CHUNK offset 0 (seq", out)
        self.assertIn("bytes before offset 0 were accepted", out)
        self.assertIn("`upload IMAGE --number 1 --offset 0`", out)

    def test_health_decoding(self):
        """Check that a HEALTH payload decodes, with and without the added fields (Sec. 5.4). Passes
        if mode, version, slot, trial time left, report number and onboard time match.
        """
        body = c.HEALTH_BODY.pack(5, 1, 2, 1, 2, 2, 1, 42, 0)
        r = c.decode_report(body)
        self.assertEqual((r["mode"], r["version"], r["slot"], r["trial_left"]), ("TRIAL", 2, "B", 42))
        r = c.decode_report(c.ADDED.pack(9, 8) + body)
        self.assertEqual((r["report_number"], r["onboard_time"]), (9, 8))

    def event_text(self, code, detail=b""):
        """Decode an EVENT report payload and return its text.

        Args:
            code: 16-bit event code.
            detail: detail bytes after the code.

        Returns:
            The decoded text of the event (Sec. 5.4).
        """
        body = c.struct.pack("<IBH", 5, c.REPORT_EVENT, code) + detail
        return c.decode_report(c.ADDED.pack(9, 8) + body)["text"]

    def test_new_refusal_names(self):
        """Check the refusal reasons added from Sec. 4.2 (0x10, 0x11) and the bench codes of
        protocol.hpp (0xFD, 0xFE, 0xFF). Passes if each text matches and the bench ones say "bench".
        """
        self.assertEqual(c.refusal_text(0x10), "Image of the wrong kind for its place")
        self.assertEqual(c.refusal_text(0x11), "Boot state could not be stored")
        self.assertEqual(c.refusal_text(0xFD), "bench: the onboard computer could not send on data link A")
        self.assertEqual(c.refusal_text(0xFE), "bench: the onboard computer's own storage failed")
        self.assertEqual(c.refusal_text(0xFF), "bench: the controller did not answer within its timeout")

    def test_refusal_printed_for_new_and_unknown_codes(self):
        """Check that a REFUSED answer prints 0x10 and 0xFF in words and an unlisted code as
        "unknown code" (Sec. 4.2). Passes if the printed lines match.
        """
        for code, text in ((0x10, "Image of the wrong kind for its place"),
                           (0xFF, "bench: the controller did not answer within its timeout"),
                           (0x42, "unknown code 0x42")):
            self.fake.refuse = {c.DELIVER: code}
            _, out = run_console(self.fake, "deliver", "--number", "1")
            self.assertIn(f"REFUSED 0x{code:02x}: {text}", out)

    def test_fallback_event_decoding(self):
        """Check EVENT 0x0002 and 0x0003 detail decoding (Sec. 5.4, "Check result" table). Passes
        if the refused copy and the check result come out in words for every listed value.
        """
        self.assertEqual(self.event_text(0x0002, b"\x02\x09"),
                         "fall-back to the previous copy: slot B refused (signature A)")
        self.assertEqual(self.event_text(0x0003, b"\x01\x0a"),
                         "fall-back to the last-resort firmware: slot A refused (signature B)")
        self.assertEqual(self.event_text(0x0002, b"\x03\x00"),
                         "fall-back to the previous copy: last-resort refused (passed)")
        for result, text in c.CHECK_RESULT.items():
            self.assertTrue(self.event_text(0x0002, bytes([1, result])).endswith(f"({text})"))
        self.assertEqual(len(c.CHECK_RESULT), 14)
        self.assertEqual(c.CHECK_RESULT[0x07], "start address outside the region")

    def test_new_check_results(self):
        """Check the check results 0x0B, 0x0C and 0x0D in events 0x0002 and 0x0003 (owner's
        decision 4, 13 Oct 2026). Passes if each reads in words and 0x0E, in no table, reads as
        "unknown code".
        """
        for result, text in ((0x0B, "flash read error"), (0x0C, "trial started too often"),
                             (0x0D, "restarted too often")):
            self.assertEqual(self.event_text(0x0002, bytes([1, result])),
                             f"fall-back to the previous copy: slot A refused ({text})")
            self.assertEqual(self.event_text(0x0003, bytes([2, result])),
                             f"fall-back to the last-resort firmware: slot B refused ({text})")
        self.assertEqual(self.event_text(0x0002, b"\x01\x0e"),
                         "fall-back to the previous copy: slot A refused (unknown code 0x0e)")

    def test_fault_detected_event(self):
        """Check EVENT 0x0001 and its fault kind (owner's decision 2). Passes if 0x01 reads "core
        clock failed, running at 16 MHz", a kind in no table is "unknown code", and a missing
        detail is said so.
        """
        base = "fault detected"
        self.assertEqual(self.event_text(0x0001, b"\x01"),
                         base + ": core clock failed, running at 16 MHz")
        self.assertEqual(self.event_text(0x0001, b"\x02"), base + ": unknown code 0x02")
        self.assertEqual(self.event_text(0x0001, b"\x00"), base + ": unknown code 0x00")
        self.assertEqual(self.event_text(0x0001), base + ": fault kind not given")

    def test_memory_change_event(self):
        """Check EVENT 0x0005 and its fault location (owner's decision 3). Passes if 0x27 reads as
        in the brief's example, the first and last code of each range read in words, a location in
        no table is "unknown code", and a missing detail is said so, also through format_report().
        """
        base = "change in memory detected"
        self.assertEqual(
            self.event_text(0x0005, b"\x27"),
            base + " at 0x27 (update receiver: image check result outside its list)")
        for where in (0x01, 0x07, 0x10, 0x11, 0x20, 0x2F):
            text = self.event_text(0x0005, bytes([where]))
            self.assertTrue(text.startswith(f"{base} at 0x{where:02x} ("), text)
        for where in (0x00, 0x08, 0x0F, 0x12, 0x1F, 0x30, 0x4F, 0xFF):
            self.assertEqual(self.event_text(0x0005, bytes([where])),
                             f"{base} at unknown code 0x{where:02x}")
        self.assertEqual(self.event_text(0x0005), base + ": location not given")
        r = c.decode_report(c.struct.pack("<IBH", 5, c.REPORT_EVENT, 0x0005) + b"\x99")
        self.assertIn("0x0005 change in memory detected at unknown code 0x99", c.format_report(r))


    def test_fallback_event_unknown_and_short_detail(self):
        """Check that an unlisted refused copy or check result, and a detail of the wrong length,
        show as unknown or as a length note (Sec. 5.4). Passes if no value is printed bare.
        """
        self.assertEqual(self.event_text(0x0002, b"\x07\x09"),
                         "fall-back to the previous copy: unknown code 0x07 refused (signature A)")
        self.assertEqual(self.event_text(0x0003, b"\x01\x7e"),
                         "fall-back to the last-resort firmware: slot A refused (unknown code 0x7e)")
        self.assertEqual(self.event_text(0x0002, b"\x01"),
                         "fall-back to the previous copy: detail of 1 bytes, expected 2")
        self.assertEqual(self.event_text(0x0002),
                         "fall-back to the previous copy: detail of 0 bytes, expected 2")

    def test_restart_by_onboard_event(self):
        """Check EVENT 0x0106 and its reason byte (Sec. 5.4). Passes if 0x01 reads "trial past its
        time", an unlisted reason is unknown, and a missing reason is said so.
        """
        base = "controller restarted by the onboard computer"
        self.assertEqual(self.event_text(0x0106, b"\x01"), base + ": trial past its time")
        self.assertEqual(self.event_text(0x0106, b"\x02"), base + ": unknown code 0x02")
        self.assertEqual(self.event_text(0x0106), base + ": reason not given")

    def test_unknown_event_and_unknown_test(self):
        """Check that an event code or failed-test value outside the tables prints as "unknown
        code" with the number (Sec. 5.4). Passes if the texts match, also through format_report().
        """
        self.assertEqual(self.event_text(0x0999), "unknown code 0x0999")
        self.assertEqual(self.event_text(0x000B, b"\x09"), "trial tests failed: unknown code 0x09")
        r = c.decode_report(c.struct.pack("<IBH", 5, c.REPORT_EVENT, 0x0999))
        self.assertIn("0x0999 unknown code 0x0999", c.format_report(r))

    def test_health_unknown_values(self):
        """Check that an unlisted mode, slot or self-test byte in HEALTH prints as "unknown code"
        (Sec. 5.4). Passes if the three texts match.
        """
        r = c.decode_report(c.HEALTH_BODY.pack(5, 1, 9, 1, 2, 8, 1, 42, 7))
        self.assertEqual((r["mode"], r["slot"], r["self_test"]),
                         ("unknown code 0x09", "unknown code 0x08", "unknown code 0x07"))



class FaultCodesHeaderTests(unittest.TestCase):
    """The console's fault tables against src/controller/common/fault_codes.h."""

    @staticmethod
    def header_codes(prefix):
        """Read the codes of one family from the controller's fault_codes.h.

        The regex runs over the #define lines only, so a code added to the header shows up here
        without any change to the test.

        Args:
            prefix: the macro name prefix, "FAULT_KIND_" or "FAULT_AT_".

        Returns:
            dict from the number (int) to the macro name without the prefix.
        """
        found = {}
        pattern = re.compile(rf"^#define\s+{prefix}(\w+)\s+0x([0-9A-Fa-f]+)U\b", re.MULTILINE)
        for name, number in pattern.findall(FAULT_CODES_H.read_text()):
            found[int(number, 16)] = name
        return found

    def test_fault_kind_table_matches_header(self):
        """Check FAULT_KIND against the FAULT_KIND_* lines of fault_codes.h. Passes if the same
        numbers are in both, each with the header's name; a new code in the header fails it until
        the console's table has it.
        """
        header = self.header_codes("FAULT_KIND_")
        self.assertGreaterEqual(len(header), 1)
        self.assertEqual({k: v[0] for k, v in c.FAULT_KIND.items()}, header)

    def test_fault_location_table_matches_header(self):
        """Check FAULT_LOCATION against the FAULT_AT_* lines of fault_codes.h. Passes if the same
        numbers are in both, each with the header's name; a new code in the header fails it until
        the console's table has it.
        """
        header = self.header_codes("FAULT_AT_")
        self.assertGreaterEqual(len(header), 1)
        self.assertEqual({k: v[0] for k, v in c.FAULT_LOCATION.items()}, header)

    def test_fault_location_codes_in_the_header_ranges(self):
        """Check that every location code lies in a range of the header and has words. Passes if
        each code is inside FAULT_RANGE_BOOTLOADER, _COMMON or _FIRMWARE and its text is not empty.
        """
        text = FAULT_CODES_H.read_text()
        bounds = {name: int(num, 16) for name, num in
                  re.findall(r"^#define\s+FAULT_RANGE_(\w+)\s+0x([0-9A-Fa-f]+)U\b", text, re.M)}
        spans = [(bounds[f"{part}_FIRST"], bounds[f"{part}_LAST"])
                 for part in ("BOOTLOADER", "COMMON", "FIRMWARE")]
        for code, (_, words) in c.FAULT_LOCATION.items():
            self.assertTrue(any(lo <= code <= hi for lo, hi in spans), hex(code))
            self.assertTrue(words)


if __name__ == "__main__":
    unittest.main(verbosity=2)
