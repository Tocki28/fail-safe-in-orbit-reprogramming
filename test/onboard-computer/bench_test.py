#!/usr/bin/env python3
"""Bench test of fsir-updater on this Mac, with a fake controller and a fake ground.

    fake ground  --UDP-->  fsir-updater  --pseudo-terminal-->  fake controller

Frames as in the Interfaces document, Sec. 4.0; answers Sec. 4.2; reports Sec. 5.4.

The fake controller (a thread) reads the updater's frames from the pseudo-terminal and
answers each command RECEIVED, then, after a short delay, DONE (Sec. 4.2); UPLOAD BEGIN takes
longer, as the real one erases its slot. It sends a HEALTH report every second, in the
controller's layout (src/controller/firmware/health-monitor/health_monitor.h at commit
5c0b5f8: 24 bytes, the
report number and onboard time sent as 0). It keeps every frame it receives and the image
bytes it is sent. Just before each last answer it looks whether any byte has arrived from the
updater: if so, the updater sent before the answer, and stop-and-wait is broken.

The fake ground sends commands over UDP, one frame per datagram, and keeps every frame the
updater sends back.

Checks (PASS or FAIL each; exit status 1 if any fails):
  upload of firmware-B.img to the onboard computer, every command RECEIVED then DONE;
  resume after a second BEGIN (REQ-049); a damaged image refused 0x08 at END, one signed
  with a wrong key refused 0x09;
  DELIVER: RECEIVED, DONE; the fake controller got exactly the image bytes; stop-and-wait
  held; the data-link sequence numbers go up by one;
  SWITCH (dangerous): the ground's ARM held; the controller sees ARM then SWITCH back to back
  with the onboard computer's numbers, the ground gets its own number back; SWITCH again
  REFUSED 0x01; three commands back to back: answers in order;
  HEALTH reports reach the ground numbered 1, 2, 3, ... with the onboard time filled in;
  every frame to the ground: Destination 0x03, Expiry 0, sequence numbers up by one;
  DELIVER refused 0x07 while the controller reports TRIAL (REQ-050);
  DELIVER refused with the controller's own code when it refuses a chunk;
  no answer from the controller: nothing made up, and the link works again after the timeout;
  a damaged datagram: no answer; an unknown command: REFUSED 0x06 only;
  the controller silent for 6 s: "controller silent" logged (REQ-061), DELIVER refused 0x05,
  then "controller heard again";
  SIGTERM: the updater stops with exit status 0.

Usage: bench_test.py UPDATER FIRMWARE_B FLIPPED_IMAGE WRONG_KEY_IMAGE WORKDIR
Standard library only.
"""
import os
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import tty
import zlib

SYNC = b"\xa5\x5a"
VERSION = 0x01
COMMAND, ANSWER, REPORT = 0x01, 0x02, 0x03
ONBOARD, CONTROLLER, GROUND = 0x01, 0x02, 0x03
HEADER = struct.Struct("<2sBBBBHII")   # Sync, Version, Type, Destination, Flags, Length, Sequence, Expiry
ANSWER_FIELDS = struct.Struct("<IBB")  # sequence answered, kind, code
RECEIVED, DONE, REFUSED, ALREADY_DONE = 0x01, 0x02, 0x03, 0x04
FINAL = (DONE, REFUSED, ALREADY_DONE)
KIND = {RECEIVED: "RECEIVED", DONE: "DONE", REFUSED: "REFUSED", ALREADY_DONE: "ALREADY DONE"}

UPLOAD_BEGIN, UPLOAD_CHUNK, UPLOAD_END, UPLOAD_CANCEL, DELIVER = 0x01, 0x02, 0x03, 0x04, 0x05
REPLACE_KEY, ARM, SWITCH, GET_STATUS = 0x0C, 0x0D, 0x20, 0x25
CHUNK = 256
EXPIRY = 0xFFFFFFFF

# HEALTH, fields in the order of the Sec. 5.4 table: report number, onboard computer clock time,
# controller clock time, report kind, mode, firmware kind, firmware version, running slot,
# failed starts, trial time left, last self-test (as test/controller/link_probe.py).
HEALTH = struct.Struct("<IIIBBBIBBHB")
OPERATE, TRIAL = 0x01, 0x02


def build(ftype, dest, seq, payload, expiry=0):
    head = HEADER.pack(SYNC, VERSION, ftype, dest, 0, len(payload), seq, expiry) + payload
    return head + struct.pack("<I", zlib.crc32(head))


def decode(raw):
    """One whole frame (a datagram) as a dict, or None."""
    if len(raw) < HEADER.size + 4:
        return None
    sync, version, ftype, dest, flags, length, seq, expiry = HEADER.unpack_from(raw)
    if sync != SYNC or version != VERSION or HEADER.size + length + 4 != len(raw):
        return None
    if struct.unpack_from("<I", raw, len(raw) - 4)[0] != zlib.crc32(raw[:-4]):
        return None
    return dict(type=ftype, dest=dest, flags=flags, seq=seq, expiry=expiry,
                payload=bytes(raw[HEADER.size:-4]), t=time.monotonic())


def split(buf):
    """Takes whole frames off the front of buf (a bytearray); returns them."""
    frames = []
    while True:
        start = buf.find(SYNC)
        if start < 0:
            del buf[:max(0, len(buf) - 1)]
            return frames
        del buf[:start]
        if len(buf) < HEADER.size:
            return frames
        length = HEADER.unpack_from(buf)[5]
        total = HEADER.size + length + 4
        if length > 1024:
            del buf[:1]
            continue
        if len(buf) < total:
            return frames
        frame = decode(bytes(buf[:total]))
        del buf[:total]
        if frame is not None:
            frames.append(frame)


class FakeController(threading.Thread):
    def __init__(self, fd):
        super().__init__(daemon=True)
        self.fd = fd
        self.buf = bytearray()
        self.lock = threading.Lock()
        self.stop = False
        self.seq = 0
        self.mode = OPERATE
        self.health_on = True
        self.ignore_next = False
        self.refuse = {}           # command id -> refusal code, after RECEIVED
        self.frames = []           # every frame received
        self.violations = []       # stop-and-wait broken
        self.errors = []           # anything else unexpected
        self.images = {}           # image number -> bytes received
        self.begin_length = {}
        self.ended = []
        self.health_sent = 0
        self.next_health = time.monotonic() + 0.5

    def write(self, ftype, payload):
        self.seq += 1
        os.write(self.fd, build(ftype, ONBOARD, self.seq, payload))

    def answer(self, seq, kind, code=0):
        self.write(ANSWER, ANSWER_FIELDS.pack(seq, kind, code))

    def read_waiting(self):
        while select.select([self.fd], [], [], 0)[0]:
            try:
                data = os.read(self.fd, 4096)
            except OSError:
                return
            if not data:
                return
            self.buf += data

    def send_health(self):
        payload = HEALTH.pack(0, 0, int(time.monotonic()), 0x01, self.mode, 0x02, 1, 0x01, 0,
                              0 if self.mode != TRIAL else 300, 0x00)
        self.write(REPORT, payload)
        self.health_sent += 1

    def run(self):
        while not self.stop:
            wait = max(0.0, self.next_health - time.monotonic())
            if select.select([self.fd], [], [], min(wait, 0.05))[0]:
                self.read_waiting()
                for frame in split(self.buf):
                    self.handle(frame)
            if time.monotonic() >= self.next_health:
                self.next_health += 1.0
                if self.health_on:
                    self.send_health()

    def handle(self, f):
        with self.lock:
            self.frames.append(f)
        if f["type"] != COMMAND or f["dest"] != CONTROLLER or not f["payload"]:
            self.errors.append(f"not a command for the controller: {f}")
            return
        if self.ignore_next:
            self.ignore_next = False
            return
        seq, p = f["seq"], f["payload"]
        cid = p[0]
        known = (UPLOAD_BEGIN, UPLOAD_CHUNK, UPLOAD_END, UPLOAD_CANCEL, SWITCH, GET_STATUS, 0x0D)
        if cid not in known:
            self.finish(seq, REFUSED, 0x06, received=False)
            return
        self.answer(seq, RECEIVED)
        if cid in self.refuse:
            time.sleep(0.01)
            self.finish(seq, REFUSED, self.refuse[cid])
            return
        if cid == UPLOAD_BEGIN:
            number, length = struct.unpack_from("<HI", p, 1)
            self.images[number] = bytearray()
            self.begin_length[number] = length
            time.sleep(0.2)   # the real controller erases 20 pages here
        elif cid == UPLOAD_CHUNK:
            number, offset = struct.unpack_from("<HI", p, 1)
            data = p[7:]
            image = self.images.setdefault(number, bytearray())
            if offset != len(image):
                self.errors.append(f"chunk at {offset}, expected {len(image)}")
            image += data
            time.sleep(0.01)
        elif cid == UPLOAD_END:
            self.ended.append(struct.unpack_from("<H", p, 1)[0])
            time.sleep(0.02)
        else:
            time.sleep(0.01)
        self.finish(seq, DONE, 0)

    def finish(self, seq, kind, code, received=True):
        # Any byte from the updater before this last answer breaks stop-and-wait.
        self.read_waiting()
        if self.buf:
            self.violations.append(f"bytes arrived before the last answer to {seq}")
        self.answer(seq, kind, code)


class Ground:
    def __init__(self, port):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.1)
        self.addr = ("127.0.0.1", port)
        self.seq = 0
        self.frames = []
        self.cond = threading.Condition()
        self.stop = False
        threading.Thread(target=self.receive, daemon=True).start()

    def receive(self):
        while not self.stop:
            try:
                data = self.sock.recv(4096)
            except socket.timeout:
                continue
            frame = decode(data)
            with self.cond:
                self.frames.append(frame if frame is not None else dict(bad=data))
                self.cond.notify_all()

    def send(self, dest, payload, damage=False):
        self.seq += 1
        raw = bytearray(build(COMMAND, dest, self.seq, payload, EXPIRY))
        if damage:
            raw[-1] ^= 0x01
        self.sock.sendto(bytes(raw), self.addr)
        return self.seq

    def answers(self, seq):
        with self.cond:
            out = []
            for f in self.frames:
                if f.get("type") == ANSWER and len(f["payload"]) == 6:
                    answered, kind, code = ANSWER_FIELDS.unpack(f["payload"])
                    if answered == seq:
                        out.append((kind, code))
            return out

    def wait_final(self, seq, timeout):
        end = time.monotonic() + timeout
        with self.cond:
            while time.monotonic() < end:
                if any(k in FINAL for k, _ in self.answers(seq)):
                    break
                self.cond.wait(0.05)
        return self.answers(seq)

    def command(self, dest, payload, timeout=5.0):
        seq = self.send(dest, payload)
        return seq, self.wait_final(seq, timeout)

    def reports(self):
        with self.cond:
            return [f for f in self.frames if f.get("type") == REPORT]


class Checks:
    def __init__(self):
        self.failed = 0

    def report(self, ok, name):
        self.failed += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}", flush=True)


def show(answers):
    return ", ".join(KIND.get(k, hex(k)) + (f" 0x{c:02x}" if k == REFUSED else "")
                     for k, c in answers)


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class UpdaterLog:
    def __init__(self, stream, path):
        self.lines = []
        self.lock = threading.Lock()
        self.file = open(path, "w")
        threading.Thread(target=self.read, args=(stream,), daemon=True).start()

    def read(self, stream):
        for line in stream:
            with self.lock:
                self.lines.append(line.rstrip("\n"))
            self.file.write(line)
            self.file.flush()

    def has(self, text):
        with self.lock:
            return any(text in line for line in self.lines)

    def wait_for(self, text, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.has(text):
                return True
            time.sleep(0.05)
        return self.has(text)


def upload(ground, number, image, first=0, last=None):
    """UPLOAD CHUNKs of image from offset first to last; returns every answer list."""
    last = len(image) if last is None else last
    results = []
    for offset in range(first, last, CHUNK):
        data = image[offset:offset + CHUNK]
        _, a = ground.command(ONBOARD, struct.pack("<BHI", UPLOAD_CHUNK, number, offset) + data)
        results.append(a)
    return results


class Bench:
    """One fsir-updater between a fake controller on a pseudo-terminal and a fake ground."""

    def __init__(self, updater, work, name, extra=()):
        self.master, self.slave = os.openpty()
        tty.setraw(self.slave)
        self.port = free_udp_port()
        self.data_dir = os.path.join(work, name + "-images")
        self.log_path = os.path.join(work, name + ".log")
        self.proc = subprocess.Popen([updater, "--device", os.ttyname(self.slave), "--udp-port",
                                      str(self.port), "--data-dir", self.data_dir, *extra],
                                     stderr=subprocess.PIPE, text=True)
        self.log = UpdaterLog(self.proc.stderr, self.log_path)
        self.fake = FakeController(self.master)
        self.ground = None

    def start(self, checks):
        checks.report(self.log.wait_for("open, 115200", 3),
                      "updater started; data link A open on the pseudo-terminal")
        self.fake.start()
        self.ground = Ground(self.port)

    def stop(self, checks):
        self.proc.send_signal(signal.SIGTERM)
        try:
            code = self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            code = None
        self.fake.stop = True
        if self.fake.is_alive():
            self.fake.join(1)
        if self.ground is not None:
            self.ground.stop = True
        checks.report(code == 0 and self.log.wait_for("stopped", 1),
                      f"SIGTERM: stopped, exit status {code}")
        os.close(self.master)
        os.close(self.slave)
        print(f"updater log: {self.log_path}")


def beat_phase(updater, work, checks):
    """--beat-s 1: GET STATUS to the controller when the link is idle; its answers stay on
    board."""
    bench = Bench(updater, work, "beat", ("--beat-s", "1"))
    try:
        bench.start(checks)
        seq, a = bench.ground.command(ONBOARD, bytes([GET_STATUS]))
        time.sleep(2.5)
        beats = [f for f in bench.fake.frames if f["payload"] == bytes([GET_STATUS])]
        checks.report(len(beats) >= 2 and [f["seq"] for f in beats] ==
                      list(range(beats[0]["seq"], beats[0]["seq"] + len(beats))),
                      f"--beat-s 1: {len(beats)} GET STATUS to the controller in 2.5 s")
        with bench.ground.cond:
            answered = {ANSWER_FIELDS.unpack(f["payload"])[0] for f in bench.ground.frames
                        if f.get("type") == ANSWER}
        checks.report(answered == {seq}, "the beat's answers are not sent to the ground")
    finally:
        bench.stop(checks)


def sigpipe_phase(updater, work, checks):
    """Audit finding #8: a log reader that goes away (closed pipe on stderr) must not kill the
    updater with SIGPIPE; it keeps serving the ground."""
    port = free_udp_port()
    proc = subprocess.Popen([updater, "--device", os.path.join(work, "no-such-device"),
                             "--udp-port", str(port), "--data-dir", os.path.join(work, "sigpipe-images")],
                            stderr=subprocess.PIPE, text=True)
    time.sleep(0.5)
    proc.stderr.close()   # the reader is gone: the next log line writes to a closed pipe
    ground = Ground(port)
    try:
        _, a = ground.command(ONBOARD, bytes([GET_STATUS]))
        alive = proc.poll() is None
        checks.report(alive and a == [(RECEIVED, 0), (DONE, 0)],
                      f"stderr pipe closed: the updater is still alive and answers GET STATUS ({show(a)}, exit {proc.poll()})")
    finally:
        ground.stop = True
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()


def main():
    if len(sys.argv) != 6:
        print(__doc__)
        return 2
    updater, fw_b_path, flipped_path, wrong_key_path, work = sys.argv[1:]
    image = open(fw_b_path, "rb").read()
    flipped = open(flipped_path, "rb").read()
    wrong_key = open(wrong_key_path, "rb").read()
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(work)
    ok_rr = [(RECEIVED, 0), (DONE, 0)]
    checks = Checks()

    bench = Bench(updater, work, "updater")
    log, fake, data_dir = bench.log, bench.fake, bench.data_dir
    try:
        bench.start(checks)
        ground = bench.ground
        checks.report(log.wait_for("controller heard", 3), "the fake controller's HEALTH heard")

        # ---- The onboard computer's own commands -----------------------------------------
        seq, a = ground.command(ONBOARD, bytes([GET_STATUS]))
        checks.report(a == ok_rr, f"GET STATUS to the onboard computer: {show(a)}")
        seq, a = ground.command(ONBOARD, bytes([REPLACE_KEY]) + bytes(33))
        checks.report(a == [(REFUSED, 0x06)], f"REPLACE KEY (not on the bench): {show(a)} only")
        bad = ground.send(ONBOARD, bytes([GET_STATUS]), damage=True)
        time.sleep(0.5)
        checks.report(ground.answers(bad) == [], "a datagram with a bad check value: no answer")

        # ---- Upload of firmware-B.img as image 2 ---------------------------------------------
        _, a = ground.command(ONBOARD, struct.pack("<BHI", UPLOAD_BEGIN, 2, len(image)))
        checks.report(a == ok_rr, f"UPLOAD BEGIN image 2, {len(image)} bytes: {show(a)}")
        results = upload(ground, 2, image)
        checks.report(all(r == ok_rr for r in results),
                      f"UPLOAD CHUNK x{len(results)}: each RECEIVED, then DONE")
        _, a = ground.command(ONBOARD, struct.pack("<BH", UPLOAD_END, 2))
        checks.report(a == ok_rr, f"UPLOAD END image 2 (checks pass): {show(a)}")
        stored = os.path.join(data_dir, "image-00002.img")
        checks.report(os.path.exists(stored) and open(stored, "rb").read() == image,
                      "image 2 stored: the same bytes as firmware-B.img")

        # ---- Resume (REQ-049): image 3 ----------------------------------------------------
        ground.command(ONBOARD, struct.pack("<BHI", UPLOAD_BEGIN, 3, len(image)))
        upload(ground, 3, image, 0, 3 * CHUNK)
        _, a = ground.command(ONBOARD, struct.pack("<BHI", UPLOAD_BEGIN, 3, len(image)))
        checks.report(a == ok_rr and log.wait_for("resuming at byte 768", 1),
                      "second UPLOAD BEGIN image 3: resumes at byte 768")
        r0 = upload(ground, 3, image, 0, CHUNK)[0]
        r5 = upload(ground, 3, image, 5 * CHUNK, 6 * CHUNK)[0]
        checks.report(r0 == ok_rr and r5 == [(RECEIVED, 0), (REFUSED, 0x06)],
                      f"resent chunk at 0: {show(r0)}; chunk past the stored bytes: {show(r5)}")
        rest = upload(ground, 3, image, 3 * CHUNK)
        _, a = ground.command(ONBOARD, struct.pack("<BH", UPLOAD_END, 3))
        checks.report(all(r == ok_rr for r in rest) and a == ok_rr,
                      f"rest of image 3 from byte 768, then UPLOAD END: {show(a)}")

        # ---- A damaged image: image 4 ------------------------------------------------------
        ground.command(ONBOARD, struct.pack("<BHI", UPLOAD_BEGIN, 4, len(flipped)))
        upload(ground, 4, flipped)
        _, a = ground.command(ONBOARD, struct.pack("<BH", UPLOAD_END, 4))
        checks.report(a == [(RECEIVED, 0), (REFUSED, 0x08)],
                      f"damaged image (byte 0x600 flipped), UPLOAD END: {show(a)}")

        # ---- An image signed with a wrong key A: image 5 ---------------------------------------
        ground.command(ONBOARD, struct.pack("<BHI", UPLOAD_BEGIN, 5, len(wrong_key)))
        upload(ground, 5, wrong_key)
        _, a = ground.command(ONBOARD, struct.pack("<BH", UPLOAD_END, 5))
        checks.report(a == [(RECEIVED, 0), (REFUSED, 0x09)],
                      f"image signed with a wrong key A, UPLOAD END: {show(a)}")
        _, a = ground.command(ONBOARD, struct.pack("<BH", DELIVER, 5))
        checks.report(a == [(RECEIVED, 0), (REFUSED, 0x06)],
                      f"DELIVER of the refused image 5 (nothing stored): {show(a)}")

        # ---- DELIVER image 2 ----------------------------------------------------------------
        before = len(fake.frames)
        seq, a = ground.command(ONBOARD, struct.pack("<BH", DELIVER, 2), timeout=15)
        checks.report(a == ok_rr, f"DELIVER image 2: {show(a)}")
        checks.report(bytes(fake.images.get(2, b"")) == image and fake.begin_length.get(2) == len(image)
                      and 2 in fake.ended,
                      f"the controller received exactly the image: {len(fake.images.get(2, b''))} "
                      f"of {len(image)} bytes, BEGIN length {fake.begin_length.get(2)}, END")
        delivered = fake.frames[before:]
        ids = [f["payload"][0] for f in delivered]
        expected_ids = [UPLOAD_BEGIN] + [UPLOAD_CHUNK] * ((len(image) + CHUNK - 1) // CHUNK) + [UPLOAD_END]
        checks.report(ids == expected_ids, f"delivery: BEGIN, {len(expected_ids) - 2} CHUNKs, END")
        checks.report(all(f["expiry"] == EXPIRY for f in delivered),
                      "delivery frames carry the DELIVER command's expiry")
        checks.report(not fake.violations and not fake.errors,
                      f"stop-and-wait held during the delivery {fake.violations[:2]} {fake.errors[:2]}")

        # ---- Commands passed on to the controller --------------------------------------------
        # SWITCH is dangerous: the ground's ARM is held by the onboard computer and not passed
        # on; the SWITCH then goes out as a pair, an ARM naming the data-link sequence number
        # the SWITCH carries, then the SWITCH (Interfaces Sec. 5.2).
        before = len(fake.frames)
        arm_seq, a = ground.command(CONTROLLER, bytes([ARM, SWITCH]) + struct.pack("<I", ground.seq + 2))
        checks.report(a == ok_rr and len(fake.frames) == before,
                      f"ARM for SWITCH held, not passed on, answers to ground sequence {arm_seq}: {show(a)}")
        seq, a = ground.command(CONTROLLER, bytes([SWITCH]))
        pair = fake.frames[before:before + 2]
        arm_f, sw = (pair + [None, None])[:2]
        checks.report(a == ok_rr, f"SWITCH passed on, answers to ground sequence {seq}: {show(a)}")
        checks.report(len(pair) == 2 and arm_f["payload"] == bytes([ARM, SWITCH]) + struct.pack("<I", sw["seq"])
                      and sw["payload"] == bytes([SWITCH]) and sw["seq"] == arm_f["seq"] + 1
                      and sw["seq"] != seq and arm_f["seq"] == fake.frames[before - 1]["seq"] + 1,
                      f"the controller saw ARM then SWITCH, back to back, as data-link sequence "
                      f"{arm_f['seq'] if arm_f else None} and {sw['seq'] if sw else None}, the onboard "
                      f"computer's own, not {seq}; the ARM names the SWITCH's number")
        _, a = ground.command(CONTROLLER, bytes([SWITCH]))
        checks.report(a == [(REFUSED, 0x01)] and len(fake.frames) == before + 2,
                      f"SWITCH again, ARM used up: REFUSED 0x01, nothing sent: {show(a)}")

        seqs = [ground.send(CONTROLLER, bytes([GET_STATUS])) for _ in range(3)]
        for s in seqs:
            ground.wait_final(s, 5)
        order = []
        with ground.cond:
            for f in ground.frames:
                if f.get("type") == ANSWER:
                    answered, kind, _ = ANSWER_FIELDS.unpack(f["payload"])
                    if answered in seqs:
                        order.append((answered, kind))
        want = [(s, k) for s in seqs for k in (RECEIVED, DONE)]
        checks.report(order == want, f"three GET STATUS back to back: answers in order {order}")
        checks.report(not fake.violations, "stop-and-wait held for commands passed on")

        link_seqs = [f["seq"] for f in fake.frames]
        checks.report(link_seqs == list(range(1, len(link_seqs) + 1)),
                      f"data link A: sequence numbers 1 to {len(link_seqs)}, up by one")

        # ---- Reports ------------------------------------------------------------------------
        # One HEALTH a second; the ones before the ground first spoke had nowhere to go.
        end = time.monotonic() + 4
        while len(ground.reports()) < 3 and time.monotonic() < end:
            time.sleep(0.1)
        reports = ground.reports()
        numbers = [struct.unpack_from("<I", r["payload"])[0] for r in reports]
        healths = [HEALTH.unpack(r["payload"]) for r in reports if len(r["payload"]) == HEALTH.size]
        now = time.time()
        checks.report(len(reports) >= 3 and len(healths) == len(reports),
                      f"HEALTH reports reached the ground: {len(reports)}, all 24 bytes")
        checks.report(numbers == list(range(numbers[0], numbers[0] + len(numbers))) if numbers else False,
                      f"report numbers go up by one: {numbers[:3]} ... {numbers[-1:]}")
        checks.report(all(abs(h[1] - now) < 120 for h in healths) and
                      all(h[3] == 0x01 and h[7] == 0x01 and h[5] == 0x02 for h in healths),
                      "onboard time filled in (within 2 min of now); controller's fields unchanged")

        frames = list(ground.frames)
        good = [f for f in frames if "bad" not in f]
        gseqs = [f["seq"] for f in good]
        checks.report(len(good) == len(frames) and all(f["dest"] == GROUND and f["expiry"] == 0 for f in good),
                      f"all {len(frames)} frames to the ground: good check value, Destination 0x03, Expiry 0")
        checks.report(gseqs == list(range(gseqs[0], gseqs[0] + len(gseqs))),
                      "frames to the ground: sequence numbers up by one")

        # ---- DELIVER during a trial (REQ-050) ---------------------------------------------------
        fake.mode = TRIAL
        time.sleep(1.3)
        _, a = ground.command(ONBOARD, struct.pack("<BH", DELIVER, 2))
        checks.report(a == [(RECEIVED, 0), (REFUSED, 0x07)], f"DELIVER while the controller reports TRIAL: {show(a)}")
        fake.mode = OPERATE
        time.sleep(1.3)

        # ---- The controller refuses a chunk ------------------------------------------------------
        fake.refuse[UPLOAD_CHUNK] = 0x0E
        _, a = ground.command(ONBOARD, struct.pack("<BH", DELIVER, 2), timeout=10)
        checks.report(a == [(RECEIVED, 0), (REFUSED, 0x0E)],
                      f"DELIVER when the controller refuses a chunk 0x0E: {show(a)}")
        fake.refuse.clear()

        # ---- No answer from the controller ----------------------------------------------------------
        fake.ignore_next = True
        seq_lost = ground.send(CONTROLLER, bytes([GET_STATUS]))
        time.sleep(2.6)
        checks.report(ground.answers(seq_lost) == [] and log.has("no last answer"),
                      "the controller does not answer: nothing sent to the ground; timeout logged")
        _, a = ground.command(CONTROLLER, bytes([GET_STATUS]))
        checks.report(a == ok_rr, f"next command after the timeout: {show(a)}")

        # ---- The controller silent (REQ-061) ---------------------------------------------------------
        fake.health_on = False
        checks.report(log.wait_for("controller silent", 7.5), "controller silent for 5 s: logged")
        _, a = ground.command(ONBOARD, struct.pack("<BH", DELIVER, 2))
        checks.report(a == [(RECEIVED, 0), (REFUSED, 0x05)],
                      f"DELIVER with no HEALTH in the last 5 s: {show(a)}")
        fake.health_on = True
        checks.report(log.wait_for("controller heard again", 3), "controller heard again: logged")

        checks.report(not fake.violations and not fake.errors,
                      f"stop-and-wait held for the whole test {fake.violations[:2]} {fake.errors[:2]}")
    finally:
        bench.stop(checks)

    beat_phase(updater, work, checks)
    sigpipe_phase(updater, work, checks)
    print("bench test:", "all passed" if checks.failed == 0 else f"{checks.failed} FAILED")
    return 1 if checks.failed else 0


if __name__ == "__main__":
    sys.exit(main())
