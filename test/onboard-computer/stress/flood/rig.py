#!/usr/bin/env python3
"""One rig of the flood stress audit: the real fsir-updater, a fake controller, a fake ground.

    fake ground (UDP) --> fsir-updater <--pseudo-terminal--> fake controller
         ^ flood_sender.py floods the updater's UDP port at 10 000 datagrams per second

One rig runs one scenario against one flood kind (stress audit A.5):
  upload    a separate ground session uploads firmware-B.img over and over;
  delivery  the same image is delivered to the fake controller over and over;
  beat      --beat-s 1: GET STATUS to the controller while the link is idle;
  trial     the fake controller reports TRIAL and never confirms;
  silence   the fake controller stops sending.
The rig samples the updater once a second (RSS, CPU, log size), and writes a log with the
answers, the timer errors and the numbers, a series file, and the updater's log excerpt.

The fake controller is bench_test.FakeController (bench_test.py:156-318), imported read-only;
the frame builder is bench_test.build (bench_test.py:86-102). Standard library only.

Usage: rig.py --scenario S --flood K --updater PATH --image FILE --work DIR --out DIR
              --commit HASH [--source same|foreign] [--tag NAME] [--seed N]
"""
import argparse
import array
import json
import mmap
import os
import re
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

sys.dont_write_bytecode = True   # the source tree must stay as it is: no __pycache__ in test/
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import bench_test as B  # noqa: E402
import flood_sender as FS  # noqa: E402

# ---- Timers of the updater, the values the measured times are compared with ------------------
# Each is read from config.hpp, which carries the source of the value (decision H, decision 20).
TRIAL_EXPECTED_S = (120000 + 30000) / 1000.0   # kTrialMs + kTrialMarginMs [config.hpp:69-70]
SILENCE_EXPECTED_S = 5.0                         # kSilenceMs [config.hpp:93]
SILENT_GAP_S = 60.0                              # kSilentResetGapMs [config.hpp:97]
SILENT_RESETS_MAX = 3                            # kSilentResetsMax [config.hpp:100]
BEAT_S = 1                                       # --beat-s given to the updater (see BEAT_WHY)
BEAT_WHY = ("1 s is the smallest non-zero --beat-s [options.cpp:parseOptions, parseNumber min 0 "
            "and whole seconds]: 60 intervals in 65 s, and the 50 ms loop pass "
            "(kPollMs, config.hpp:222) is 5 percent of it, measurable")

# ---- Bench choices of this harness (no source; stated so that they can be changed) ------------
FLOOD_S = 65.0            # flood time of the short scenarios: the 60 s asked for plus 5 s margin
FLOOD_TAIL_S = 10.0       # the flood runs this long after the last new cycle is started
FLOOD_RATE = 10000        # datagrams per second asked for (the task)
CMD_WAIT_S = 2.0          # wait for the last answer of one command; kAnswerTimeoutMs = 2 s
CMD_RESENDS = 2           # resends of the same bytes after a missing answer (REQ-031)
OVERRUN_GRACE_S = 5.0     # a cycle may run this long past the end of the scenario
DELIVER_WAIT_S = 15.0     # as bench_test.py:864 (timeout=15)
PROBE_PERIOD_S = 0.1      # one probe GET STATUS to the onboard computer every 100 ms
PROBE_WAIT_S = 2.0        # a probe unanswered after this long counts as lost
BEAT_WARMUP_S = 2.0       # beats in the first seconds of the flood are not measured
RESET_WAIT_MAX_S = 30.0   # longest wait past the expected time for a reset before "no reset"
POST_EVENT_S = 5.0        # wait after the last reset for EVENT 0x0106 to reach the ground
SILENCE_RUN_S = SILENCE_EXPECTED_S + 3 * SILENT_GAP_S + 20.0   # one gap past the give-up + 15 s
SAMPLE_S = 1.0            # sampling period of the updater (task: once a second)
START_WAIT_S = 5.0        # longest wait for the updater to open the pseudo-terminal
STOP_WAIT_S = 5.0         # longest wait for SIGTERM to stop the updater [bench_test.py:622]
FIRST_IMAGE = 2           # first image number of the upload scenario
DELIVER_IMAGE = 1         # image number stored before the flood and delivered during it
LOG_EDGE_LINES = 200      # head and tail lines of the updater log kept (task)
LOG_COUNTS = ("ground: command", "not a good frame", "more datagrams dropped",
              "runtime error", "ground link: answers and reports now go to",
              "no last answer", "no answer to")
RECV_POLL_S = 0.1         # the receiving thread wakes at least this often to see its stop flag
DRAIN_S = 2.0             # wait after the flood for queued answers to reach the ground
DRAIN_BATCH = 500         # most datagrams one wakeup of the receiving thread takes
SOCKET_BUFFER = 4 * 1024 * 1024   # kern.ipc.maxsockbuf = 8388608 (sysctl, measured)
DONE, RECEIVED_KIND = B.DONE, B.RECEIVED
ANSWER_BYTES = 26         # 16 header + 6 payload + 4 check [frame.hpp:46, kAnswerBytes]
LOG_LINE = re.compile(r"^\[\s*(\d+)\.(\d{3})\] (.*)$")


def stats(values):
    """Summarises a list of numbers: count, minimum, median, 95th and 99th percentile, maximum.

    A summary instead of the series keeps the logs short (task).

    Args:
        values: numbers, any one unit

    Returns:
        A dict; only {"n": 0} for an empty list. Values are in the unit given.
    """
    data = sorted(values)
    n = len(data)
    if n == 0:
        return {"n": 0}

    def pick(q):
        """Picks the value at fraction q of the sorted list (nearest rank).

        Args:
            q: fraction between 0 and 1

        Returns:
            The value.
        """
        return data[min(n - 1, int(q * n))]

    mean = sum(data) / n
    var = sum((v - mean) ** 2 for v in data) / n
    return {"n": n, "min": data[0], "p50": pick(0.5), "p95": pick(0.95), "p99": pick(0.99),
            "max": data[-1], "mean": mean, "std": var ** 0.5}


def on_term(signum, frame):
    """Turns SIGTERM into SystemExit, so the finally clause of main() stops the updater.

    The master sends SIGTERM to the exact PID of a rig that ran past its time limit; without
    this the rig would die and leave the updater and the flood sender running.

    Args:
        signum: signal number (unused)
        frame: interrupted stack frame (unused)

    Returns:
        Does not return.
    """
    raise SystemExit(3)


def now_text():
    """Gives the Mac's local time with its zone, for the first line of a log.

    Returns:
        Text such as "2026-10-16 10:44:00 EEST".
    """
    return time.strftime("%Y-%m-%d %H:%M:%S %Z")


def udp_counters():
    """Reads this Mac's UDP counters from netstat: datagrams received, dropped for a full buffer.

    The counters are system-wide, so other programs' traffic is in them too (stated in the log).

    Returns:
        A dict with "received" and "dropped_full_buffer", or an empty dict if not found.
    """
    out = subprocess.run(["netstat", "-s", "-p", "udp"], capture_output=True, text=True).stdout
    found = {}
    for key, pattern in (("received", r"(\d+) datagrams received"),
                         ("dropped_full_buffer", r"(\d+) dropped due to full socket buffers")):
        m = re.search(pattern, out)
        if m:
            found[key] = int(m.group(1))
    return found


def grep_log(path, patterns):
    """Finds the lines of a log that contain any of the fixed strings.

    The log can be hundreds of megabytes under a flood, so grep reads it, not this program.

    Args:
        path: log file
        patterns: strings to look for

    Returns:
        A list of (stamp in seconds, text) pairs in file order; lines without a stamp are skipped.
    """
    cmd = ["grep", "-F"]
    for p in patterns:
        cmd += ["-e", p]
    out = subprocess.run(cmd + [path], capture_output=True, text=True, errors="replace").stdout
    found = []
    for line in out.splitlines():
        m = LOG_LINE.match(line)
        if m:
            found.append((int(m.group(1)) + int(m.group(2)) / 1000.0, m.group(3)))
    return found


def count_log(path, pattern):
    """Counts the lines of a log that contain a fixed string.

    Args:
        path: log file
        pattern: the string

    Returns:
        The count.
    """
    out = subprocess.run(["grep", "-F", "-c", pattern, path], capture_output=True, text=True)
    return int(out.stdout.strip() or 0)


def log_excerpt(path, out_path, edge, header):
    """Writes the first and last lines of a log, and its total size, to a file.

    The task asks for the first and last 200 lines and the total size, so a flood does not leave
    a multi-megabyte file in the results.

    Args:
        path: the full log
        out_path: file to write
        edge: number of lines at each end
        header: first line of the file (time, commit, command)

    Returns:
        A dict with the size in bytes and the line count of the full log.
    """
    size = os.path.getsize(path)
    lines = int(subprocess.run(["wc", "-l", path], capture_output=True, text=True)
                .stdout.split()[0])
    head = subprocess.run(["head", "-n", str(edge), path], capture_output=True, text=True,
                          errors="replace").stdout
    tail = subprocess.run(["tail", "-n", str(edge), path], capture_output=True, text=True,
                          errors="replace").stdout
    with open(out_path, "w") as out:
        out.write(header + "\n")
        out.write(f"updater log: {size} bytes, {lines} lines; first {edge} and last {edge} lines\n")
        out.write("---- first lines ----\n" + head)
        if lines > 2 * edge:
            out.write("---- last lines ----\n" + tail)
    return {"bytes": size, "lines": lines}


class Fake(B.FakeController):
    """The bench fake controller, plus a list of the times of its HEALTH reports."""

    def __init__(self, fd):
        """Makes the fake controller on the pseudo-terminal's master side.

        Args:
            fd: file descriptor of the pseudo-terminal master

        Returns:
            None.
        """
        super().__init__(fd)
        self.health_log = []   # (wall time, monotonic time, mode) of each HEALTH written

    def send_health(self):
        """Notes the time, then sends one HEALTH report as the bench fake does.

        The time is taken just before the write, so the timer tests can compare the updater's
        reaction with the moment the controller spoke.

        Returns:
            None.
        """
        self.health_log.append((time.time(), time.monotonic(), self.mode))
        super().send_health()


class Ground:
    """The fake ground: one UDP socket, a thread that reads every datagram it gets back."""

    def __init__(self, port):
        """Opens the socket and starts the receiving thread.

        Blocking socket read with select, so the descriptor stays shareable with the flood
        sender process (a non-blocking flag would be shared with it).

        Args:
            port: the updater's UDP port on 127.0.0.1

        Returns:
            None.
        """
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, SOCKET_BUFFER)
        self.addr = ("127.0.0.1", port)
        self.cond = threading.Condition()
        self.lock = threading.Lock()
        self.seq = 0
        self.answers = {}            # ground sequence -> [(kind, code, monotonic time)]
        self.reports = []            # decoded reports from the updater
        self.flood_kinds = {}        # answer kind -> count, for answers to flood sequences
        self.flood_done_ms = array.array("d")   # send-to-answer time of fresh valid frames
        self.times = None            # mmap of the send times written by the flood sender
        self.other = 0               # datagrams that decode to nothing known
        self.stop = False
        self.thread = threading.Thread(target=self.receive, daemon=True)
        self.thread.start()

    def receive(self):
        """Thread body: reads datagrams until stopped.

        At most DRAIN_BATCH datagrams per wakeup, then back to select, so the stop flag is seen.

        Returns:
            None.
        """
        while not self.stop:
            if not select.select([self.sock], [], [], RECV_POLL_S)[0]:
                continue
            for _ in range(DRAIN_BATCH):
                try:
                    data = self.sock.recv(4096, socket.MSG_DONTWAIT)
                except OSError:
                    break
                self.take(data, time.monotonic())

    def take(self, data, now):
        """Sorts one datagram: an answer to the flood is counted, anything else is kept.

        An answer to a flood sequence is recognised by its size and sequence field without a
        check value, to keep the cost per datagram small at 20 000 answers a second.

        Args:
            data: the datagram
            now: monotonic time when it was read, seconds

        Returns:
            None.
        """
        if len(data) == ANSWER_BYTES and data[3] == B.ANSWER:
            answered = struct.unpack_from("<I", data, B.HEADER.size)[0]
            if answered >= FS.VALID_BASE:
                kind = data[B.HEADER.size + 4]
                self.flood_kinds[kind] = self.flood_kinds.get(kind, 0) + 1
                index = answered - FS.VALID_BASE
                if (kind == DONE and answered < FS.REPEAT_BASE and self.times is not None
                        and (index + 1) * 8 <= len(self.times)):
                    sent = struct.unpack_from("<d", self.times, index * 8)[0]
                    self.flood_done_ms.append((now - sent) * 1000.0)
                return
        frame = B.decode(data)
        if frame is None:
            self.other += 1
            return
        with self.cond:
            if frame["type"] == B.ANSWER and len(frame["payload"]) == B.ANSWER_FIELDS.size:
                answered, kind, code = B.ANSWER_FIELDS.unpack(frame["payload"])
                self.answers.setdefault(answered, []).append((kind, code, now))
            elif frame["type"] == B.REPORT and len(self.reports) < 20000:
                frame["t"] = now
                self.reports.append(frame)
            self.cond.notify_all()

    def send(self, dest, payload, seq=None):
        """Sends one command to the updater.

        Args:
            dest: Destination byte
            payload: payload bytes
            seq: sequence number to use, or None for the next one of this session

        Returns:
            (sequence number, frame bytes, monotonic send time).
        """
        with self.lock:
            if seq is None:
                self.seq += 1
                seq = self.seq
        raw = B.build(B.COMMAND, dest, seq, payload, B.EXPIRY)
        sent = time.monotonic()
        self.sock.sendto(raw, self.addr)
        return seq, raw, sent

    def final(self, seq):
        """Gives the last answer to a command, if there is one.

        Args:
            seq: ground sequence number

        Returns:
            (kind, code, time) or None.
        """
        for item in self.answers.get(seq, []):
            if item[0] in B.FINAL:
                return item
        return None

    def command(self, dest, payload, wait=CMD_WAIT_S, resends=CMD_RESENDS):
        """Sends a command and waits for its last answer, resending the same bytes if it is late.

        A resend with the same sequence number and bytes is what REQ-031 allows after a lost
        answer. The wait is bounded: (resends + 1) x wait.

        Args:
            dest: Destination byte
            payload: payload bytes
            wait: seconds to wait for the last answer after each send
            resends: how many times the same bytes are sent again

        Returns:
            A dict: seq, final (kind, code, time) or None, sent (first send time), resent.
        """
        seq, raw, first = self.send(dest, payload)
        resent = 0
        for attempt in range(resends + 1):
            with self.cond:
                self.cond.wait_for(lambda: self.final(seq) is not None, timeout=wait)
            if self.final(seq) is not None:
                break
            if attempt < resends:
                self.sock.sendto(raw, self.addr)
                resent += 1
        return dict(seq=seq, final=self.final(seq), sent=first, resent=resent)


class Probe(threading.Thread):
    """Sends GET STATUS to the onboard computer every PROBE_PERIOD_S and keeps the send times."""

    def __init__(self, ground):
        """Makes the probe thread (not yet started).

        Args:
            ground: the Ground to send from

        Returns:
            None.
        """
        super().__init__(daemon=True)
        self.ground = ground
        self.sent = []           # (sequence, monotonic send time)
        self.halt = False

    def run(self):
        """Thread body: one GET STATUS per period until halted.

        Returns:
            None.
        """
        nxt = time.monotonic()
        while not self.halt:
            seq, _, sent = self.ground.send(B.ONBOARD, bytes([B.GET_STATUS]))
            self.sent.append((seq, sent))
            nxt += PROBE_PERIOD_S
            time.sleep(max(0.0, nxt - time.monotonic()))

    def result(self):
        """Counts the probes answered (DONE) within PROBE_WAIT_S and the answer latencies.

        Probes sent in the last PROBE_WAIT_S before the call are left out: their answers may
        still be on the way.

        Returns:
            A dict: sent, answered, share, latency_ms (stats).
        """
        horizon = time.monotonic() - PROBE_WAIT_S
        counted = [(s, t) for s, t in self.sent if t < horizon]
        lat = []
        for seq, t in counted:
            fin = self.ground.final(seq)
            if fin is not None and fin[0] == DONE and fin[2] - t <= PROBE_WAIT_S:
                lat.append((fin[2] - t) * 1000.0)
        return dict(sent=len(counted), answered=len(lat),
                    share=(len(lat) / len(counted)) if counted else None, latency_ms=stats(lat))


def parse_ps_time(text):
    """Turns the TIME column of ps ([H:]M:SS.hh) into seconds.

    Args:
        text: the column

    Returns:
        CPU seconds.
    """
    total = 0.0
    for part in text.split(":"):
        total = total * 60.0 + float(part)
    return total


class Sampler(threading.Thread):
    """Once a second: the updater's RSS, CPU use, log size, and whether it is alive."""

    def __init__(self, proc, log_path):
        """Makes the sampler thread (not yet started).

        Args:
            proc: the subprocess.Popen of the updater
            log_path: the updater's log file

        Returns:
            None.
        """
        super().__init__(daemon=True)
        self.proc = proc
        self.log_path = log_path
        self.rows = []     # (t, rss_kb, cpu_percent, log_bytes, alive, load1)
        self.halt = False

    def sample(self, last):
        """Takes one sample with ps -o rss=,time= for the exact PID.

        CPU percent is the change of the process's CPU time over the time between two samples.

        Args:
            last: (monotonic time, cpu seconds) of the previous sample, or None

        Returns:
            (row or None, new last)
        """
        alive = self.proc.poll() is None
        out = subprocess.run(["ps", "-o", "rss=,time=", "-p", str(self.proc.pid)],
                             capture_output=True, text=True).stdout.split()
        t = time.monotonic()
        if len(out) < 2:
            return (t, 0, 0.0, os.path.getsize(self.log_path), alive, os.getloadavg()[0]), last
        cpu = parse_ps_time(out[1])
        pct = 0.0
        if last is not None and t > last[0]:
            pct = 100.0 * (cpu - last[1]) / (t - last[0])
        return ((t, int(out[0]), pct, os.path.getsize(self.log_path), alive,
                 os.getloadavg()[0]), (t, cpu))

    def run(self):
        """Thread body: sample every SAMPLE_S until halted.

        Returns:
            None.
        """
        last = None
        nxt = time.monotonic()
        while not self.halt:
            row, last = self.sample(last)
            self.rows.append(row)
            nxt += SAMPLE_S
            time.sleep(max(0.0, nxt - time.monotonic()))


class FloodRun:
    """One flood: the sender process, its statistics, and the files it needs."""

    def __init__(self, kind, source, seed, rig):
        """Prepares a flood of one kind against a rig; nothing is sent yet.

        Args:
            kind: "none" or one of flood_sender.KINDS
            source: "same" (the rig's ground socket) or "foreign" (its own socket)
            seed: seed of the garbage generator
            rig: the Rig

        Returns:
            None.
        """
        self.kind = kind
        self.source = source
        self.seed = seed
        self.rig = rig
        self.proc = None
        self.cmd = None
        self.stats_path = os.path.join(rig.work, "flood-stats.json")
        self.times_path = os.path.join(rig.work, "flood-times.bin")
        self.started = None
        self.stats = None
        self.counters_before = None

    def start(self, seconds):
        """Starts the sender for the given time; does nothing for kind "none".

        Args:
            seconds: flood time, seconds

        Returns:
            None.
        """
        self.counters_before = udp_counters()
        self.started = time.monotonic()
        if self.kind == "none":
            return
        cmd = [sys.executable, os.path.join(HERE, "flood_sender.py"), "--port", str(self.rig.port),
               "--kind", self.kind, "--rate", str(FLOOD_RATE), "--seconds", str(seconds),
               "--seed", str(self.seed), "--stats", self.stats_path]
        pass_fds = ()
        if self.source == "same":
            fd = self.rig.ground.sock.fileno()
            cmd += ["--fd", str(fd)]
            pass_fds = (fd,)
        else:
            cmd += ["--foreign"]
        if self.kind in ("valid", "mix"):
            with open(self.times_path, "wb") as f:
                f.truncate(FS.times_bytes(FLOOD_RATE, seconds))
            cmd += ["--times", self.times_path]
            handle = open(self.times_path, "rb")
            self.rig.ground.times = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)
        self.cmd = cmd
        self.proc = subprocess.Popen(cmd, pass_fds=pass_fds)

    def stop(self):
        """Ends the sender: SIGTERM to the exact PID, a bounded wait, then its statistics.

        Returns:
            None.
        """
        if self.proc is None:
            return
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
        try:
            self.proc.wait(STOP_WAIT_S)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        try:
            with open(self.stats_path) as f:
                self.stats = json.load(f)
        except (OSError, ValueError):
            self.stats = {"error": "no statistics written"}

    def summary(self, ground, after):
        """Gives the flood's numbers: rate reached and the answers it received.

        Args:
            ground: the Ground that counted the answers
            after: udp_counters() taken after the flood

        Returns:
            A dict.
        """
        out = dict(kind=self.kind, source=self.source, seed=self.seed, cmd=self.cmd)
        if self.stats:
            keep = {k: v for k, v in self.stats.items() if k not in ("per_second", "last_hex")}
            out.update(keep)
            ps = self.stats.get("per_second", [])
            out["per_second_stats"] = stats(ps[:int(self.stats.get("elapsed_s", 0))])
        names = {B.RECEIVED: "RECEIVED", B.DONE: "DONE", B.REFUSED: "REFUSED",
                 B.ALREADY_DONE: "ALREADY_DONE"}
        out["answers_to_flood"] = {names.get(k, hex(k)): v for k, v in ground.flood_kinds.items()}
        sent_valid = (self.stats or {}).get("valid_frames", 0)
        done = ground.flood_kinds.get(DONE, 0)
        if self.kind == "valid":
            out["valid_done_share"] = done / sent_valid if sent_valid else None
        out["valid_done_latency_ms"] = stats(list(ground.flood_done_ms))
        if self.counters_before and after:
            out["udp_delta_system_wide"] = {k: after[k] - self.counters_before[k]
                                            for k in after if k in self.counters_before}
        return out


class Rig:
    """The updater process, its pseudo-terminal, the fake controller and the ground."""

    def __init__(self, args, extra):
        """Starts the updater with its own pseudo-terminal, port, data directory and restore script.

        The restore script is a copy of fake_restore.py with the interpreter named by absolute
        path: it appends the time of each call to restore-calls.txt and prints the result line
        the updater expects (restart_runner.hpp: "RESTORE DONE: reset").

        Args:
            args: the parsed command line
            extra: extra updater options

        Returns:
            None.
        """
        self.args = args
        self.work = os.path.join(args.work, args.tag)
        if os.path.isdir(self.work):
            shutil.rmtree(self.work)
        os.makedirs(self.work)
        self.master, self.slave = os.openpty()
        tty.setraw(self.slave)
        self.port = B.free_udp_port()
        self.data_dir = os.path.join(self.work, "images")
        self.log_path = os.path.join(self.work, "updater.log")
        self.calls_path = os.path.join(self.work, "restore-calls.txt")
        self.script = os.path.join(self.work, "restore.py")
        with open(os.path.join(HERE, "fake_restore.py")) as src, open(self.script, "w") as dst:
            dst.write(src.read().replace("@PYTHON@", sys.executable))
        os.chmod(self.script, 0o755)
        # The first run of a newly written script took 0.7 to 1.3 s on this Mac in the unflooded
        # baseline (calls 2 and 3: 0.06 s; cause not verified). The rig runs it once before the
        # updater starts, so that delay does not hide in the timer results; the call is erased.
        subprocess.run([self.script, "warm-up"], capture_output=True, timeout=START_WAIT_S * 6)
        os.remove(self.calls_path)
        self.log_file = open(self.log_path, "w")
        self.command = [args.updater, "--device", os.ttyname(self.slave), "--udp-port",
                        str(self.port), "--data-dir", self.data_dir, "--restore-script",
                        self.script, *extra]
        self.started_wall = time.time()
        self.proc = subprocess.Popen(self.command, stderr=self.log_file,
                                     stdout=subprocess.DEVNULL)
        self.fake = Fake(self.master)
        self.ground = None
        self.sampler = Sampler(self.proc, self.log_path)
        self.log_offset = None   # wall time of log stamp 0

    def log_has(self, text):
        """Tells whether the updater's log has a line with the text (the file is read by grep).

        Args:
            text: fixed string

        Returns:
            True if found.
        """
        return count_log(self.log_path, text) > 0

    def wait_log(self, text, timeout):
        """Waits for a log line, with an upper bound.

        Args:
            text: fixed string
            timeout: longest wait, seconds

        Returns:
            True if the line appeared.
        """
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.log_has(text):
                return True
            time.sleep(0.05)
        return self.log_has(text)

    def start(self):
        """Waits for the updater to open the link, starts the fake controller and the ground.

        Also calibrates the log clock (not in the beat scenario): the stamp of "controller heard"
        is the moment the updater read the first HEALTH, and the fake wrote it at a known wall
        time.

        Returns:
            True if the updater and the fake controller are up and talking.
        """
        if not self.wait_log("open, 115200", START_WAIT_S):
            return False
        self.fake.start()
        self.ground = Ground(self.port)
        self.sampler.start()
        if not self.wait_log("controller heard", START_WAIT_S):
            return False
        if self.args.scenario == "beat":
            return True   # the beat's own answers make "controller heard" appear before any HEALTH
        stamp = grep_log(self.log_path, ["controller heard"])[0][0]
        first_wall = self.fake.health_log[0][0]
        self.log_offset = first_wall - stamp
        return True

    def wall_of(self, stamp):
        """Converts an updater log stamp into wall time with the calibration of start().

        Args:
            stamp: seconds since the updater's start, from a log line

        Returns:
            Wall time, seconds since the epoch.
        """
        return self.log_offset + stamp

    def restore_calls(self):
        """Reads the times of the calls of the fake restore script.

        Returns:
            A list of wall times, seconds since the epoch.
        """
        try:
            with open(self.calls_path) as f:
                return [float(line.split()[0]) for line in f if line.strip()]
        except OSError:
            return []

    def finish(self):
        """Stops the sampler, the updater (SIGTERM to its exact PID), the fake and the ground.

        Returns:
            A dict: exit code of the updater and whether it was alive when asked to stop.
        """
        self.sampler.halt = True
        alive = self.proc.poll() is None
        if alive:
            self.proc.send_signal(signal.SIGTERM)
        try:
            code = self.proc.wait(STOP_WAIT_S)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            code = self.proc.wait()
            code = ("killed after %.0f s" % STOP_WAIT_S, code)
        self.fake.stop = True
        if self.ground is not None:
            self.ground.stop = True
        if self.fake.is_alive():
            self.fake.join(1)
        self.log_file.close()
        return dict(alive_at_stop=alive, exit_code=code)


def sleep_until(t_mono):
    """Sleeps until a monotonic time; returns at once if it has passed.

    Args:
        t_mono: monotonic time, seconds

    Returns:
        None.
    """
    time.sleep(max(0.0, t_mono - time.monotonic()))


def upload_cycle(rig, number, image, t_stop):
    """Uploads the image once as image `number` and checks the stored bytes.

    UPLOAD BEGIN, one UPLOAD CHUNK per 256 bytes, UPLOAD END; each command waits for its last
    answer (bench_test.py:559-571 does the same). The stored file is read back and compared.

    Args:
        rig: the Rig
        number: image number (16 bits)
        image: the image bytes
        t_stop: monotonic time after which the cycle gives up (plus OVERRUN_GRACE_S)

    Returns:
        A dict: ok, why (when not ok), seconds, commands, resends.
    """
    ground = rig.ground
    start = time.monotonic()
    plan = [struct.pack("<BHI", B.UPLOAD_BEGIN, number, len(image))]
    for offset in range(0, len(image), B.CHUNK):
        head = struct.pack("<BHI", B.UPLOAD_CHUNK, number, offset)
        plan.append(head + image[offset:offset + B.CHUNK])
    plan.append(struct.pack("<BH", B.UPLOAD_END, number))
    resends = 0
    for index, payload in enumerate(plan):
        if time.monotonic() > t_stop + OVERRUN_GRACE_S:
            return dict(ok=False, why=f"cut at command {index}: scenario time over", commands=index,
                        resends=resends, seconds=time.monotonic() - start)
        r = ground.command(B.ONBOARD, payload)
        resends += r["resent"]
        if r["final"] is None or r["final"][0] != DONE:
            why = "no final answer" if r["final"] is None else f"final kind {r['final'][:2]}"
            return dict(ok=False, why=f"command {index} of {len(plan)}: {why}", commands=index,
                        resends=resends, seconds=time.monotonic() - start)
    seconds = r["final"][2] - start
    stored = os.path.join(rig.data_dir, "image-%05d.img" % number)
    try:
        same = open(stored, "rb").read() == image
    except OSError:
        same = False
    return dict(ok=same, why="" if same else "stored bytes differ or file missing",
                commands=len(plan), resends=resends, seconds=seconds)


def scenario_upload(rig, flood, image, res):
    """Scenario 1: uploads of firmware-B.img by the rig's ground session during the flood.

    Cycles run back to back until FLOOD_S has passed; each uses a new image number.

    Args:
        rig: the Rig
        flood: the FloodRun
        image: image bytes
        res: result dict to fill

    Returns:
        None.
    """
    t0 = time.monotonic()
    flood.start(FLOOD_S + FLOOD_TAIL_S)
    t_stop = t0 + FLOOD_S
    cycles = []
    number = FIRST_IMAGE
    while time.monotonic() < t_stop:
        cycles.append(upload_cycle(rig, number, image, t_stop))
        number += 1
    ok = [c for c in cycles if c["ok"]]
    cut = [c for c in cycles if c["why"].startswith("cut")]
    res["upload"] = dict(
        cycles=len(cycles), completed_identical=len(ok), failed=len(cycles) - len(ok) - len(cut),
        cut_by_end_of_scenario=len(cut),
        failures=[c["why"] for c in cycles if not c["ok"] and c not in cut][:5],
        seconds=stats([c["seconds"] for c in ok]), resends=sum(c["resends"] for c in cycles),
        commands=sum(c["commands"] for c in cycles), window_s=time.monotonic() - t0)


def scenario_delivery(rig, flood, image, res):
    """Scenario 2: deliveries of a stored image to the fake controller during the flood.

    The image is uploaded as DELIVER_IMAGE before the flood. Each delivery is judged on the fake
    controller's side (the bytes it received, the time from the DELIVER to its UPLOAD END), so a
    ground that does not get the answers is still measured.

    Args:
        rig: the Rig
        flood: the FloodRun
        image: image bytes
        res: result dict to fill

    Returns:
        None.
    """
    ground, fake = rig.ground, rig.fake
    pre = upload_cycle(rig, DELIVER_IMAGE, image, time.monotonic())
    res["pre_upload_ok"] = pre["ok"]
    t0 = time.monotonic()
    flood.start(FLOOD_S + FLOOD_TAIL_S + DELIVER_WAIT_S)
    t_stop = t0 + FLOOD_S
    rows = []
    while time.monotonic() < t_stop:
        ended = len(fake.ended)
        mark = len(fake.frames)
        seq, _, sent = ground.send(B.ONBOARD, struct.pack("<BH", B.DELIVER, DELIVER_IMAGE))
        end = time.monotonic() + DELIVER_WAIT_S
        while len(fake.ended) == ended and time.monotonic() < end:
            time.sleep(0.01)
        ok_end = len(fake.ended) > ended
        with ground.cond:
            ground.cond.wait_for(lambda: ground.final(seq) is not None, timeout=3.0)
        fin = ground.final(seq)
        frames = fake.frames[mark:]
        end_f = [f for f in frames if f["payload"][0] == B.UPLOAD_END]
        rows.append(dict(
            fake_end_s=(end_f[0]["t"] - sent) if end_f else None,
            ground_final_s=(fin[2] - sent) if fin else None,
            ground_kind=(fin[0], fin[1]) if fin else None,
            identical=ok_end and bytes(fake.images.get(DELIVER_IMAGE, b"")) == image))
    good = [r for r in rows if r["identical"]]
    res["delivery"] = dict(
        deliveries=len(rows), identical=len(good),
        fake_end_seconds=stats([r["fake_end_s"] for r in good if r["fake_end_s"] is not None]),
        ground_final_seconds=stats([r["ground_final_s"] for r in rows if r["ground_final_s"]]),
        ground_done=sum(1 for r in rows if r["ground_kind"] == (DONE, 0)),
        stop_and_wait_violations=fake.violations[:3], fake_errors=fake.errors[:3],
        window_s=time.monotonic() - t0)


def scenario_beat(rig, flood, res):
    """Scenario 3: the beat. GET STATUS to the controller every --beat-s s while the link is idle.

    The intervals are taken from the times the fake controller received the frames. They carry
    the fake's own reading delay (a few ms), which is the same with and without the flood.

    Args:
        rig: the Rig
        flood: the FloodRun
        res: result dict to fill

    Returns:
        None.
    """
    t0 = time.monotonic()
    flood.start(FLOOD_S + FLOOD_TAIL_S)
    sleep_until(t0 + FLOOD_S)
    beats = [f["t"] for f in rig.fake.frames
             if f["dest"] == B.CONTROLLER and f["payload"] == bytes([B.GET_STATUS])
             and f["t"] >= t0 + BEAT_WARMUP_S]
    gaps = [(b - a) * 1000.0 for a, b in zip(beats, beats[1:])]
    s = stats(gaps)
    expected_ms = BEAT_S * 1000.0
    res["beat"] = dict(why_beat_s=BEAT_WHY, beat_s=BEAT_S, beats=len(beats), interval_ms=s,
                       error_ms_mean=(s["mean"] - expected_ms) if gaps else None,
                       error_ms_max=(max(abs(g - expected_ms) for g in gaps) if gaps else None),
                       jitter_std_ms=s.get("std"),
                       window_s=time.monotonic() - t0 - BEAT_WARMUP_S)


def scenario_trial(rig, flood, res):
    """Scenario 4: the fake controller reports TRIAL and never confirms; the updater must reset.

    The expected time of the reset is the time the fake wrote its first TRIAL HEALTH plus
    TRIAL_EXPECTED_S. The measured times are the fake restore script's first call and the
    updater's own log line (through the log calibration of Rig.start).

    Args:
        rig: the Rig
        flood: the FloodRun
        res: result dict to fill

    Returns:
        None.
    """
    t0 = time.monotonic()
    flood.start(TRIAL_EXPECTED_S + RESET_WAIT_MAX_S + POST_EVENT_S + 5.0)
    time.sleep(1.5)
    rig.fake.mode = B.TRIAL
    deadline = t0 + 1.5 + 1.2 + TRIAL_EXPECTED_S + RESET_WAIT_MAX_S
    while time.monotonic() < deadline and not rig.restore_calls():
        time.sleep(0.1)
    time.sleep(POST_EVENT_S)
    trial0 = next((w for w, _, m in rig.fake.health_log if m == B.TRIAL), None)
    calls = rig.restore_calls()
    lines = grep_log(rig.log_path, ["trial: no EVENT"])
    res["trial"] = dict(
        expected_s_after_first_trial_health=TRIAL_EXPECTED_S, restore_calls=len(calls),
        script_error_s=(calls[0] - (trial0 + TRIAL_EXPECTED_S)) if calls and trial0 else None,
        log_line_error_s=(rig.wall_of(lines[0][0]) - (trial0 + TRIAL_EXPECTED_S))
        if lines and trial0 else None,
        spawn_delay_s=(calls[0] - rig.wall_of(lines[0][0])) if calls and lines else None,
        events_0106=events_0106(rig.ground), window_s=time.monotonic() - t0)


def events_0106(ground):
    """Lists the EVENT 0x0106 reports the ground received: reason byte and monotonic time.

    EVENT layout: kind at offset 12, code at 13, detail from 15 [protocol.hpp:148-163].

    Args:
        ground: the Ground

    Returns:
        A list of (reason, time) pairs.
    """
    found = []
    for r in list(ground.reports):
        p = r["payload"]
        if len(p) >= 16 and p[12] == 0x02 and struct.unpack_from("<H", p, 13)[0] == 0x0106:
            found.append((p[15], r["t"]))
    return found


def scenario_silence(rig, flood, res):
    """Scenario 5: the fake controller stops sending; the updater resets it on schedule.

    Expected from the last frame the fake wrote (T): "controller silent" and reset 1 at T + 5 s
    [config.hpp:93, controller_watch.cpp:resetDue], reset 2 at T + 65 s, reset 3 at T + 125 s
    [config.hpp:97, 100], the give-up line at T + 185 s, and no fourth reset.

    Args:
        rig: the Rig
        flood: the FloodRun
        res: result dict to fill

    Returns:
        None.
    """
    t0 = time.monotonic()
    flood.start(SILENCE_RUN_S + 10.0)
    time.sleep(2.0)
    rig.fake.health_on = False
    time.sleep(0.3)
    last = rig.fake.health_log[-1][0]
    sleep_until(t0 + 2.3 + SILENCE_RUN_S)
    calls = rig.restore_calls()
    lines = grep_log(rig.log_path, ["controller silent: no frame", "reset 1 of", "reset 2 of",
                                    "reset 3 of", "still silent after"])
    expected = [SILENCE_EXPECTED_S + k * SILENT_GAP_S for k in range(SILENT_RESETS_MAX)]
    starts = [s for s, t in lines if re.search(r"reset \d of \d started", t)]
    since = [rig.wall_of(s) - last for s in starts]
    res["silence"] = dict(
        last_frame_wall=last, expected_s_after_last_frame=expected + [expected[-1] + SILENT_GAP_S],
        restore_calls=len(calls),
        script_error_s=[c - (last + e) for c, e in zip(calls, expected)],
        log_error_s=[a - e for a, e in zip(since, expected)],
        silent_line_error_s=[(rig.wall_of(s) - last) - SILENCE_EXPECTED_S for s, t in lines
                             if t.startswith("controller silent: no frame")],
        spawn_delay_s=[c - rig.wall_of(s) for c, s in zip(calls, starts)],
        log_lines=[(round(rig.wall_of(s) - last, 3), t[:60]) for s, t in lines],
        events_0106=[(r, round(t - rig.fake.health_log[-1][1], 3))
                     for r, t in events_0106(rig.ground)],
        window_s=time.monotonic() - t0)


def write_outputs(args, rig, flood, res, probe, after):
    """Writes the three files of this run: the log, the series and the updater log excerpt.

    The first line of the log is the Mac time, the commit and the exact command (task).

    Args:
        args: the parsed command line
        rig: the Rig
        flood: the FloodRun
        res: the result dict
        probe: the Probe, or None
        after: udp_counters() taken after the flood

    Returns:
        None.
    """
    base = os.path.join(args.out, args.tag)
    header = f"{res['started_text']} | commit {args.commit} | {' '.join(sys.argv)}"
    rows = rig.sampler.rows
    first = rows[0][0] if rows else 0.0
    per_second = (flood.stats or {}).get("per_second", [])
    with open(base + ".series.csv", "w") as f:
        f.write("# " + header + "\n")
        f.write("t_s,rss_kb,cpu_pct,updater_log_bytes,alive,load1,flood_sent_in_second\n")
        for t, rss, cpu, size, alive, load in rows:
            sent = ""
            idx = int(t - flood.started) if flood.started is not None else -1
            if 0 <= idx < len(per_second):
                sent = per_second[idx]
            f.write(f"{t - first:.1f},{rss},{cpu:.1f},{size},{int(alive)},{load:.2f},{sent}\n")
    res["updater"] = dict(
        samples=len(rows),
        rss_kb=dict(first=rows[0][1] if rows else None, last=rows[-1][1] if rows else None,
                    max=max((r[1] for r in rows), default=None)),
        cpu_pct=stats([r[2] for r in rows[1:]]),
        died_at_sample=next((i for i, r in enumerate(rows) if not r[4]), None),
        **res.get("updater_end", {}))
    res.pop("updater_end", None)
    res["updater"]["log_counts"] = {p: count_log(rig.log_path, p) for p in LOG_COUNTS}
    res["updater"]["log"] = log_excerpt(rig.log_path, base + ".updater.txt", LOG_EDGE_LINES,
                                         header)
    res["flood"] = flood.summary(rig.ground, after)
    res["probe"] = probe.result() if probe else None
    res["fake_controller"] = dict(violations=rig.fake.violations[:3], errors=rig.fake.errors[:3])
    res["loadavg_end"] = os.getloadavg()
    if res["updater"].get("died_at_sample") is not None and flood.stats:
        res["last_datagrams_hex"] = flood.stats.get("last_hex", [])
    with open(base + ".log", "w") as f:
        f.write(header + "\n")
        f.write(f"load averages at start {res['loadavg_start']}, at end {res['loadavg_end']}\n")
        f.write(json.dumps(res, indent=1, default=str) + "\n")
    os.remove(rig.log_path)


def main():
    """Runs one scenario against one flood and writes its files.

    Returns:
        Exit status: 0 the run finished (also when a result is bad: that is data), 2 the updater
        did not start.
    """
    global FLOOD_S
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--scenario", required=True,
                        choices=("upload", "delivery", "beat", "trial", "silence"))
    parser.add_argument("--flood", required=True, choices=("none",) + FS.KINDS)
    parser.add_argument("--source", choices=("same", "foreign"), default="same")
    parser.add_argument("--updater", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--tag", default=None)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--flood-s", type=float, default=FLOOD_S,
                        help="flood time of upload, delivery and beat (smoke tests use less)")
    args = parser.parse_args()
    FLOOD_S = args.flood_s
    signal.signal(signal.SIGTERM, on_term)
    suffix = "-foreign" if args.source == "foreign" else ""
    args.tag = args.tag or f"{args.scenario}-{args.flood}{suffix}"
    image = open(args.image, "rb").read()
    os.makedirs(args.out, exist_ok=True)
    res = dict(tag=args.tag, scenario=args.scenario, flood=args.flood, source=args.source,
               started_text=now_text(), loadavg_start=os.getloadavg(), image_bytes=len(image))
    rig = Rig(args, ["--beat-s", str(BEAT_S)] if args.scenario == "beat" else [])
    flood = FloodRun(args.flood, args.source, args.seed, rig)
    probe = None
    after = {}
    try:
        if not rig.start():
            res["error"] = "updater or fake controller did not start"
            return 2
        if args.scenario in ("beat", "trial", "silence"):
            probe = Probe(rig.ground)
            probe.start()
        if args.scenario == "upload":
            scenario_upload(rig, flood, image, res)
        elif args.scenario == "delivery":
            scenario_delivery(rig, flood, image, res)
        elif args.scenario == "beat":
            scenario_beat(rig, flood, res)
        elif args.scenario == "trial":
            scenario_trial(rig, flood, res)
        else:
            scenario_silence(rig, flood, res)
        after = udp_counters()
    finally:
        if probe:
            probe.halt = True
        flood.stop()
        if flood.kind != "none":
            time.sleep(DRAIN_S)
        res["updater_end"] = rig.finish()
        write_outputs(args, rig, flood, res, probe, after)
    return 0


if __name__ == "__main__":
    sys.exit(main())
