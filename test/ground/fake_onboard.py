"""Fake onboard computer for the ground console tests: UDP on localhost.

Answers per Interfaces Sec. 4.2 (RECEIVED, then DONE or REFUSED; ALREADY DONE for a sequence
number seen before), takes the upload, relays ARM / SWITCH / GET STATUS as the controller
would, and sends HEALTH reports (Sec. 5.4, with the onboard computer's two added fields) to
the source of the most recent datagram. Starts as slot A version 1 OPERATE. After SWITCH it
plays the controller's trial (Sec. 5.4): EVENT 0x0006 trial started, HEALTH in TRIAL on slot B
version 2, then EVENT 0x000A trial tests passed, or 0x000B (detail: the test) with 0x0008 and a
return to slot A when `fail_test` is set. After 0x000A it waits for ARM + CONFIRM (REFUSED
0x0F before the tests passed); CONFIRM gives EVENT 0x0007 and OPERATE on slot B. With no
CONFIRM by the end of the trial time `trial_s` it gives the firmware up (0x0008) and returns
to slot A. `rollback` makes it give up at once with no test result. Test double only, never
part of src/.
"""
import queue
import socket
import struct
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "src" / "ground"))
import console as c  # noqa: E402

ANSWER_DELAY_S = 0.02    # RECEIVED -> DONE gap, so a command sent too early is visible
REPORT_PERIOD_S = 0.1    # HEALTH report rate of the fake
TRIAL_REPORTS = 3        # HEALTH reports in TRIAL before OPERATE


class FakeOnboard:
    def __init__(self, refuse=None, rollback=False, fail_test=None, trial_s=3.0):
        """Bind a UDP socket on localhost and start the receive, work and report threads.

        Args:
            refuse: dict of command id -> REFUSED code the fake gives for that command.
            rollback: True to give the new firmware up at once, with no test result.
            fail_test: detail byte of EVENT 0x000B (the failed test), or None for passing tests.
            trial_s: trial time of the fake, seconds.
        """
        self.refuse = refuse or {}          # command id -> REFUSED code
        self.rollback = rollback            # after SWITCH go back to slot A version 1, no test result
        self.fail_test = fail_test          # detail byte of EVENT 0x000B, or None: tests pass
        self.trial_s = trial_s              # trial time of the fake, seconds
        self.tests_passed = False
        self.confirmed = threading.Event()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self.source = None
        self.state = dict(mode=1, slot=1, version=1)
        self.trial_left = 0
        self.seen = {}                      # seq -> terminal kind sent
        self.arrivals = []                  # (time, seq, command id) of every datagram
        self.done_at = {}                   # seq -> time its DONE/REFUSED was sent
        self.upload = bytearray()
        self.upload_len = None
        self.ended = False
        self.delivered = False
        self.armed = None                   # (command id, seq)
        self.log = []
        self.report_no = 0
        self.stop = False
        self.q = queue.Queue()
        for fn in (self._recv, self._work, self._reports):
            threading.Thread(target=fn, daemon=True).start()

    def close(self):
        """Stop the threads and close the socket.
        """
        self.stop = True
        self.sock.close()

    def _send(self, ftype, payload, seq=0):
        """Send one frame to the source of the latest datagram, if there is one.

        Args:
            ftype: frame Type field.
            payload: payload bytes.
            seq: sequence number field; 0 for reports.
        """
        if self.source:
            try:
                self.sock.sendto(c.build(ftype, c.DEST_GROUND, seq, payload), self.source)
            except OSError:
                pass

    def _answer(self, seq, kind, code=0):
        """Send an answer (Sec. 4.2) to a command.

        Args:
            seq: sequence number answered.
            kind: RECEIVED, DONE, REFUSED or ALREADY_DONE.
            code: DONE or REFUSED code.
        """
        self._send(c.TYPE_ANSWER, c.ANSWER.pack(seq, kind, code))

    def _recv(self):
        """Receiver thread: parse datagrams, remember their source, queue the commands.
        """
        while not self.stop:
            try:
                data, addr = self.sock.recvfrom(4096)
            except OSError:
                return
            frame = c.parse(data)
            if frame is None or frame["type"] != c.TYPE_COMMAND:
                continue
            self.source = addr
            self.arrivals.append((time.monotonic(), frame["seq"], frame["payload"][0]))
            self.q.put(frame)

    def _work(self):
        """Worker thread: answer RECEIVED, run each new command, answer DONE or REFUSED (ALREADY DONE for a repeat).
        """
        while not self.stop:
            try:
                frame = self.q.get(timeout=0.1)
            except queue.Empty:
                continue
            seq, p = frame["seq"], frame["payload"]
            if seq in self.seen:
                self._answer(seq, c.ALREADY_DONE)
                continue
            self._answer(seq, c.RECEIVED)
            time.sleep(ANSWER_DELAY_S)
            code = self.refuse.get(p[0]) or self._run(frame["dest"], p, seq)
            self.seen[seq] = c.REFUSED if code else c.DONE
            self.done_at[seq] = time.monotonic()
            self._answer(seq, self.seen[seq], code or 0)

    def _run(self, dest, p, seq):
        """Returns a REFUSED code, or 0 for DONE."""
        cid = p[0]
        self.log.append((dest, cid))
        if cid == c.UPLOAD_BEGIN:
            _, self.upload_len = struct.unpack_from("<HI", p, 1)
            self.upload, self.ended = bytearray(), False
        elif cid == c.UPLOAD_CHUNK:
            _, off = struct.unpack_from("<HI", p, 1)
            if off != len(self.upload):
                return 0x06
            self.upload += p[7:]
        elif cid == c.UPLOAD_END:
            self.ended = len(self.upload) == self.upload_len
            return 0 if self.ended else 0x08
        elif cid == c.DELIVER:
            if not self.ended:
                return 0x06
            self.delivered = True
        elif cid == c.ARM:
            self.armed = (p[1], struct.unpack_from("<I", p, 2)[0])
        elif cid == c.SWITCH:
            if self.armed != (c.SWITCH, seq) or not self.delivered:
                return 0x01
            threading.Thread(target=self._switch, daemon=True).start()
        elif cid == c.CONFIRM:
            if self.state["mode"] != 2:
                return 0x05
            if self.armed != (c.CONFIRM, seq) or dest != c.DEST_CONTROLLER:
                return 0x01
            if not self.tests_passed:
                return 0x0F
            self.state = dict(mode=1, slot=2, version=2)
            self.trial_left = 0
            self.confirmed.set()
            self._event(c.EVENT_CONFIRMED)
        elif cid != c.GET_STATUS:
            return 0x06
        return 0

    def _event(self, code, detail=b""):
        """Send an EVENT report (Sec. 5.4) with the onboard computer's added fields."""
        self.report_no += 1
        body = struct.pack("<IBH", 1000 + self.report_no, c.REPORT_EVENT, code) + detail
        self._send(c.TYPE_REPORT, c.ADDED.pack(self.report_no, 2000 + self.report_no) + body)

    def _back_to_a(self):
        """Return to slot A version 1 in OPERATE with no trial time left.
        """
        self.state = dict(mode=1, slot=1, version=1)
        self.trial_left = 0

    def _switch(self):
        """The controller's side of SWITCH: trial, tests, then wait for CONFIRM."""
        self.tests_passed = False
        self.confirmed.clear()
        self.state = dict(mode=2, slot=2, version=2)
        self.trial_left = int(self.trial_s)
        self._event(c.EVENT_TRIAL_STARTED)
        time.sleep(TRIAL_REPORTS * REPORT_PERIOD_S)
        if self.rollback:
            self._back_to_a()
            return
        if self.fail_test is not None:
            self._event(c.EVENT_TESTS_FAILED, bytes([self.fail_test]))
            self._event(c.EVENT_GIVEN_UP)
            self._back_to_a()
            return
        self.tests_passed = True
        self._event(c.EVENT_TESTS_PASSED)
        if not self.confirmed.wait(self.trial_s):
            self._event(c.EVENT_GIVEN_UP)
            self._back_to_a()

    def _reports(self):
        """Report thread: send a HEALTH report every REPORT_PERIOD_S to the latest source.
        """
        while not self.stop:
            time.sleep(REPORT_PERIOD_S)
            if self.source is None:
                continue
            self.report_no += 1
            s = self.state
            body = c.HEALTH_BODY.pack(1000 + self.report_no, c.REPORT_HEALTH, s["mode"], 1,
                                      s["version"], s["slot"], 0, self.trial_left, 0)
            self._send(c.TYPE_REPORT, c.ADDED.pack(self.report_no, 2000 + self.report_no) + body)

    def stop_and_wait_held(self):
        """Every new sequence number arrived after the previous one's DONE/REFUSED was sent."""
        first = {}
        for t, seq, _ in self.arrivals:
            first.setdefault(seq, t)
        seqs = sorted(first)
        return all(first[b] >= self.done_at.get(a, float("inf")) for a, b in zip(seqs, seqs[1:]))
