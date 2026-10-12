#!/usr/bin/env python3
"""Ground console: drives a firmware update through the onboard computer over UDP.

Frames as in the Interfaces document, Sec. 4.0; answers as in Sec. 4.2; commands as in
Sec. 5.1; reports as in Sec. 5.4. Standard library only.

BENCH LIMIT (accepted for the demo): no envelope encryption and no command signatures
(Interfaces Sec. 4.1 is left out). Frames travel as plain UDP datagrams, one frame each.

Every command is stop-and-wait: the next one is sent only after the previous one's DONE or
REFUSED (Sec. 4.2). Commands whose answer does not come within the timeout are sent again
with the same sequence number, so the onboard computer answers ALREADY DONE and does not run
them twice (Sec. 4.2, REQ-031).

Subcommands:  status | upload IMAGE --number N | deliver --number N | switch | update
"""
import argparse
import random
import socket
import struct
import sys
import time
import zlib

# ---- Frame, Sec. 4.0 -----------------------------------------------------------------
SYNC = b"\xa5\x5a"                      # Sec. 4.0, field table: start of a frame
VERSION = 0x01                          # Sec. 4.0: frame format version
TYPE_COMMAND, TYPE_ANSWER, TYPE_REPORT = 0x01, 0x02, 0x03   # Sec. 4.0: Type field
DEST_ONBOARD, DEST_CONTROLLER, DEST_GROUND = 0x01, 0x02, 0x03   # Sec. 4.0: Destination field
HEADER = struct.Struct("<2sBBBBHII")    # Sec. 4.0: Sync, Version, Type, Dest, Flags, Length, Seq, Expiry
CHECK_BYTES = 4                         # Sec. 4.0: CRC-32 at the end
PAYLOAD_MAX = 1024                      # Sec. 4.0: Length is 0 to 1024
SEQ_MAX = 2**32 - 1                     # Sec. 4.0: sequence numbers run 1 to 2^32-1

# ---- Answers, Sec. 4.2 ---------------------------------------------------------------
ANSWER = struct.Struct("<IBB")          # Sec. 4.2: sequence answered (4), kind (1), code (1)
RECEIVED, DONE, REFUSED, ALREADY_DONE = 0x01, 0x02, 0x03, 0x04   # Sec. 4.2: kinds
KINDS = {RECEIVED: "RECEIVED", DONE: "DONE", REFUSED: "REFUSED", ALREADY_DONE: "ALREADY DONE"}
REFUSAL_TEXT = {                        # Sec. 4.2: table of REFUSED codes
    0x01: "Dangerous command without a valid arming command",
    0x02: "Arming command expired",
    0x03: "Command past its expiry",
    0x04: "Old command sent again",
    0x05: "Not allowed in the current mode",
    0x06: "Impossible values, or an unknown command",
    0x07: "No new firmware during a trial",
    0x08: "Image damaged",
    0x09: "Image not from the operator",
    0x0A: "Image older than the minimum version",
    0x0B: "Image built for another controller",
    0x0C: "The controller's clock disagrees with the scheduled time",
    0x0D: "A dangerous command cannot be scheduled",
    0x0E: "Image built for the other slot",
}

# ---- Commands, Sec. 5.1 --------------------------------------------------------------
UPLOAD_BEGIN, UPLOAD_CHUNK, UPLOAD_END, UPLOAD_CANCEL, DELIVER = 0x01, 0x02, 0x03, 0x04, 0x05
ARM, SWITCH, GET_STATUS = 0x0D, 0x20, 0x25
CHUNK_MAX = 256                         # Sec. 5.1: UPLOAD CHUNK carries up to 256 data bytes

# ---- Reports, Sec. 5.4 ---------------------------------------------------------------
REPORT_HEALTH, REPORT_EVENT = 0x01, 0x02            # Sec. 5.4: report kind
MODES = {0x01: "OPERATE", 0x02: "TRIAL", 0x03: "FALLBACK"}          # Sec. 5.4: HEALTH mode
SLOTS = {0x01: "A", 0x02: "B", 0x03: "last-resort"}                  # Sec. 5.4: running slot
SELF_TEST = {0x00: "PASSED", 0x01: "FAILED"}                         # Sec. 5.4: last self-test
EVENTS = {                                                           # Sec. 5.4: event codes
    0x0001: "fault detected", 0x0002: "fall-back to the previous copy",
    0x0003: "fall-back to the last-resort firmware", 0x0004: "self-test failed",
    0x0005: "change in memory detected", 0x0006: "trial started",
    0x0007: "firmware confirmed", 0x0008: "firmware given up", 0x0009: "restart",
    0x0101: "data link lost", 0x0102: "data link working again",
    0x0103: "restore done", 0x0104: "health reports dropped",
    0x0105: "onboard computer clock differs from ground time",
}
ADDED = struct.Struct("<II")            # Sec. 5.4: report number (4), onboard clock time (4)
HEALTH_BODY = struct.Struct("<IBBBIBBHB")   # Sec. 5.4 order: controller clock (4), kind (1),
#   mode (1), firmware kind (1), version (4), slot (1), failed starts (1), trial left (2), self-test (1)
HEALTH_LEN = HEALTH_BODY.size           # 16 bytes as the controller sends it
HEALTH_LEN_ADDED = ADDED.size + HEALTH_LEN   # 24 bytes once the onboard computer has added its two fields

# ---- Bench values --------------------------------------------------------------------
DEFAULT_HOST, DEFAULT_PORT = "192.168.7.1", 47001   # brief: onboard computer bench address and port
EXPIRY_S = 60               # bench value: a command is good for 60 s; ground time is Unix time on the bench
TIMEOUT_S = 3.0             # brief: a command normally answers within a few ms; 3 s counts as lost
# DELIVER moves the whole image to the controller over data link A (115200 baud 8N1 =
# 11520 byte/s, Sec. 6.2): a 64 KiB image is about 6 s of line time, plus flash writes and
# the framing overhead, so 30 s leaves a margin of several times.
DELIVER_TIMEOUT_S = 30.0
RETRIES = 5                 # resends of one command before giving up; keeps a dead link from hanging the demo
HEALTH_WAIT_S = 10.0        # how long `update` waits for the first HEALTH report
UPDATE_WATCH_S = 60.0       # brief: the update has 60 s after SWITCH to confirm or roll back
STATUS_SECONDS = 10.0       # default listening time of `status`
RECV_BYTES = 2048           # larger than the biggest frame (16 + 1024 + 4)


class Refused(Exception):
    """A command was REFUSED or never answered."""


def build(ftype, dest, seq, payload, expiry=0):
    head = HEADER.pack(SYNC, VERSION, ftype, dest, 0, len(payload), seq, expiry) + payload
    return head + struct.pack("<I", zlib.crc32(head))   # Sec. 4.0: CRC-32 like zlib over all bytes before it


def parse(data):
    """One datagram -> frame dict, or None if it is not a good frame (Sec. 4.0: receiver drops it)."""
    if len(data) < HEADER.size + CHECK_BYTES:
        return None
    sync, version, ftype, dest, flags, length, seq, expiry = HEADER.unpack_from(data)
    if sync != SYNC or version != VERSION or length > PAYLOAD_MAX:
        return None
    if len(data) != HEADER.size + length + CHECK_BYTES:
        return None
    if struct.unpack_from("<I", data, len(data) - CHECK_BYTES)[0] != zlib.crc32(data[:-CHECK_BYTES]):
        return None
    return dict(type=ftype, dest=dest, seq=seq, payload=data[HEADER.size:HEADER.size + length])


def decode_report(payload):
    """Sec. 5.4 report payload -> dict. Added fields (report number, onboard time) if present."""
    out = {}
    if len(payload) >= HEALTH_LEN_ADDED:
        out["report_number"], out["onboard_time"] = ADDED.unpack_from(payload)
        payload = payload[ADDED.size:]
    if len(payload) >= 5 and payload[4] == REPORT_EVENT:
        code = struct.unpack_from("<H", payload, 5)[0] if len(payload) >= 7 else 0
        return dict(out, kind="EVENT", code=code, text=EVENTS.get(code, f"event 0x{code:04x}"),
                    detail=payload[7:])
    if len(payload) != HEALTH_LEN or payload[4] != REPORT_HEALTH:
        return None
    clock, _, mode, fwkind, version, slot, failed, trial_left, selftest = HEALTH_BODY.unpack(payload)
    return dict(out, kind="HEALTH", controller_time=clock, mode=MODES.get(mode, f"0x{mode:02x}"),
                fw_kind=fwkind, version=version, slot=SLOTS.get(slot, f"0x{slot:02x}"),
                slot_code=slot, failed_starts=failed, trial_left=trial_left,
                self_test=SELF_TEST.get(selftest, f"0x{selftest:02x}"))


def format_report(r):
    extra = ""
    if "report_number" in r:
        extra = f"#{r['report_number']} onboard t={r['onboard_time']} "
    if r["kind"] == "EVENT":
        return f"EVENT   {extra}0x{r['code']:04x} {r['text']}"
    return (f"HEALTH  {extra}mode {r['mode']}, firmware kind {r['fw_kind']} version {r['version']}, "
            f"slot {r['slot']}, failed starts {r['failed_starts']}, trial left {r['trial_left']} s, "
            f"self-test {r['self_test']}")


class Console:
    """One UDP socket to the onboard computer; stop-and-wait commands; collects reports."""

    def __init__(self, host, port, loss=0.0, timeout=TIMEOUT_S, first_seq=1, local_port=0,
                 quiet=False, rng=None):
        self.addr = (host, port)
        self.loss = loss
        self.timeout = timeout
        self.seq = first_seq - 1            # Sec. 4.0: first frame is 1 [REQ-065]
        self.quiet = quiet
        self.rng = rng or random.Random()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("", local_port))    # the onboard computer reports to our source address
        self.dropped = 0
        self.sent = 0
        self.health = None                  # latest HEALTH report
        self.reports = []                   # reports received and not yet consumed

    def say(self, text, end="\n"):
        if not self.quiet:
            print(text, end=end, flush=True)

    def next_seq(self):
        self.seq += 1
        if self.seq > SEQ_MAX:
            raise Refused("sequence numbers used up")
        return self.seq

    def transmit(self, frame):
        """Radio simulator, simplest form: drop with probability `loss` before sending."""
        if self.loss > 0 and self.rng.random() < self.loss:
            self.dropped += 1
            return
        self.sock.sendto(frame, self.addr)
        self.sent += 1

    def pump(self, wait):
        """Wait up to `wait` s for one datagram; returns a frame or None. Reports are stored."""
        self.sock.settimeout(max(wait, 0.001))
        try:
            data, _ = self.sock.recvfrom(RECV_BYTES)
        except (socket.timeout, BlockingIOError):
            return None
        frame = parse(data)
        if frame and frame["type"] == TYPE_REPORT:
            report = decode_report(frame["payload"])
            if report:
                self.reports.append(report)
                if report["kind"] == "HEALTH":
                    self.health = report
                self.say("report  " + format_report(report))
            return None
        return frame

    def command(self, dest, payload, timeout=None, what="command"):
        """Send one command and wait for its DONE (or REFUSED). Returns the DONE kind.
        Resends the identical frame (same sequence number) after each timeout."""
        timeout = timeout or self.timeout
        seq = self.next_seq()
        frame = build(TYPE_COMMAND, dest, seq, payload, int(time.time()) + EXPIRY_S)
        for attempt in range(RETRIES + 1):
            self.transmit(frame)
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                answer = self.pump(end - time.monotonic())
                if not answer or answer["type"] != TYPE_ANSWER or len(answer["payload"]) != ANSWER.size:
                    continue
                answered, kind, code = ANSWER.unpack(answer["payload"])
                if answered != seq:
                    continue                # a late duplicate answer to an earlier command
                if kind == RECEIVED:
                    continue
                if kind == REFUSED:
                    text = REFUSAL_TEXT.get(code, "unknown reason")
                    raise Refused(f"{what} (seq {seq}) REFUSED 0x{code:02x}: {text}")
                if kind in (DONE, ALREADY_DONE):
                    return kind
            if attempt < RETRIES:
                self.say(f"no answer to {what} (seq {seq}) within {timeout:g} s, sending again")
        raise Refused(f"{what} (seq {seq}): no answer after {RETRIES + 1} tries")


# ---- Commands ------------------------------------------------------------------------
def upload(con, data, number, start=0):
    """UPLOAD BEGIN, CHUNK..., END (Sec. 5.1). `start` > 0 resumes: BEGIN is skipped."""
    t0 = time.monotonic()
    total = len(data)
    if start == 0:
        con.command(DEST_ONBOARD, struct.pack("<BHI", UPLOAD_BEGIN, number, total), what="UPLOAD BEGIN")
    offset = start
    try:
        while offset < total:
            chunk = data[offset:offset + CHUNK_MAX]
            con.command(DEST_ONBOARD, struct.pack("<BHI", UPLOAD_CHUNK, number, offset) + chunk,
                        what=f"UPLOAD CHUNK offset {offset}")
            offset += len(chunk)
            con.say(f"upload  {offset}/{total} bytes ({100 * offset // max(total, 1)} %)   ", end="\r")
        con.command(DEST_ONBOARD, struct.pack("<BH", UPLOAD_END, number), what="UPLOAD END")
    except Refused as e:
        raise Refused(f"{e}; the onboard computer has bytes up to offset {offset}: "
                      f"resume with --offset {offset}") from None
    con.say(f"\nupload  complete, {total} bytes in {time.monotonic() - t0:.2f} s")


def deliver(con, number):
    t0 = time.monotonic()
    con.command(DEST_ONBOARD, struct.pack("<BH", DELIVER, number), timeout=DELIVER_TIMEOUT_S, what="DELIVER")
    con.say(f"deliver complete in {time.monotonic() - t0:.2f} s")


def arm_and_switch(con):
    """ARM naming SWITCH and the sequence number SWITCH will use, then SWITCH (Sec. 5.1, 5.2)."""
    switch_seq = con.seq + 2            # ARM takes the next number, SWITCH the one after
    con.command(DEST_CONTROLLER, struct.pack("<BBI", ARM, SWITCH, switch_seq), what="ARM")
    con.command(DEST_CONTROLLER, bytes([SWITCH]), what="SWITCH")
    con.say("switch  sent")


def wait_health(con, seconds):
    """Latest HEALTH report within `seconds`, sending GET STATUS first so the onboard computer
    knows where to forward reports (it reports to the source of the latest datagram)."""
    con.health = None
    con.command(DEST_CONTROLLER, bytes([GET_STATUS]), what="GET STATUS")
    end = time.monotonic() + seconds
    while con.health is None and time.monotonic() < end:
        con.pump(end - time.monotonic())
    return con.health


def watch_update(con, old, free_code, seconds):
    """Watch HEALTH reports after SWITCH. Returns ('confirmed'|'rolled back'|'timeout', report)."""
    left = False                        # has the controller left the old state since SWITCH?
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        con.pump(min(0.5, end - time.monotonic()))
        while con.reports:
            r = con.reports.pop(0)
            if r["kind"] != "HEALTH":
                continue
            if r["slot_code"] == free_code and r["mode"] == "OPERATE":
                return "confirmed", r
            if r["mode"] != "OPERATE" or r["slot_code"] != old["slot_code"]:
                left = True
            elif left and r["mode"] == "OPERATE" and r["slot_code"] == old["slot_code"]:
                return "rolled back", r
    return "timeout", con.health


def image_for_free_slot(running_code, image_dir):
    if running_code == 0x01:
        return "B", f"{image_dir}/firmware-B.img", 0x02
    if running_code == 0x02:
        return "A", f"{image_dir}/firmware-A.img", 0x01
    raise Refused("controller runs the last-resort firmware: no free slot to update")


def cmd_update(con, args):
    t0 = time.monotonic()
    con.say("update  reading a HEALTH report")
    old = wait_health(con, HEALTH_WAIT_S)
    if old is None:
        raise Refused("no HEALTH report received")
    if old["mode"] == "TRIAL":
        raise Refused("controller is in a trial: no new firmware during a trial (REFUSED 0x07 would follow)")
    name, path, free_code = image_for_free_slot(old["slot_code"], args.image_dir)
    con.say(f"update  running slot {old['slot']} version {old['version']}; free slot {name}, image {path}")
    with open(path, "rb") as f:
        data = f.read()
    upload(con, data, args.number)
    deliver(con, args.number)
    con.reports.clear()                 # only reports from now on count
    arm_and_switch(con)
    result, r = watch_update(con, old, free_code, UPDATE_WATCH_S)
    elapsed = time.monotonic() - t0
    if result == "confirmed":
        print(f"UPDATE CONFIRMED: slot {r['slot']} version {r['version']}  ({elapsed:.1f} s)")
        return 0
    if result == "rolled back":
        print(f"UPDATE ROLLED BACK: running slot {r['slot']} version {r['version']}  ({elapsed:.1f} s)")
        return 1
    print(f"UPDATE UNRESOLVED: no confirmation or rollback within {UPDATE_WATCH_S:g} s  ({elapsed:.1f} s)")
    return 2


def cmd_status(con, args):
    con.command(DEST_CONTROLLER, bytes([GET_STATUS]), what="GET STATUS")
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        con.pump(end - time.monotonic())
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=DEFAULT_HOST, help="onboard computer address (default %(default)s)")
    p.add_argument("--port", type=int, default=DEFAULT_PORT, help="onboard computer UDP port")
    p.add_argument("--local-port", type=int, default=0, help="local UDP port to bind (0: any)")
    p.add_argument("--radio-loss", type=float, default=0.0, metavar="P",
                   help="drop each outgoing datagram with probability P (0..1), resend after the timeout")
    p.add_argument("--timeout", type=float, default=TIMEOUT_S, help="seconds before a command is sent again")
    p.add_argument("--first-seq", type=int, default=1,
                   help="first sequence number (1; raise it when the onboard computer remembers an earlier run, REQ-065)")
    p.add_argument("--seed", type=int, default=None, help="seed of the radio-loss random numbers")
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("status", help="print HEALTH reports for N seconds")
    s.add_argument("--seconds", type=float, default=STATUS_SECONDS)
    s = sub.add_parser("upload", help="UPLOAD BEGIN / CHUNK / END")
    s.add_argument("image")
    s.add_argument("--number", type=int, required=True)
    s.add_argument("--offset", type=int, default=0, help="resume: first byte not yet on the onboard computer")
    s = sub.add_parser("deliver", help="DELIVER an uploaded image to the controller")
    s.add_argument("--number", type=int, required=True)
    sub.add_parser("switch", help="ARM, then SWITCH, both to the controller")
    s = sub.add_parser("update", help="whole update: status, upload, deliver, arm, switch, watch")
    s.add_argument("--number", type=int, default=1, help="image number (default 1)")
    s.add_argument("--image-dir", default="src/controller/build/images")
    a = p.parse_args(argv)
    if not 0.0 <= a.radio_loss <= 1.0:
        p.error("--radio-loss must be between 0 and 1")

    con = Console(a.host, a.port, a.radio_loss, a.timeout, a.first_seq, a.local_port,
                  rng=random.Random(a.seed))
    try:
        if a.cmd == "status":
            code = cmd_status(con, a)
        elif a.cmd == "upload":
            with open(a.image, "rb") as f:
                upload(con, f.read(), a.number, a.offset)
            code = 0
        elif a.cmd == "deliver":
            deliver(con, a.number)
            code = 0
        elif a.cmd == "switch":
            arm_and_switch(con)
            code = 0
        else:
            code = cmd_update(con, a)
    except Refused as e:
        print(f"FAILED: {e}")
        code = 1
    finally:
        con.sock.close()
    if a.radio_loss > 0:
        print(f"radio simulator: dropped {con.dropped} of {con.dropped + con.sent} outgoing datagrams")
    return code


if __name__ == "__main__":
    sys.exit(main())
