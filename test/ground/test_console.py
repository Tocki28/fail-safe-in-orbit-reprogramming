#!/usr/bin/env python3
"""Ground console tests against a fake onboard computer on localhost (no board, no network)."""
import contextlib
import io
import random
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / "src" / "ground"))
import console as c  # noqa: E402
from fake_onboard import FakeOnboard  # noqa: E402

IMAGES = HERE.parents[1] / "src" / "controller" / "build" / "images"
FAST_S = "0.3"           # command timeout in tests; real value is 3 s


def run_console(fake, *args):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = c.main(["--host", "127.0.0.1", "--port", str(fake.port), "--timeout", FAST_S, *args])
    return code, out.getvalue()


class ConsoleTests(unittest.TestCase):
    def setUp(self):
        self.fake = FakeOnboard()
        self.addCleanup(self.fake.close)
        self.image = IMAGES / "firmware-B.img"
        self.assertTrue(self.image.exists(), "run `make -C src/controller` first")

    def update_args(self):
        return ["update", "--image-dir", str(IMAGES)]

    def test_frame_roundtrip_and_bad_crc(self):
        f = c.build(c.TYPE_COMMAND, 2, 7, b"\x25", 99)
        self.assertEqual(c.parse(f)["seq"], 7)
        self.assertIsNone(c.parse(f[:-1] + bytes([f[-1] ^ 1])))

    def test_upload_bytes_equal_image_and_stop_and_wait(self):
        code, out = run_console(self.fake, "upload", str(self.image), "--number", "1")
        self.assertEqual(code, 0, out)
        self.assertEqual(bytes(self.fake.upload), self.image.read_bytes())
        self.assertTrue(self.fake.ended)
        self.assertTrue(self.fake.stop_and_wait_held())

    def test_update_confirmed(self):
        code, out = run_console(self.fake, *self.update_args())
        self.assertEqual(code, 0, out)
        self.assertIn("UPDATE CONFIRMED: slot B version 2", out)
        self.assertEqual(bytes(self.fake.upload), self.image.read_bytes())  # free slot was B
        self.assertTrue(self.fake.stop_and_wait_held())
        self.assertIn((c.DEST_CONTROLLER, c.ARM), self.fake.log)
        self.assertEqual(self.fake.log[-1], (c.DEST_CONTROLLER, c.SWITCH))

    def test_update_rolled_back(self):
        self.fake.rollback = True
        code, out = run_console(self.fake, *self.update_args())
        self.assertEqual(code, 1, out)
        self.assertIn("UPDATE ROLLED BACK: running slot A version 1", out)

    def test_update_with_radio_loss(self):
        code, out = run_console(self.fake, "--radio-loss", "0.3", "--seed", "5", *self.update_args())
        self.assertEqual(code, 0, out)
        self.assertIn("UPDATE CONFIRMED: slot B version 2", out)
        self.assertIn("radio simulator: dropped", out)
        self.assertEqual(bytes(self.fake.upload), self.image.read_bytes())
        self.assertTrue(self.fake.stop_and_wait_held())

    def test_upload_with_radio_loss_larger_image(self):
        big = Path(tempfile.mkdtemp()) / "big.img"
        big.write_bytes(random.Random(1).randbytes(256 * 12 + 77))   # 13 chunks
        code, out = run_console(self.fake, "--radio-loss", "0.3", "--seed", "3",
                                "upload", str(big), "--number", "4")
        self.assertEqual(code, 0, out)
        self.assertEqual(bytes(self.fake.upload), big.read_bytes())
        self.assertRegex(out, r"dropped [1-9]\d* of")

    def test_refusal_printed_with_text(self):
        self.fake.refuse = {c.UPLOAD_END: 0x09}
        code, out = run_console(self.fake, "upload", str(self.image), "--number", "1")
        self.assertEqual(code, 1)
        self.assertIn("REFUSED 0x09: Image not from the operator", out)

    def test_refusal_0e_text(self):
        self.fake.refuse = {c.DELIVER: 0x0E}
        code, out = run_console(self.fake, "deliver", "--number", "1")
        self.assertIn("REFUSED 0x0e: Image built for the other slot", out)

    def test_switch_arms_the_switch_sequence_number(self):
        self.fake.delivered = True
        code, out = run_console(self.fake, "switch")
        self.assertEqual(code, 0, out)
        self.assertEqual(self.fake.armed[0], c.SWITCH)
        self.assertEqual(self.fake.armed[1], self.fake.arrivals[-1][1])   # ARM named SWITCH's own number
        self.assertEqual(self.fake.state["slot"], 2)

    def test_help_states_bench_limit(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertRaises(SystemExit):
            c.main(["--help"])
        self.assertIn("no envelope encryption", out.getvalue())

    def test_health_decoding(self):
        body = c.HEALTH_BODY.pack(5, 1, 2, 1, 2, 2, 1, 42, 0)
        r = c.decode_report(body)
        self.assertEqual((r["mode"], r["version"], r["slot"], r["trial_left"]), ("TRIAL", 2, "B", 42))
        r = c.decode_report(c.ADDED.pack(9, 8) + body)
        self.assertEqual((r["report_number"], r["onboard_time"]), (9, 8))


if __name__ == "__main__":
    unittest.main(verbosity=2)
