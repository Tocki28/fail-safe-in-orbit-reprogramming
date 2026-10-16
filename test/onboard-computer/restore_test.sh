#!/bin/sh
# restore_test.sh - tests of src/onboard-computer/restore/restore.sh and test/onboard-computer/brick.sh
# with a MOCK OpenOCD (bench only, REQ-101). No hardware, no real OpenOCD.
#
# Question answered: does restore.sh write only images that the updater's check-only mode
# accepted (REQ-118), write the bootloader as its body only, end every run with exactly one
# result line (REQ-060), stay within its time limit, and leave no work folder behind?
# And does brick.sh fail when the flash start is not erased after the mass erase?
#
# The mock OpenOCD (written into a temp dir) records its arguments, keeps the Tcl file it was
# given and a copy of every file named in "flash write_image {FILE} ADDR bin", prints the
# "STEP:" and "STEPDONE:" lines, and by environment variable fails at, or hangs in, a named step
# (MOCK_FAIL_STEP, MOCK_HANG_STEP), or fails or hangs just after it (MOCK_FAIL_AFTER, MOCK_HANG_AFTER).
# Mode reset (decision H, accepted by the owner, 13 Oct 2026): success, a step that fails,
# time-outs and signals before and after the reset, no image needed, no fsir-updater needed.
# The real fsir-updater (host build) and real signed images are used.
#
# Usage: restore_test.sh      env: PY, KEYS, FSIR_UPDATER, SHELLS (default "sh dash"), RESTORE and BRICK
#                             (scripts under test), RUN_BOUND_S (bound of one run, default 30)
# Each check prints PASS or FAIL; the exit status is non-zero if any check failed.
here=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$here/../.." && pwd)
PY=${PY:-$REPO/../.venv/bin/python}
KEYS=${KEYS:-$REPO/../keys-dev}
FSIR_UPDATER=${FSIR_UPDATER:-$REPO/src/onboard-computer/updater/build/host/fsir-updater}
SHELLS=${SHELLS:-"sh dash"}
RESTORE=${RESTORE:-$REPO/src/onboard-computer/restore/restore.sh}
BRICK=${BRICK:-$here/brick.sh}
BL_BIN=$REPO/src/controller/bootloader/build/bootloader.bin
IMG=$REPO/test/controller/build/images
LR_IMG=$IMG/last-resort-v1.img
MAIN_IMG=$IMG/slotA-v1.img
FLIPPED_IMG=$IMG/slotA-v1-flipped.img
WRONGKEY_IMG=$IMG/slotA-v1-wrong-key-a.img

T=$(mktemp -d "${TMPDIR:-/tmp}/restore_test.XXXXXX") || { echo "FAIL setup: mktemp"; exit 1; }
trap 'rm -rf "$T"' EXIT
trap 'exit 1' INT TERM HUP
npass=0; nfail=0; runno=0

# @brief  Print one PASS or FAIL line and count it.
#
# @param $1  1 for pass, anything else for fail
# @param $2  name of the check
# @return 0
verdict() {
    if [ "$1" = 1 ]; then npass=$((npass + 1)); echo "PASS [$SH] $2"; else nfail=$((nfail + 1)); echo "FAIL [$SH] $2"; fi
    return 0
}

# @brief  Count the lines starting with a prefix in a file.
#
# @param $1  file
# @param $2  prefix
# @stdout the count
# @return 0
count_lines() { grep -c "^$2" "$1" || true; }   # grep -c exits 1 for a count of 0: not an error here

# @brief  Run restore.sh once with a private TMPDIR, mock OpenOCD folder and log, under a bound.
#
# Why python: the run is bounded (RUN_BOUND_S; a script that hangs or loops is killed by PID and
# counted as a failure, so no test waits forever), and a signal can be sent while it runs; a
# shell's background job would ignore SIGINT. If SIGNAL is set (INT, TERM, HUP) the signal is
# sent once the mock has saved its PID. Sets R_OUT (stdout file), R_ERR (stderr file), R_RC
# (exit status, -999 if the bound killed it), R_TMP (the private TMPDIR) and R_MOCK (the mock's
# record folder). Environment for the run comes from the caller's exports (MOCK_FAIL_STEP,
# MOCK_HANG_STEP, OPENOCD_TIMEOUT_S, FSIR_UPDATER ...) plus those set here.
#
# @param $@  arguments of restore.sh
# @return 0
run_restore() {
    runno=$((runno + 1))
    R_TMP=${RUN_TMP:-$T/run$runno/tmp}; R_MOCK=$T/run$runno/mock
    mkdir -p "$R_TMP" "$R_MOCK"
    R_OUT=$T/run$runno/out; R_ERR=$T/run$runno/err
    SHELL_UNDER_TEST=$SH RESTORE=$RESTORE R_OUT=$R_OUT R_ERR=$R_ERR R_RC_FILE=$T/run$runno/rc \
        TMPDIR=$R_TMP MOCK_DIR=$R_MOCK OPENOCD=${MOCK_OPENOCD:-$T/mock/openocd} OCD_SCRIPTS=$T/mock/tcl \
        LOG=$T/run$runno/openocd.log "$PY" -I "$T/runner.py" "$@"
    R_RC=$(cat "$T/run$runno/rc" 2>/dev/null || echo -998)
    return 0
}

# @brief  Check the rules that hold for every run: one result line, private TMPDIR empty.
#
# @param $1  name of the case
# @param $2  expected number of lines starting with "RESTORE " (normally 1)
# @return 0
check_common() {
    n=$(grep -c '^RESTORE ' "$R_OUT" || true)
    [ "$n" -eq "$2" ] && verdict 1 "$1: exactly $2 result line" || verdict 0 "$1: $n result lines (expected $2): $(cat "$R_OUT")"
    left=$(ls -A "$R_TMP" | wc -l)
    [ "$left" -eq 0 ] && verdict 1 "$1: work folder removed" || verdict 0 "$1: left in TMPDIR: $(ls -A "$R_TMP")"
    return 0
}

# @brief  Check a refused run: failed line, expected status, mock never called.
#
# @param $1  name of the case
# @param $2  expected exit status
# @return 0
check_refused() {
    check_common "$1" 1
    grep -q '^RESTORE FAILED: ' "$R_OUT" && verdict 1 "$1: RESTORE FAILED line" || verdict 0 "$1: no RESTORE FAILED line: $(cat "$R_OUT")"
    [ "$R_RC" -eq "$2" ] && verdict 1 "$1: exit $2" || verdict 0 "$1: exit $R_RC (expected $2)"
    [ ! -e "$R_MOCK/calls" ] && verdict 1 "$1: OpenOCD never called" || verdict 0 "$1: OpenOCD was called"
    return 0
}

# @brief  Wait, bounded, until a file exists.
#
# @param $1  file
# @param $2  tries, one every 0.2 s
# @return 0 if the file appeared; 1 otherwise
wait_for_file() {
    w=0
    while [ ! -e "$1" ] && [ "$w" -lt "$2" ]; do sleep 0.2; w=$((w + 1)); done
    [ -e "$1" ]
}

# @brief  Check that the process whose PID the mock saved is gone.
#
# @param $1  name of the case
# @param $2  file with the PID
# @return 0
check_mock_dead() {
    if [ ! -s "$2" ]; then verdict 0 "$1: mock never saved its PID"; return 0; fi
    sleep 0.3
    if kill -0 "$(cat "$2")" 2>/dev/null; then verdict 0 "$1: mock process $(cat "$2") still running"; kill -KILL "$(cat "$2")" 2>/dev/null || true
    else verdict 1 "$1: no mock process left behind"; fi
    return 0
}

# @brief  Run restore.sh in mode restore or full against the second mock (tclsh, memory model).
#
# Why: the backup-register step (F-3, REQ-057) must be seen as commands in order and its
# read-back must be able to fail; the first mock never interprets the Tcl. Extra mock variables
# (MOCK_STUCK, MOCK_READ_FORCE, MOCK_FAIL_ON_BKP_WRITE, MOCK_FAIL_STEP) come from the caller.
#
# @param $1  mode, restore or full
# @return 0; sets the same variables as run_restore
bkp_run() {
    MOCK_OPENOCD=$T/mock/openocd-bkp; export MOCK_OPENOCD
    if [ "$1" = full ]; then
        run_restore full --bootloader "$T/in/bl.img" --last-resort "$LR_IMG" --main "$MAIN_IMG"
    else
        run_restore restore --bootloader "$T/in/bl.img" --last-resort "$LR_IMG"
    fi
    unset MOCK_OPENOCD
    return 0
}

# @brief  Check a good run's trace and end state: 16 registers cleared in order, DBP back to 0.
#
# Defects caught: no clear step (the F-3 defect: planted values survive); a register left out
# (a loop of 15, BKP15 or BKP0 skipped); the writes before the flash verify or after the final
# reset; DBP never set (the writes are ignored, so the planted values survive); DBP not cleared
# afterwards; no read-back.
#
# @param $1  name of the case
# @return 0
bkp_check_cleared() {
    res=$(awk -f "$T/mock/trace_check.awk" "$R_MOCK/trace" 2>&1)
    [ "$res" = OK ] && verdict 1 "$1: 16 registers written 0 under DBP, read back, in order" \
        || verdict 0 "$1: trace: $res"
    n0=$(grep -c '^0x00000000$' "$R_MOCK/bkp.final" 2>/dev/null || true)
    [ "$n0" = 16 ] && verdict 1 "$1: all 16 registers hold 0 at the end (planted values gone)" \
        || verdict 0 "$1: registers at the end: $(tr '\n' ' ' < "$R_MOCK/bkp.final" 2>/dev/null)"
    grep -qx 'DBP 0' "$R_MOCK/pwr.final" 2>/dev/null && verdict 1 "$1: DBP is 0 at the end" \
        || verdict 0 "$1: DBP not 0 at the end"
    return 0
}

# @brief  Check a run whose clear step failed: result line, exit 1, no chip release.
#
# Defects caught: a success line after a bad read-back or a failed step; another step name; an
# exit status other than 1; the chip released ("reset run") with a stale record still in the
# backup registers.
#
# @param $1  name of the case
# @return 0
bkp_check_failed() {
    check_common "$1" 1
    grep -q '^RESTORE FAILED: clear backup registers' "$R_OUT" && [ "$R_RC" -eq 1 ] \
        && verdict 1 "$1: RESTORE FAILED: clear backup registers, exit 1" \
        || verdict 0 "$1: rc $R_RC: $(cat "$R_OUT")"
    ! grep -q '^RESET run' "$R_MOCK/trace" 2>/dev/null && verdict 1 "$1: the chip was not released" \
        || verdict 0 "$1: reset run issued after the failed clear"
    return 0
}

# ---- Setup ---------------------------------------------------------------------------
for f in "$PY" "$FSIR_UPDATER" "$RESTORE" "$BRICK"; do
    [ -x "$f" ] || [ -f "$f" ] || { echo "FAIL setup: missing $f"; exit 1; }
done
for f in "$BL_BIN" "$LR_IMG" "$MAIN_IMG" "$FLIPPED_IMG" "$WRONGKEY_IMG" "$KEYS/key-a.pem" "$KEYS/key-b.pem" "$KEYS/key-wrong.pem"; do
    [ -f "$f" ] || { echo "FAIL setup: missing $f (make -C src/controller / test/controller, development keys)"; exit 1; }
done

mkdir -p "$T/mock/tcl" "$T/in"
# The mock OpenOCD. exec sleep keeps the PID of the "hanging OpenOCD" the PID the script saved.
cat > "$T/mock/openocd" <<'EOT'
#!/bin/sh
# mock OpenOCD: records, prints STEP lines, fails or hangs at a named step.
# Like the real OpenOCD 0.12.0 (measured on the MP257): after "init" it has a built-in command
# "step [address]", so a bare "step" line fails with that usage text and exit 1.
echo "$*" >> "$MOCK_DIR/calls"
tcl=; prev=
for a in "$@"; do if [ "$prev" = -f ]; then tcl=$a; fi; prev=$a; done
cp "$tcl" "$MOCK_DIR/steps.tcl"
inited=0
while IFS= read -r line; do
    case $line in
        'step '*)
            if [ "$inited" -eq 1 ]; then echo "gdb_sync"; echo "step [address]"; exit 1; fi
            echo "invalid command name \"step\""; exit 1 ;;
        'fsir_step "'*) ;;
        *) continue ;;
    esac
    name=$(printf '%s\n' "$line" | sed -n 's/^fsir_step "\([^"]*\)".*/\1/p')
    case $line in
        *'flash write_image {'*)
            f=$(printf '%s\n' "$line" | sed -n 's/.*flash write_image {\([^}]*\)} \(0x[0-9A-Fa-f]*\) bin.*/\1/p')
            a=$(printf '%s\n' "$line" | sed -n 's/.*flash write_image {\([^}]*\)} \(0x[0-9A-Fa-f]*\) bin.*/\2/p')
            cp "$f" "$MOCK_DIR/written.$a"
            ;;
        *) ;;
    esac
    echo "STEP: $name"
    if [ "${MOCK_FAIL_STEP:-}" = "$name" ]; then echo "STEPFAIL: $name: mock failure"; exit 1; fi
    if [ "${MOCK_HANG_STEP:-}" = "$name" ]; then echo $$ > "$MOCK_DIR/pid"; exec sleep 100; fi
    echo "STEPDONE: $name"   # as the fsir_step procedure of the Tcl head does on success
    case $line in *'{ init }'*) inited=1 ;; *) ;; esac
    if [ "${MOCK_FAIL_AFTER:-}" = "$name" ]; then echo "mock: OpenOCD exits 1 after the step"; exit 1; fi
    if [ "${MOCK_HANG_AFTER:-}" = "$name" ]; then echo $$ > "$MOCK_DIR/pid"; exec sleep 100; fi
done < "$tcl"
exit 0
EOT
chmod +x "$T/mock/openocd"
cat > "$T/runner.py" <<'EOT'
# Runs the script under test with default signal handling, bounded; optional signal (SIGNAL).
import os, signal, subprocess, sys, time
env = dict(os.environ)
args = [env["SHELL_UNDER_TEST"], env["RESTORE"]] + sys.argv[1:]
bound = float(env.get("RUN_BOUND_S", "30"))
sig = env.get("SIGNAL", "")
with open(env["R_OUT"], "w") as out, open(env["R_ERR"], "w") as err:
    p = subprocess.Popen(args, stdout=out, stderr=err, env=env)
    start = time.time()
    if sig:
        pidfile = os.path.join(env["MOCK_DIR"], "pid")
        while not os.path.exists(pidfile) and time.time() - start < 15:
            time.sleep(0.1)
        time.sleep(0.2)
        p.send_signal(getattr(signal, "SIG" + sig))
    try:
        rc = p.wait(timeout=max(1.0, bound - (time.time() - start)))
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        rc = -999
open(env["R_RC_FILE"], "w").write(str(rc))
EOT

# The second mock OpenOCD (F-3, REQ-057): it RUNS the generated Tcl in tclsh against a memory
# model, so a read-back can return a value and a register can refuse a write. Used only by the
# backup-register cases, through MOCK_OPENOCD (unset: the first mock above, behaviour unchanged).
# Model: TAMP_BKP0R..15R (0x40002500..0x4000253C) start planted with non-zero values (fixture
# values, no claim about the chip); a write to them is accepted only while PWR_CR1 (0x40007000)
# bit 8 DBP is 1 (the F-3 contract; the datasheet locator is not checked here, TBD). Every
# access is traced in order to $MOCK_DIR/trace; final contents go to bkp.final and pwr.final.
# Environment: MOCK_STUCK "10 3" (those registers ignore writes), MOCK_READ_FORCE "10=0x1" (the
# read of that register returns that value), MOCK_FAIL_ON_BKP_WRITE n (a write to BKPnR raises
# a Tcl error, as an SWD fault would).
MOCK_PRELUDE_DIR=$T/mock; export MOCK_PRELUDE_DIR
cat > "$T/mock/prelude-restore.tcl" <<'EOT'
proc tr {s} { set f [open $::env(MOCK_DIR)/trace a]; puts $f $s; close $f }
proc env_or {n d} { if {[info exists ::env($n)]} { return $::env($n) }; return $d }
set ::B0 [expr {0x40002500}]
set ::PWR [expr {0x40007000}]
set ::stuck [env_or MOCK_STUCK {}]
set ::force [env_or MOCK_READ_FORCE {}]
set ::failw [env_or MOCK_FAIL_ON_BKP_WRITE -1]
array set ::mem {}
set ::mem($::PWR) 0
for {set i 0} {$i < 16} {incr i} { set ::mem([expr {$::B0 + 4 * $i}]) [expr {0xA5A50000 + $i}] }
proc bkp_index {a} {
    if {$a >= $::B0 && $a <= $::B0 + 60 && ($a - $::B0) % 4 == 0} { return [expr {($a - $::B0) / 4}] }
    return -1
}
proc rd {a} {
    set a [expr {$a}]
    set i [bkp_index $a]
    if {$i >= 0} {
        tr "BKPR $i"
        foreach p $::force { lassign [split $p =] fi fv; if {$fi == $i} { return [expr {$fv}] } }
        return $::mem($a)
    }
    if {$a == $::PWR} { tr "PWRR" } elseif {$a >= 0x40002400 && $a < 0x40002800} { tr "TAMPR" }
    if {[info exists ::mem($a)]} { return $::mem($a) }
    return 0
}
proc wr {a v} {
    set a [expr {$a}]; set v [expr {$v & 0xFFFFFFFF}]
    set i [bkp_index $a]
    if {$i >= 0} {
        if {$i == $::failw} { tr "BKPW $i fault"; error "mock SWD fault writing TAMP_BKP${i}R" }
        set dbp [expr {($::mem($::PWR) >> 8) & 1}]
        set ok [expr {$dbp && [lsearch -exact $::stuck $i] < 0}]
        tr [format "BKPW %d 0x%08X %d %d" $i $v $dbp $ok]
        if {$ok} { set ::mem($a) $v }
        return
    }
    if {$a == $::PWR} {
        set old [expr {($::mem($a) >> 8) & 1}]; set ::mem($a) $v; set new [expr {($v >> 8) & 1}]
        tr [format "PWRW 0x%08X" $v]
        if {$new != $old} { tr "DBP $new" }
        return
    }
    if {$a >= 0x40002400 && $a < 0x40002800} { tr "TAMPW" }
    set ::mem($a) $v
}
proc mww {a v {n 1}} { for {set k 0} {$k < $n} {incr k} { wr [expr {$a + 4 * $k}] $v } }
proc mmw {a s c} { wr $a [expr {([rd $a] & ~$c) | $s}] }
proc mdw {a {n 1}} { for {set k 0} {$k < $n} {incr k} { puts [format "0x%08x: %08x" [expr {$a + 4 * $k}] [rd [expr {$a + 4 * $k}]]] } }
proc read_memory {a w n} { set l {}; for {set k 0} {$k < $n} {incr k} { lappend l [rd [expr {$a + 4 * $k}]] }; return $l }
proc memread32 {a} { return [rd $a] }
proc write_memory {a w l} { set k 0; foreach v $l { wr [expr {$a + 4 * $k}] $v; incr k } }
proc mem2array {name w a n} { upvar 1 $name arr; for {set k 0} {$k < $n} {incr k} { set arr($k) [rd [expr {$a + 4 * $k}]] } }
proc init {} { proc step {args} { puts "step \[address\]"; exit 1 } }
proc reset {args} { tr "RESET $args" }
proc flash {args} { tr "FLASH $args" }
proc dump {} {
    set f [open $::env(MOCK_DIR)/bkp.final w]
    for {set i 0} {$i < 16} {incr i} { puts $f [format "0x%08X" $::mem([expr {$::B0 + 4 * $i}])] }
    close $f
    set f [open $::env(MOCK_DIR)/pwr.final w]; puts $f "DBP [expr {($::mem($::PWR) >> 8) & 1}]"; close $f
}
proc shutdown {args} { dump; exit [expr {[llength $args] ? 1 : 0}] }
source [lindex $argv 0]
dump
EOT
cat > "$T/mock/openocd-bkp" <<'EOT'
#!/bin/sh
# mock OpenOCD 2: runs the generated Tcl file in tclsh with prelude-restore.tcl.
echo "$*" >> "$MOCK_DIR/calls"
tcl=; prev=
for a in "$@"; do if [ "$prev" = -f ]; then tcl=$a; fi; prev=$a; done
cp "$tcl" "$MOCK_DIR/steps.tcl"
exec tclsh "$MOCK_PRELUDE_DIR/prelude-restore.tcl" "$tcl"
EOT
chmod +x "$T/mock/openocd-bkp"
# Reads a trace and prints OK, or the first rule it breaks: every register written 0 with DBP
# set, all after the last FLASH command and before the last RESET, all read back after the
# writes, DBP cleared after the last read-back and before the last RESET.
cat > "$T/mock/trace_check.awk" <<'EOT'
{ n++ }
/^FLASH /  { lastflash = n }
/^RESET /  { lastreset = n }
/^DBP 1$/  { if (!dbp1) dbp1 = n }
/^DBP 0$/  { lastdbp0 = n }
/^BKPW [0-9]+ 0x/ {
    if ($3 == "0x00000000" && $5 == 1) { w[$2] = n; if (!firstw) firstw = n; lastw = n }
    else { bad = bad " bad-write(reg " $2 " value " $3 " dbp " $4 " accepted " $5 ")" }
}
/^BKPR / { r[$2] = n; lastr = n }
END {
    if (!lastflash) { print "no FLASH command traced"; exit }
    if (!lastreset) { print "no RESET traced"; exit }
    for (i = 0; i < 16; i++) {
        if (!(i in w)) { print "register " i ": no write of 0 with DBP set" bad; exit }
        if (!(i in r)) { print "register " i ": no read-back"; exit }
        if (w[i] < lastflash) { print "register " i ": cleared before the last flash command"; exit }
        if (r[i] < w[i]) { print "register " i ": read back before its write"; exit }
    }
    if (bad) { print "unexpected writes:" bad; exit }
    if (!dbp1 || dbp1 > firstw) { print "DBP not set before the first register write"; exit }
    if (!lastdbp0 || lastdbp0 < lastr) { print "DBP not cleared after the last read-back"; exit }
    if (lastw > lastreset || lastdbp0 > lastreset) { print "clear step runs after the final reset"; exit }
    print "OK"
}
EOT

# Signed bootloader images from tools/make_image.py: good, signed with a wrong key A.
"$PY" "$REPO/tools/make_image.py" --bin "$BL_BIN" --kind bootloader --slot bootloader --version 1 \
    --key-a "$KEYS/key-a.pem" --key-b "$KEYS/key-b.pem" --build-id "restore_test" --out "$T/in/bl.img" > /dev/null \
    || { echo "FAIL setup: make_image.py (good bootloader image)"; exit 1; }
"$PY" "$REPO/tools/make_image.py" --bin "$BL_BIN" --kind bootloader --slot bootloader --version 1 \
    --key-a "$KEYS/key-wrong.pem" --key-b "$KEYS/key-b.pem" --build-id "restore_test" --out "$T/in/bl-wrong-key.img" > /dev/null \
    || { echo "FAIL setup: make_image.py (wrong-key bootloader image)"; exit 1; }
head -c 192 "$LR_IMG" > "$T/in/size192.img"
head -c 100 "$LR_IMG" > "$T/in/size100.img"
: > "$T/in/size0.img"
mkdir -p "$T/in/dir with space" "$T/in/br[ack]et"
cp "$LR_IMG" "$T/in/dir with space/lr.img"
cp "$LR_IMG" "$T/in/br[ack]et/lr.img"

# ---- Cases, once for every shell in SHELLS ------------------------------------------
for SH in $SHELLS; do
    command -v "$SH" > /dev/null 2>&1 || { echo "NOTE shell $SH not installed: not run"; continue; }
    GOOD_UPDATER=$FSIR_UPDATER; export FSIR_UPDATER
    unset MOCK_FAIL_STEP MOCK_HANG_STEP MOCK_FAIL_AFTER MOCK_HANG_AFTER OPENOCD_TIMEOUT_S

    # Syntax of the scripts under test.
    "$SH" -n "$RESTORE" && "$SH" -n "$BRICK" && verdict 1 "$SH -n restore.sh brick.sh" || verdict 0 "$SH -n restore.sh brick.sh"

    # Good runs.
    run_restore restore --bootloader "$T/in/bl.img" --last-resort "$LR_IMG"
    check_common "good restore" 1
    grep -q '^RESTORE DONE: restore' "$R_OUT" && [ "$R_RC" -eq 0 ] && verdict 1 "good restore: RESTORE DONE, exit 0" || verdict 0 "good restore: rc $R_RC: $(cat "$R_OUT")"
    [ -e "$R_MOCK/calls" ] && verdict 1 "good restore: mock OpenOCD received the Tcl" || verdict 0 "good restore: mock not called"
    cmp -s "$R_MOCK/written.0x08000000" "$BL_BIN" && verdict 1 "good restore: bootloader written is the body only (== bootloader.bin)" || verdict 0 "good restore: bootloader written differs from bootloader.bin"
    cmp -s "$R_MOCK/written.0x0801A000" "$LR_IMG" && verdict 1 "good restore: last-resort written whole" || verdict 0 "good restore: last-resort written differs"
    [ ! -e "$R_MOCK/written.0x08006000" ] && verdict 1 "good restore: no main image written" || verdict 0 "good restore: main image written in mode restore"
    grep -q 'write_image {' "$R_MOCK/steps.tcl" && ! grep 'write_image' "$R_MOCK/steps.tcl" | grep -qv 'write_image {' \
        && verdict 1 "good restore: every path in the Tcl is in braces" || verdict 0 "good restore: unbraced path in the Tcl"
    [ "$(grep -v '^[[:space:]]*$' "$R_MOCK/steps.tcl" | tail -n 1)" = shutdown ] && verdict 1 "good restore: Tcl ends with shutdown" || verdict 0 "good restore: Tcl does not end with shutdown"

    # OpenOCD's own "step [address]" replaces a Tcl procedure named step once init has run (measured
    # on the MP257, OpenOCD 0.12.0); the generated file must therefore never call a bare step.
    ! grep -Eq '^[[:space:]]*step([[:space:]]|$)' "$R_MOCK/steps.tcl" && grep -q '^fsir_step ' "$R_MOCK/steps.tcl" \
        && verdict 1 "good restore: the Tcl never calls a bare step (OpenOCD's built-in after init)" || verdict 0 "good restore: bare step in the Tcl"

    run_restore full --bootloader "$T/in/bl.img" --last-resort "$LR_IMG" --main "$MAIN_IMG"
    check_common "good full" 1
    grep -q '^RESTORE DONE: full' "$R_OUT" && [ "$R_RC" -eq 0 ] && verdict 1 "good full: RESTORE DONE, exit 0" || verdict 0 "good full: rc $R_RC: $(cat "$R_OUT")"
    cmp -s "$R_MOCK/written.0x08000000" "$BL_BIN" && verdict 1 "good full: bootloader written is the body only" || verdict 0 "good full: bootloader written differs"
    cmp -s "$R_MOCK/written.0x0801A000" "$LR_IMG" && verdict 1 "good full: last-resort written whole" || verdict 0 "good full: last-resort differs"
    cmp -s "$R_MOCK/written.0x08006000" "$MAIN_IMG" && verdict 1 "good full: main written whole" || verdict 0 "good full: main differs"

    # A step that fails in OpenOCD.
    MOCK_FAIL_STEP="verify bootloader"; export MOCK_FAIL_STEP
    run_restore restore --bootloader "$T/in/bl.img" --last-resort "$LR_IMG"
    unset MOCK_FAIL_STEP
    check_common "step failure" 1
    grep -q '^RESTORE FAILED: verify bootloader' "$R_OUT" && [ "$R_RC" -eq 1 ] && verdict 1 "step failure: names the step, exit 1" || verdict 0 "step failure: rc $R_RC: $(cat "$R_OUT")"

    # Unsigned bootloader: the raw .bin.
    run_restore restore --bootloader "$BL_BIN" --last-resort "$LR_IMG"
    check_refused "unsigned bootloader (raw .bin)" 1

    # Wrong signature.
    run_restore full --bootloader "$T/in/bl.img" --last-resort "$LR_IMG" --main "$WRONGKEY_IMG"
    check_refused "main signed with a wrong key" 1
    run_restore restore --bootloader "$T/in/bl-wrong-key.img" --last-resort "$LR_IMG"
    check_refused "bootloader signed with a wrong key" 1

    # Damaged image, wrong kind.
    run_restore full --bootloader "$T/in/bl.img" --last-resort "$LR_IMG" --main "$FLIPPED_IMG"
    check_refused "damaged main (flipped byte)" 1
    run_restore full --bootloader "$T/in/bl.img" --last-resort "$LR_IMG" --main "$LR_IMG"
    check_refused "wrong kind (last-resort given as main)" 1
    run_restore restore --bootloader "$MAIN_IMG" --last-resort "$LR_IMG"
    check_refused "wrong kind (main given as bootloader)" 1

    # Images of 192 bytes or less.
    run_restore restore --bootloader "$T/in/bl.img" --last-resort "$T/in/size192.img"
    check_refused "192-byte file" 1
    run_restore restore --bootloader "$T/in/bl.img" --last-resort "$T/in/size100.img"
    check_refused "100-byte file" 1
    run_restore restore --bootloader "$T/in/bl.img" --last-resort "$T/in/size0.img"
    check_refused "empty file" 1

    # Paths with Tcl syntax characters.
    run_restore restore --bootloader "$T/in/bl.img" --last-resort "$T/in/dir with space/lr.img"
    check_refused "path with a space" 2
    run_restore restore --bootloader "$T/in/bl.img" --last-resort "$T/in/br[ack]et/lr.img"
    check_refused "path with brackets" 2
    RUN_TMP="$T/tmp dir"
    run_restore restore --bootloader "$T/in/bl.img" --last-resort "$LR_IMG"
    RUN_TMP=
    check_refused "TMPDIR with a space (work folder path)" 2

    # Missing argument values.
    run_restore full --bootloader "$T/in/bl.img" --last-resort "$LR_IMG" --main
    check_refused "--main without a value" 2
    grep -q '^RESTORE FAILED: usage' "$R_OUT" && verdict 1 "--main without a value: usage failure" || verdict 0 "--main without a value: $(cat "$R_OUT")"
    if grep -qi 'shift' "$R_ERR"; then verdict 0 "--main without a value: shell error: $(cat "$R_ERR")"; else verdict 1 "--main without a value: no shell error"; fi
    run_restore restore --bootloader
    check_refused "--bootloader without a value" 2
    run_restore restore --bootloader "$T/in/bl.img"
    check_refused "missing --last-resort" 2
    run_restore bogus --bootloader "$T/in/bl.img" --last-resort "$LR_IMG"
    check_refused "unknown mode" 2

    # Updater missing.
    FSIR_UPDATER=$T/does-not-exist   # (assignments before a function call may persist in POSIX sh: set and restore explicitly)
    run_restore restore --bootloader "$T/in/bl.img" --last-resort "$LR_IMG"
    FSIR_UPDATER=$GOOD_UPDATER; export FSIR_UPDATER
    check_refused "fsir-updater missing" 2
    grep -q '^RESTORE FAILED: check' "$R_OUT" && verdict 1 "fsir-updater missing: step is check" || verdict 0 "fsir-updater missing: $(cat "$R_OUT")"

    # Time-out: the mock hangs in "write bootloader"; limit 2 s.
    t0=$(date +%s)
    OPENOCD_TIMEOUT_S=2; MOCK_HANG_STEP="write bootloader"; export OPENOCD_TIMEOUT_S MOCK_HANG_STEP
    run_restore restore --bootloader "$T/in/bl.img" --last-resort "$LR_IMG"
    unset OPENOCD_TIMEOUT_S MOCK_HANG_STEP
    t1=$(date +%s)
    check_common "time-out" 1
    grep -q '^RESTORE FAILED: write bootloader (time-out after 2 s' "$R_OUT" && [ "$R_RC" -eq 1 ] \
        && verdict 1 "time-out: names the step and the limit, exit 1" || verdict 0 "time-out: rc $R_RC: $(cat "$R_OUT")"
    [ $((t1 - t0)) -le 10 ] && verdict 1 "time-out: ended in $((t1 - t0)) s" || verdict 0 "time-out: took $((t1 - t0)) s"
    check_mock_dead "time-out" "$R_MOCK/pid"

    # Signals while the mock hangs.
    for sig in INT TERM HUP; do
        MOCK_HANG_STEP="erase slot A"; export MOCK_HANG_STEP
        SIGNAL=$sig; export SIGNAL
        run_restore restore --bootloader "$T/in/bl.img" --last-resort "$LR_IMG"
        unset MOCK_HANG_STEP SIGNAL
        check_common "SIG$sig" 1
        grep -q '^RESTORE FAILED: erase slot A (interrupted by SIG'"$sig"')' "$R_OUT" && [ "$R_RC" -eq 1 ] \
            && verdict 1 "SIG$sig: names the step, exit 1" || verdict 0 "SIG$sig: rc $R_RC: $(cat "$R_OUT")"
        check_mock_dead "SIG$sig" "$R_MOCK/pid"
    done

    # Mode reset (decision H, accepted by the owner, 13 Oct 2026): restart the controller, write nothing.
    run_restore reset
    check_common "good reset" 1
    [ "$(cat "$R_OUT")" = "RESTORE DONE: reset" ] && [ "$R_RC" -eq 0 ] && verdict 1 "good reset: the line is exactly RESTORE DONE: reset, exit 0" || verdict 0 "good reset: rc $R_RC: $(cat "$R_OUT")"
    [ "$(grep -c '^fsir_step ' "$R_MOCK/steps.tcl")" -eq 2 ] && grep -q '^fsir_step "connect" { init }$' "$R_MOCK/steps.tcl" \
        && grep -q '^fsir_step "reset and run" { reset run }$' "$R_MOCK/steps.tcl" \
        && [ "$(grep -v '^[[:space:]]*$' "$R_MOCK/steps.tcl" | tail -n 1)" = shutdown ] \
        && verdict 1 "good reset: the Tcl is init, reset run, shutdown, and nothing else" || verdict 0 "good reset: unexpected Tcl: $(cat "$R_MOCK/steps.tcl")"
    ! grep -q 'flash' "$R_MOCK/steps.tcl" && ! ls "$R_MOCK"/written.* > /dev/null 2>&1 && verdict 1 "good reset: nothing erased or written" || verdict 0 "good reset: flash command or written file"
    grep -q 'reset_config srst_only srst_nogate connect_assert_srst' "$R_MOCK/calls" && verdict 1 "good reset: same reset configuration as restore" || verdict 0 "good reset: reset_config missing: $(cat "$R_MOCK/calls")"
    FSIR_UPDATER=$T/does-not-exist
    run_restore reset
    FSIR_UPDATER=$GOOD_UPDATER; export FSIR_UPDATER
    grep -q '^RESTORE DONE: reset$' "$R_OUT" && [ "$R_RC" -eq 0 ] && verdict 1 "good reset: needs no fsir-updater" || verdict 0 "good reset without fsir-updater: rc $R_RC: $(cat "$R_OUT")"
    run_restore reset --bootloader "$T/in/bl.img"
    check_refused "reset with an image" 2
    grep -q '^RESTORE FAILED: usage (mode reset takes no images)' "$R_OUT" && verdict 1 "reset with an image: usage failure" || verdict 0 "reset with an image: $(cat "$R_OUT")"
    MOCK_FAIL_STEP="reset and run"; export MOCK_FAIL_STEP
    run_restore reset
    unset MOCK_FAIL_STEP
    check_common "reset step failure" 1
    grep -q '^RESTORE FAILED: reset and run' "$R_OUT" && [ "$R_RC" -eq 1 ] && verdict 1 "reset step failure: names the step, exit 1" || verdict 0 "reset step failure: rc $R_RC: $(cat "$R_OUT")"
    MOCK_FAIL_AFTER="reset and run"; export MOCK_FAIL_AFTER
    run_restore reset
    unset MOCK_FAIL_AFTER
    check_common "reset then OpenOCD fails" 1
    grep -q '^RESTORE FAILED: after reset' "$R_OUT" && [ "$R_RC" -eq 1 ] && verdict 1 "reset then OpenOCD fails: RESTORE FAILED: after reset, exit 1" || verdict 0 "reset then OpenOCD fails: rc $R_RC: $(cat "$R_OUT")"

    # Reset time-out: before the reset (hang in connect) and after it (hang after the reset step); limit 2 s.
    t0=$(date +%s)
    OPENOCD_TIMEOUT_S=2; MOCK_HANG_STEP="connect"; export OPENOCD_TIMEOUT_S MOCK_HANG_STEP
    run_restore reset
    unset OPENOCD_TIMEOUT_S MOCK_HANG_STEP
    t1=$(date +%s)
    check_common "reset time-out before" 1
    grep -q '^RESTORE FAILED: connect (time-out after 2 s' "$R_OUT" && [ "$R_RC" -eq 1 ] \
        && verdict 1 "reset time-out before the reset: names connect and the limit, exit 1" || verdict 0 "reset time-out before: rc $R_RC: $(cat "$R_OUT")"
    [ $((t1 - t0)) -le 10 ] && verdict 1 "reset time-out before: ended in $((t1 - t0)) s" || verdict 0 "reset time-out before: took $((t1 - t0)) s"
    check_mock_dead "reset time-out before" "$R_MOCK/pid"
    OPENOCD_TIMEOUT_S=2; MOCK_HANG_AFTER="reset and run"; export OPENOCD_TIMEOUT_S MOCK_HANG_AFTER
    run_restore reset
    unset OPENOCD_TIMEOUT_S MOCK_HANG_AFTER
    check_common "reset time-out after" 1
    grep -q '^RESTORE FAILED: after reset (time-out after 2 s' "$R_OUT" && [ "$R_RC" -eq 1 ] \
        && verdict 1 "reset time-out after the reset: RESTORE FAILED: after reset, exit 1" || verdict 0 "reset time-out after: rc $R_RC: $(cat "$R_OUT")"
    check_mock_dead "reset time-out after" "$R_MOCK/pid"

    # Reset signals, before and after the reset step.
    for sig in INT TERM HUP; do
        MOCK_HANG_STEP="connect"; export MOCK_HANG_STEP
        SIGNAL=$sig; export SIGNAL
        run_restore reset
        unset MOCK_HANG_STEP SIGNAL
        check_common "reset SIG$sig" 1
        grep -q '^RESTORE FAILED: connect (interrupted by SIG'"$sig"')' "$R_OUT" && [ "$R_RC" -eq 1 ] \
            && verdict 1 "reset SIG$sig before the reset: names connect, exit 1" || verdict 0 "reset SIG$sig: rc $R_RC: $(cat "$R_OUT")"
        check_mock_dead "reset SIG$sig" "$R_MOCK/pid"
        MOCK_HANG_AFTER="reset and run"; export MOCK_HANG_AFTER
        SIGNAL=$sig; export SIGNAL
        run_restore reset
        unset MOCK_HANG_AFTER SIGNAL
        check_common "reset SIG$sig after" 1
        grep -q '^RESTORE FAILED: after reset (interrupted by SIG'"$sig"')' "$R_OUT" && [ "$R_RC" -eq 1 ] \
            && verdict 1 "reset SIG$sig after the reset: RESTORE FAILED: after reset, exit 1" || verdict 0 "reset SIG$sig after: rc $R_RC: $(cat "$R_OUT")"
        check_mock_dead "reset SIG$sig after" "$R_MOCK/pid"
    done

    # F-3 (REQ-057): modes restore and full clear TAMP_BKP0R..15R after the flash verify, before
    # the final reset. The second mock runs the Tcl in tclsh.
    if ! command -v tclsh > /dev/null 2>&1; then verdict 0 "F-3: tclsh not installed (the mock needs it)"; else
    unset MOCK_STUCK MOCK_READ_FORCE MOCK_FAIL_ON_BKP_WRITE
    for m in full restore; do
        bkp_run "$m"
        check_common "F-3 good $m" 1
        grep -q "^RESTORE DONE: $m" "$R_OUT" && [ "$R_RC" -eq 0 ] \
            && verdict 1 "F-3 good $m: RESTORE DONE, exit 0" || verdict 0 "F-3 good $m: rc $R_RC: $(cat "$R_OUT")"
        bkp_check_cleared "F-3 good $m"
    done
    # A read-back that is not 0: first, middle, last register; the write itself is accepted.
    for i in 0 10 15; do
        MOCK_READ_FORCE="$i=0x1"; export MOCK_READ_FORCE
        bkp_run full
        unset MOCK_READ_FORCE
        bkp_check_failed "F-3 read-back of BKP$i is not 0 (full)"
    done
    MOCK_READ_FORCE="10=0x1"; export MOCK_READ_FORCE
    bkp_run restore
    unset MOCK_READ_FORCE
    bkp_check_failed "F-3 read-back of BKP10 is not 0 (restore)"
    # A register that ignores the write (the planted value stays): the script's own read-back must see it.
    for i in 0 15; do
        MOCK_STUCK="$i"; export MOCK_STUCK
        bkp_run full
        unset MOCK_STUCK
        bkp_check_failed "F-3 BKP$i ignores the write (full)"
    done
    # The step itself fails: an SWD fault during a register write, and a failure of the step by name.
    MOCK_FAIL_ON_BKP_WRITE=7; export MOCK_FAIL_ON_BKP_WRITE
    bkp_run restore
    unset MOCK_FAIL_ON_BKP_WRITE
    bkp_check_failed "F-3 write fault on BKP7 (restore)"
    MOCK_FAIL_STEP="clear backup registers"; export MOCK_FAIL_STEP
    run_restore full --bootloader "$T/in/bl.img" --last-resort "$LR_IMG" --main "$MAIN_IMG"
    unset MOCK_FAIL_STEP
    check_common "F-3 step fails (first mock)" 1
    grep -q '^RESTORE FAILED: clear backup registers' "$R_OUT" && [ "$R_RC" -eq 1 ] \
        && verdict 1 "F-3 step fails (first mock): RESTORE FAILED: clear backup registers, exit 1" \
        || verdict 0 "F-3 step fails (first mock): rc $R_RC: $(cat "$R_OUT")"
    # Mode reset writes nothing: no backup-register or PWR_CR1 access, and no such address in the Tcl.
    MOCK_OPENOCD=$T/mock/openocd-bkp; export MOCK_OPENOCD
    run_restore reset
    unset MOCK_OPENOCD
    check_common "F-3 reset" 1
    [ "$(cat "$R_OUT")" = "RESTORE DONE: reset" ] && [ "$R_RC" -eq 0 ] \
        && verdict 1 "F-3 reset: RESTORE DONE: reset, exit 0" || verdict 0 "F-3 reset: rc $R_RC: $(cat "$R_OUT")"
    ! grep -Eq '^(BKP|PWR|DBP|TAMP)' "$R_MOCK/trace" && grep -q '^RESET run' "$R_MOCK/trace" \
        && verdict 1 "F-3 reset: no write or read of 0x400025xx or 0x40007000 (reset run traced)" \
        || verdict 0 "F-3 reset: memory access traced: $(cat "$R_MOCK/trace" 2>/dev/null)"
    ! grep -Eiq '0x4000(25|7000)|mww|mmw|write_memory' "$R_MOCK/steps.tcl" \
        && verdict 1 "F-3 reset: no such address or memory command in the Tcl" \
        || verdict 0 "F-3 reset: backup-register command in the Tcl"
    fi

    # brick.sh: the check after the mass erase. The mock interprets the Tcl with tclsh.
    command -v tclsh > /dev/null 2>&1 || { verdict 0 "brick: tclsh not installed (the mock needs it)"; continue; }
    cat > "$T/mock/prelude.tcl" <<'EOT'
# Minimal OpenOCD for brick.sh: the commands brick.sh uses, read_memory driven by MOCK_WORD.
# init defines step as the real OpenOCD 0.12.0 does (measured on the MP257), replacing any procedure
# of that name; brick.sh must not depend on one.
proc init {} { proc step {args} { puts "step \[address\]"; exit 1 } }
proc reset {args} {}
proc stm32l4x {args} {}
proc mdw {args} { puts "mdw (not a check: it never fails)" }
proc read_memory {addr width count} { return [list $::env(MOCK_WORD)] }
proc shutdown {args} { exit [expr {[llength $args] ? 1 : 0}] }
source [lindex $argv 0]
EOT
    cat > "$T/mock/openocd-tcl" <<'EOT'
#!/bin/sh
tcl=; prev=
for a in "$@"; do if [ "$prev" = -f ]; then tcl=$a; fi; prev=$a; done
exec tclsh "$MOCK_DIR/prelude.tcl" "$tcl"
EOT
    chmod +x "$T/mock/openocd-tcl"
    ! grep -Eq '^[[:space:]]*step([[:space:]]|$)' "$BRICK" && grep -q '^fsir_step "mass erase"' "$BRICK" \
        && verdict 1 "brick: the Tcl never calls a bare step (OpenOCD's built-in after init)" || verdict 0 "brick: bare step in the Tcl"
    for word in 305419896 0; do
        runno=$((runno + 1)); R_TMP=$T/run$runno/tmp; mkdir -p "$R_TMP"
        TMPDIR=$R_TMP MOCK_DIR=$T/mock MOCK_WORD=$word OPENOCD=$T/mock/openocd-tcl OCD_SCRIPTS=$T/mock/tcl \
            LOG=$T/run$runno/log "$SH" "$BRICK" > "$T/run$runno/out" 2> "$T/run$runno/err"
        rc=$?
        grep -q '^BRICK FAILED: check flash start is erased' "$T/run$runno/out" && [ "$rc" -eq 1 ] \
            && verdict 1 "brick word $word: BRICK FAILED: check flash start is erased" || verdict 0 "brick word $word: rc $rc: $(cat "$T/run$runno/out")"
        [ "$(ls -A "$R_TMP" | wc -l)" -eq 0 ] && verdict 1 "brick word $word: temp file removed" || verdict 0 "brick word $word: temp file left"
    done
    for word in 4294967295 0xffffffff; do
        runno=$((runno + 1)); R_TMP=$T/run$runno/tmp; mkdir -p "$R_TMP"
        TMPDIR=$R_TMP MOCK_DIR=$T/mock MOCK_WORD=$word OPENOCD=$T/mock/openocd-tcl OCD_SCRIPTS=$T/mock/tcl \
            LOG=$T/run$runno/log "$SH" "$BRICK" > "$T/run$runno/out" 2> "$T/run$runno/err"
        rc=$?
        grep -q '^BRICK DONE' "$T/run$runno/out" && [ "$rc" -eq 0 ] \
            && verdict 1 "brick word $word: BRICK DONE" || verdict 0 "brick word $word: rc $rc: $(cat "$T/run$runno/out")"
        [ "$(ls -A "$R_TMP" | wc -l)" -eq 0 ] && verdict 1 "brick word $word: temp file removed" || verdict 0 "brick word $word: temp file left"
    done
done

echo "restore_test: $npass PASS, $nfail FAIL"
[ "$nfail" -eq 0 ]
