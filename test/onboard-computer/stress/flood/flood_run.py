#!/usr/bin/env python3
"""Master of the flood stress audit A.5: runs every scenario against every flood kind.

For each flood kind (none, valid, repeat, garbage, mix) two phases, each a set of rigs
(rig.py) that run at the same time, each rig with its own updater, port and flood:
  phase 1 (about 80 s): upload, delivery and beat;
  phase 2 (about 220 s): trial and silence.
Then the upload and delivery scenarios again with the flood's valid frames sent from a source
address other than the ground session's (the updater sends answers to the source of the most
recent good command, command_gate.hpp). A timer result that is off by more than TIMER_OFF_S is
run once more and both results are kept. The last step writes summary.txt.

Why the rigs of a phase run together: the whole audit then takes about 30 minutes, not 60. The
cost is that one Mac runs up to three floods at once; every log carries the load average, and
the udp counters it records are system-wide.

Usage: flood_run.py --updater PATH --image FILE --out DIR --work DIR
Standard library only.
"""
import argparse
import json
import os
import signal
import subprocess
import sys
import time

sys.dont_write_bytecode = True   # the source tree must stay as it is: no __pycache__ in test/
HERE = os.path.dirname(os.path.abspath(__file__))
KINDS = ("none", "valid", "repeat", "garbage", "mix")
FOREIGN_KINDS = ("valid", "repeat", "mix")
PHASE1 = ("upload", "delivery", "beat")
PHASE2 = ("trial", "silence")
TIMER_OFF_S = 0.5          # a trial or silence timer later or earlier than this is run again
BEAT_OFF_MS = 100.0        # a beat interval mean off by more than this is run again
PHASE1_LIMIT_S = 240.0     # longest a phase-1 rig may run: 65 s flood plus cycles plus stops
PHASE2_LIMIT_S = 420.0     # longest a phase-2 rig may run: 205 s of silence plus stops
KILL_WAIT_S = 30.0         # wait for a rig to clean up after SIGTERM before killing its PID


def launch(args, commit, scenario, flood, source, tag):
    """Starts one rig as a child process.

    Args:
        args: the parsed command line
        commit: short commit hash for the logs
        scenario: scenario name (rig.py --scenario)
        flood: flood kind
        source: "same" or "foreign"
        tag: name of the run, used in file names

    Returns:
        The subprocess.Popen.
    """
    cmd = [sys.executable, os.path.join(HERE, "rig.py"), "--scenario", scenario, "--flood",
           flood, "--source", source, "--updater", args.updater, "--image", args.image,
           "--work", args.work, "--out", args.out, "--commit", commit, "--tag", tag]
    return subprocess.Popen(cmd)


def wait_all(procs, limit_s):
    """Waits for the rigs, with an upper bound; a late rig gets SIGTERM, then a kill.

    Only the exact PIDs this program started are signalled.

    Args:
        procs: list of (tag, Popen)
        limit_s: seconds allowed for the whole set

    Returns:
        A dict tag -> exit status; "timeout" for a rig that had to be stopped.
    """
    end = time.monotonic() + limit_s
    status = {}
    for tag, proc in procs:
        try:
            status[tag] = proc.wait(max(0.0, end - time.monotonic()))
        except subprocess.TimeoutExpired:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(KILL_WAIT_S)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            status[tag] = "timeout"
    return status


def load_result(out, tag):
    """Reads the JSON part of a rig's log (everything after its first two lines).

    Args:
        out: results directory
        tag: name of the run

    Returns:
        The result dict, or None if the file is missing or unreadable.
    """
    try:
        with open(os.path.join(out, tag + ".log")) as f:
            return json.loads(f.read().split("\n", 2)[2])
    except (OSError, ValueError, IndexError):
        return None


def timer_off(res):
    """Says why a timer result is off, or None if it is not.

    Off means: a trial or silence error past TIMER_OFF_S, no reset where one was due, or a beat
    interval mean past BEAT_OFF_MS from the beat period.

    Args:
        res: a result dict

    Returns:
        A short reason, or None.
    """
    if res is None:
        return "no result"
    if "trial" in res:
        t = res["trial"]
        for key in ("script_error_s", "log_line_error_s"):
            if t.get(key) is None:
                return "no reset seen"
            if abs(t[key]) > TIMER_OFF_S:
                return f"trial {key} {t[key]:.3f}"
    if "silence" in res:
        s = res["silence"]
        if s["restore_calls"] != 3:
            return f"{s['restore_calls']} resets"
        if len(s["log_error_s"]) != 3:
            return f"{len(s['log_error_s'])} reset lines"
        for e in s["script_error_s"] + s["log_error_s"] + s["silent_line_error_s"]:
            if abs(e) > TIMER_OFF_S:
                return f"silence error {e:.3f}"
    if "beat" in res:
        b = res["beat"]
        if b.get("error_ms_mean") is None or abs(b["error_ms_mean"]) > BEAT_OFF_MS:
            return f"beat mean error {b.get('error_ms_mean')}"
    return None


def run_set(args, commit, items, limit_s, results):
    """Runs a set of rigs together and collects their results.

    A timer result that is off is run once more, alone, under the tag with "-rerun".

    Args:
        args: the parsed command line
        commit: short commit hash
        items: list of (scenario, flood, source)
        limit_s: seconds allowed for the set
        results: dict tag -> result, filled

    Returns:
        None.
    """
    procs = []
    for scenario, flood, source in items:
        tag = f"{scenario}-{flood}" + ("-foreign" if source == "foreign" else "")
        procs.append((tag, launch(args, commit, scenario, flood, source, tag)))
    status = wait_all(procs, limit_s)
    for scenario, flood, source in items:
        tag = f"{scenario}-{flood}" + ("-foreign" if source == "foreign" else "")
        results[tag] = load_result(args.out, tag)
        print(f"{time.strftime('%H:%M:%S')} {tag}: exit {status[tag]}", flush=True)
        why = timer_off(results[tag]) if scenario in ("beat", "trial", "silence") else None
        if why:
            again = tag + "-rerun"
            print(f"{time.strftime('%H:%M:%S')} {tag} is off ({why}); running it once more",
                  flush=True)
            proc = launch(args, commit, scenario, flood, source, again)
            wait_all([(again, proc)], limit_s)
            results[again] = load_result(args.out, again)
            results[again + "|why"] = why


def fmt(x, digits=3):
    """Formats a number for the summary table, or "-" if there is none.

    Args:
        x: a number or None
        digits: decimals

    Returns:
        Text.
    """
    return "-" if x is None else f"{x:.{digits}f}"


def row(tag, res):
    """Makes the line of one run for the summary: the scenario's numbers, then the common ones.

    Args:
        tag: name of the run
        res: its result dict, or None

    Returns:
        One line of text.
    """
    if res is None:
        return f"{tag:28s} NO RESULT"
    own = ""
    if "upload" in res:
        u = res["upload"]
        own = (f"uploads {u['completed_identical']}/{u['cycles']} identical, "
               f"failed {u['failed']}, cut {u['cut_by_end_of_scenario']}, "
               f"time p50 {fmt(u['seconds'].get('p50'))} max {fmt(u['seconds'].get('max'))} s, "
               f"resends {u['resends']}")
    elif "delivery" in res:
        d = res["delivery"]
        own = (f"deliveries {d['identical']}/{d['deliveries']} identical, ground DONE "
               f"{d['ground_done']}, time p50 {fmt(d['fake_end_seconds'].get('p50'))} max "
               f"{fmt(d['fake_end_seconds'].get('max'))} s")
    elif "beat" in res:
        b = res["beat"]
        i = b["interval_ms"]
        own = (f"beats {b['beats']}, interval mean {fmt(i.get('mean'), 1)} min "
               f"{fmt(i.get('min'), 1)} max {fmt(i.get('max'), 1)} std {fmt(i.get('std'), 1)} ms")
    elif "trial" in res:
        t = res["trial"]
        own = (f"trial: resets {t['restore_calls']}, error script {fmt(t['script_error_s'])} "
               f"log {fmt(t['log_line_error_s'])} spawn {fmt(t['spawn_delay_s'])} s, "
               f"EVENT 0x0106 {len(t['events_0106'])}")
    elif "silence" in res:
        s = res["silence"]
        script = [round(e, 3) for e in s["script_error_s"]]
        logged = [round(e, 3) for e in s["log_error_s"]]
        spawn = [round(e, 3) for e in s["spawn_delay_s"]]
        own = (f"silence: resets {s['restore_calls']}, script errors {script} s, log "
               f"{logged} s, spawn {spawn} s, EVENT 0x0106 {len(s['events_0106'])}")
    f = res.get("flood", {})
    u = res.get("updater", {})
    p = res.get("probe") or {}
    fl = f.get("valid_done_latency_ms", {})
    pl = p.get("latency_ms", {})
    rss = u.get("rss_kb", {})
    cpu = u.get("cpu_pct", {})
    log = u.get("log", {})
    common = (f" | flood {fmt(f.get('rate_reached'), 0)}/s, DONE share "
              f"{fmt(f.get('valid_done_share'), 3)}, lat p50 {fmt(fl.get('p50'), 1)} max "
              f"{fmt(fl.get('max'), 1)} ms | probe share {fmt(p.get('share'), 3)} p50 "
              f"{fmt(pl.get('p50'), 1)} max {fmt(pl.get('max'), 1)} ms | rss {rss.get('first')}"
              f"->{rss.get('max')} kB, cpu mean {fmt(cpu.get('mean'), 0)} max "
              f"{fmt(cpu.get('max'), 0)} % | log {log.get('bytes')} B {log.get('lines')} lines"
              f" | alive_at_stop {u.get('alive_at_stop')} exit {u.get('exit_code')} | load1 "
              f"{fmt(res.get('loadavg_start', [None])[0], 1)}")
    return f"{tag:28s} {own}{common}"


def write_summary(args, commit, results, started):
    """Writes summary.txt: one line per run, in run order, and the reasons for re-runs.

    Args:
        args: the parsed command line
        commit: short commit hash
        results: dict tag -> result
        started: Mac time text of the start

    Returns:
        None.
    """
    with open(os.path.join(args.out, "summary.txt"), "w") as out:
        out.write(f"{started} | commit {commit} | {' '.join(sys.argv)}\n")
        for tag, res in results.items():
            if tag.endswith("|why"):
                out.write(f"  re-run reason for {tag[:-4]}: {res}\n")
            else:
                out.write(row(tag, res) + "\n")


def main():
    """Runs the whole audit.

    Returns:
        Exit status: 0 when all runs finished (a bad result is data, not a failure of this
        program).
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--updater", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--work", required=True)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    os.makedirs(args.work, exist_ok=True)
    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True,
                            text=True, cwd=HERE).stdout.strip()
    started = time.strftime("%Y-%m-%d %H:%M:%S %Z")
    results = {}
    begin = time.monotonic()
    for kind in KINDS:
        run_set(args, commit, [(s, kind, "same") for s in PHASE1], PHASE1_LIMIT_S, results)
        run_set(args, commit, [(s, kind, "same") for s in PHASE2], PHASE2_LIMIT_S, results)
        write_summary(args, commit, results, started)
    for kind in FOREIGN_KINDS:
        run_set(args, commit, [(s, kind, "foreign") for s in PHASE1[:2]], PHASE1_LIMIT_S, results)
        write_summary(args, commit, results, started)
    print(f"all runs done in {time.monotonic() - begin:.0f} s; {args.out}/summary.txt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
