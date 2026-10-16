#!/bin/sh
# restore.sh - rewrite the controller (NUCLEO-G431KB, STM32G431KB) over SWD (reprogramming
# link A, Interfaces Sec. 6.4) through the Nucleo's on-board ST-LINK, driven by OpenOCD on
# the onboard computer (STM32MP257F-DK).
#
# BENCH STAND-IN (accepted): on the final design the MP257's own pins drive SWD. Here the
# ST-LINK on the Nucleo does, plugged into the MP257's USB. Nothing else differs for the
# controller: it sees SWD either way.
#
#   restore.sh restore --bootloader BL.img --last-resort LR.img
#   restore.sh full    --bootloader BL.img --last-resort LR.img --main FW.img
#   restore.sh reset
#
# Modes restore and full also clear the 16 TAMP backup registers (REQ-057; step "clear backup
# registers", after the flash is written and verified, before the final reset and run): the data
# the bootloader and the firmware keep there across a reset (src/controller/common/backup_layout.h)
# belongs to the firmware that was on the chip, and a restored chip starts with none. Mode reset
# does not clear them.
#
# Mode reset (decision H, accepted by the owner, 13 Oct 2026; REQ-013, REQ-061, REQ-124) writes
# nothing: OpenOCD runs "init; reset run; shutdown" under the same reset configuration, time
# limit and result line as the other modes. The updater runs it when a trial has run past its
# time (src/onboard-computer/updater/restart_runner.hpp) and reads the result line:
#   RESTORE DONE: reset                   the controller was reset and runs
#   RESTORE FAILED: after reset (...)     the reset ran, then OpenOCD did not end cleanly (time-out,
#                                         signal or non-zero exit after the "reset and run" step)
#   RESTORE FAILED: <step> (...)          the reset is not shown to have happened
# Mode reset takes no image and does not need fsir-updater.
#
# BL.img is a SIGNED bootloader image (kind bootloader, tools/make_image.py), not the raw
# .bin. Every input is copied into a private work folder and checked there by the updater's
# check-only mode (fsir-updater --check-image, deployed next to this script) BEFORE the first
# OpenOCD step; only the checked copies are written (REQ-118).
#
# Output: exactly one result line on stdout at the end, from which the updater will read
# the result (REQ-060):
#   RESTORE DONE: <mode> ...        exit 0 (mode reset: exactly "RESTORE DONE: reset")
#   RESTORE FAILED: <step> ...      exit 1 (step failed) or 2 (usage / tool missing)
# The OpenOCD log is kept in $LOG (default /tmp/restore-openocd.log).
#
# REQ-056: nothing here talks to the controller's software. SWD reaches the chip core and
# flash controller directly, with the chip held in reset.
#
# Requirements (verification): REQ-054/055 restore the last-resort firmware; REQ-057 clear
# the boot state and the kept data in the backup registers (modes restore and full); REQ-118
# write only images that passed the checks; REQ-120 last-resort first; REQ-121 bootloader and
# main firmware back in order; REQ-060 report the result.
#
# OpenOCD references: OpenOCD User's Guide 0.12.0, chapter "Reset Configuration"
# (reset_config), chapter "Flash Commands" (flash erase_address, flash write_image,
# flash verify_image), chapter "General Commands" (shutdown), Flash Driver stm32l4x
# (stm32l4x driver: stm32g4x.cfg uses it; pages are 2 KB, writes are 8-byte words).

set -u

# ---- Layout: the values of src/controller/common/layout.h ---------------------------
# Kept as a copy because this script runs on the MP257 without the repository. If
# layout.h changes, this block must change by hand (nothing checks it yet; see README).
BOOTLOADER_START=0x08000000; BOOTLOADER_BYTES=24576   # 0x6000
SLOT_A_START=0x08006000;     SLOT_BYTES=40960         # 0xA000
SLOT_B_START=0x08010000
LAST_RESORT_START=0x0801A000; LAST_RESORT_BYTES=16384 # 0x4000
BOOT_STATE_START=0x0801E000;  BOOT_STATE_BYTES=8192   # 0x2000
RAM_START=536870912   # 0x20000000
RAM_BYTES=32768       # 0x8000
# ---- Backup registers (REQ-057): STM32G431, addresses as OpenOCD writes them ---------------
# Source of every value: RM0440 Rev 9 (ST, March 2025).
# TAMP_BKP0R: TAMP base 0x4000 2400 [Table 3, p. 87] + offset 0x100 [Sec. 38.6.8, p. 1623].
# BKP_COUNT: 16 backup registers, TAMP_BKP0R to TAMP_BKP15R [Sec. 38.1, p. 1610], the same as
# BACKUP_REGISTERS in src/controller/common/backup_layout.h (a copy: this script runs without
# the repository; if that header changes, this value must change by hand).
BKP0R_ADDR=0x40002500; BKP_COUNT=16
# PWR_CR1: PWR base 0x4000 7000 [Table 3, p. 86] + offset 0x00 [Sec. 6.4.1, p. 253].
# DBP is bit 8 [Sec. 6.4.1, p. 253]: 0x100. Writes to the backup registers need it set
# [Sec. 38.3.3, p. 1612].
PWR_CR1_ADDR=0x40007000; PWR_CR1_DBP=0x100
# RCC_APB1ENR1: RCC base 0x4002 1000 [Table 3, p. 85] + offset 0x58 [Sec. 7.4.17, p. 309].
# PWREN is bit 28 [Sec. 7.4.17, p. 309]: 0x10000000. The power interface clock is off after a
# reset (bit 28 is 0 in the reset value 0x0000 0400, Sec. 7.4.17, p. 309), and a register of a
# peripheral whose clock is off is not written [Sec. 7.4.17, p. 309, Note]; so the step turns
# it on before it sets DBP. The reset that ends the restore turns it off again.
# Proposal (TBR), awaiting the owner's acceptance: this clock enable is not in the bench log of
# the failure (req-057-restore-keeps-backup.log, planted without a reset, firmware running with
# the clock on); it is taken from the manual and not yet measured on the board.
RCC_APB1ENR1_ADDR=0x40021058; RCC_PWREN=0x10000000
# Image header (common/image_format.h): magic "FSIR", format 1, kind at byte 5, target
# 0x0431 at bytes 6-7, body length at 12, body CRC-32 at 16; body starts at byte 64;
# two 64-byte signatures follow the body. The updater checks all of it; this script only
# needs the body length (offset 12) to cut the bootloader body out of its image.
HDR=64; SIGS=128
OFFSET_LENGTH=12
# An image of HDR + SIGS bytes or less has an empty body and cannot be a firmware image.
MIN_IMAGE_EXCLUSIVE=$(( HDR + SIGS ))

# ---- Time limits -------------------------------------------------------------------
# The longest OpenOCD may run before it is stopped and the restore reported failed. Measured on
# the bench, 13 Oct 2026: a full restore (bootloader 21 KB, last-resort 2 KB, main firmware 20 KB,
# each written and verified; slot B and boot state erased) took 4.0 s
# (test/onboard-computer/results/2026-10-13/install-restore-full.log). With every region full
# (24 + 16 + 40 KB, about twice as much) it scales to about 8 s. 30 s is about 4 times that worst
# case: a slow but healthy restore is never cut, a hung one is caught in half a minute (owner
# accepted 30 s, 13 Oct 2026). Overridable by the environment (the tests set it small).
OPENOCD_TIMEOUT_S=${OPENOCD_TIMEOUT_S:-30}
# TBD: placeholder bound for one image check (a local file read and two signature checks);
# not a measured value. Overridable by the environment.
CHECK_TIMEOUT_S=${CHECK_TIMEOUT_S:-30}
# TBD: placeholder wait between the TERM and the KILL sent to a child that did not stop;
# not a measured value. Overridable by the environment.
KILL_GRACE_S=${KILL_GRACE_S:-5}

here=$(cd "$(dirname "$0")" && pwd)
OPENOCD=${OPENOCD:-$here/openocd}
OCD_SCRIPTS=${OCD_SCRIPTS:-$here/tcl}
FSIR_UPDATER=${FSIR_UPDATER:-$here/fsir-updater}
LOG=${LOG:-/tmp/restore-openocd.log}

# State of the run. REPORTED is set when the one result line has been printed.
mode=           # restore, full or reset; set from the first argument (reset_ran reads it from a signal handler)
WORK=
REPORTED=0
CHILD_PID=
CHILD_RUNNING=0
WD_PID=
OCD_STARTED=0   # 1 once OpenOCD has been started; before that $LOG may be a stale file of an older run
NL='
'

# @brief  Print the one RESTORE FAILED result line, remove the work folder and leave the script.
#
# fail <step> <detail> <status>
# The line is the report of REQ-060; the updater reads it later. It is printed at most once
# per run: a second call (for example a signal arriving while the normal failure path runs)
# only leaves the script, so the contract "exactly one result line" holds.
#
# @param $1  name of the step that failed
# @param $2  detail text shown in brackets (optional)
# @param $3  exit status (optional, default 1)
# @stdout "RESTORE FAILED: <step> (<detail>)", once
# @return exits the script with $3 (default 1)
fail() {
    if [ "$REPORTED" -eq 0 ]; then
        REPORTED=1
        echo "RESTORE FAILED: $1${2:+ ($2)}"
    fi
    exit "${3:-1}"
}

# @brief  Print the usage text and the RESTORE FAILED: usage result line, then leave the script.
#
# Usage text goes to stderr, the result line to stdout, so the line stays the only stdout
# output of a failed start. The detail says which argument was wrong (REQ-060: the reader of
# the line needs the cause).
#
# @param $1  detail text shown in brackets (optional)
# @stdout "RESTORE FAILED: usage (<detail>)"
# @return exits the script with 2
usage() {
    echo "usage: $0 restore|full --bootloader IMG --last-resort IMG [--main IMG] [--serial ST-LINK-serial]" >&2
    echo "       $0 reset [--serial ST-LINK-serial]     (restart the controller; writes nothing)" >&2
    echo "  IMG: signed images (tools/make_image.py); the bootloader image has kind bootloader" >&2
    echo "  env: OPENOCD=path/to/openocd  OCD_SCRIPTS=path/to/tcl  FSIR_UPDATER=path/to/fsir-updater" >&2
    echo "       LOG=path  OPENOCD_TIMEOUT_S=seconds" >&2
    fail usage "${1:-}" 2
}

# ---- Small helpers (POSIX sh + od, dd, tail, head, wc, mktemp; all in busybox) -------
# @brief  Read one 32-bit little-endian word from a file.
#
# le32 <file> <byte offset> -> decimal value of the little-endian word
#
# @param $1  file
# @param $2  byte offset of the word
# @stdout the word as a decimal number
# @return 0
le32() {
    od -An -tu1 -j "$2" -N 4 "$1" | {
        read -r b0 b1 b2 b3
        echo $(( b0 + b1 * 256 + b2 * 65536 + b3 * 16777216 ))
    }
}

# @brief  Format a number as 8 hex digits.
#
# Used in the failure messages of the vector table check.
#
# @param $1  decimal number
# @stdout the number as 8 lower-case hex digits
# @return 0
hex8() { printf '%08x' "$1"; }

# @brief  Decide whether a path is safe to put into the generated OpenOCD Tcl script.
#
# Tcl treats space, brackets, braces, dollar, double quote, backslash and newline as syntax
# even inside a word; a path carrying one could change what a command does. The paths are
# also braced in the script, but a path with these characters is refused outright (decision
# of the owner, audit item 17), so a mistake in the quoting cannot turn into a flash command.
#
# @param $1  path to test
# @return 0 if the path holds none of the characters; 1 if it holds one or is empty
path_ok() {
    [ -n "$1" ] || return 1
    case $1 in
        *" "*) return 1 ;;
        *"["*) return 1 ;;
        *"]"*) return 1 ;;
        *"{"*) return 1 ;;
        *"}"*) return 1 ;;
        *'$'*) return 1 ;;
        *'"'*) return 1 ;;
        *'\'*) return 1 ;;
        *"$NL"*) return 1 ;;
        *) return 0 ;;   # any other path is fine
    esac
}

# ---- Child processes, time limit, cleanup ---------------------------------------------

# @brief  Stop the watchdog subshell and wait for it.
#
# The watchdog traps TERM, kills its own sleep and exits, so this returns at once. Killing by
# PID only; no pkill (project rule).
#
# @return 0 (a watchdog that is already gone is not an error)
stop_watchdog() {
    if [ -n "$WD_PID" ]; then
        kill -TERM "$WD_PID" 2>/dev/null || true   # already gone: nothing to stop
        wait "$WD_PID" 2>/dev/null || true         # status of a killed subshell is not needed
        WD_PID=
    fi
    return 0
}

# @brief  Start a background watchdog that stops CHILD_PID after a delay.
#
# After <delay> seconds it creates $WORK/timedout, sends TERM to CHILD_PID, waits KILL_GRACE_S
# and sends KILL. KILL cannot be ignored, so a plain `wait` on the child is bounded by
# delay + KILL_GRACE_S (the bounded-wait rule). A delay of 0 is used to stop the child at once
# from the signal handler. The sleep runs inside the subshell and is killed by its own TERM
# trap, so no sleep process is left behind when the child ends first.
#
# @param $1  delay in seconds before TERM
# @return 0; WD_PID holds the watchdog's PID
start_watchdog() {
    (
        trap 'if [ -n "${s:-}" ]; then kill "$s" 2>/dev/null; fi; exit 0' TERM
        sleep "$1" & s=$!
        wait "$s"
        : > "$WORK/timedout"
        kill -TERM "$CHILD_PID" 2>/dev/null
        sleep "$KILL_GRACE_S" & s=$!
        wait "$s"
        kill -KILL "$CHILD_PID" 2>/dev/null
        exit 0
    ) &
    WD_PID=$!
    return 0
}

# @brief  Run a command in the background under a time limit, output to a file.
#
# run_bounded <seconds> <output file> <command> [args...]
# Why: OpenOCD can hang on a USB or SWD fault and the restore must still end with one result
# line (REQ-060). `timeout` exists in three incompatible forms (GNU, busybox -t, none on the
# Mac), so a watchdog written in sh is used instead: one code path, kills by PID only.
# Sets RB_STATUS (exit status of the command) and RB_TIMEDOUT (1 if the limit stopped it).
# A signal arriving during the wait runs the trap handler, which ends the script.
#
# @param $1  time limit in seconds
# @param $2  file that receives stdout and stderr of the command
# @param $3  command, then its arguments
# @return 0 if the command exited 0 in time; 1 otherwise
run_bounded() {
    rb_secs=$1; rb_out=$2; shift 2
    rm -f "$WORK/timedout"
    "$@" > "$rb_out" 2>&1 &
    CHILD_PID=$!
    CHILD_RUNNING=1
    start_watchdog "$rb_secs"
    wait "$CHILD_PID"
    RB_STATUS=$?
    CHILD_RUNNING=0
    stop_watchdog
    RB_TIMEDOUT=0
    if [ -f "$WORK/timedout" ]; then RB_TIMEDOUT=1; fi
    [ "$RB_STATUS" -eq 0 ] && [ "$RB_TIMEDOUT" -eq 0 ]
}

# @brief  Stop a still-running child and the watchdog, remove the work folder.
#
# Runs on every exit of the script (EXIT trap). If the script ends without having printed a
# result line (a shell error, set -u), it prints RESTORE FAILED so the contract of exactly one
# line holds even then (REQ-060).
#
# @stdout "RESTORE FAILED: <step> (script ended unexpectedly)" only if no line was printed yet
# @return exits with status 1 in that unexpected case; otherwise returns and the script ends
#         with the status it already had
cleanup() {
    stop_watchdog
    if [ "$CHILD_RUNNING" -eq 1 ] && [ -n "$CHILD_PID" ]; then
        kill -KILL "$CHILD_PID" 2>/dev/null || true   # child was running; gone is fine
        CHILD_RUNNING=0
    fi
    if [ -n "$WORK" ]; then rm -rf "$WORK"; fi
    if [ "$REPORTED" -eq 0 ]; then
        REPORTED=1
        echo "RESTORE FAILED: script (ended unexpectedly)"
        exit 1
    fi
}

# @brief  Handle INT, TERM and HUP: stop OpenOCD, print one RESTORE FAILED line, leave.
#
# Why: an interrupt during a flash step must not leave a hung OpenOCD or a work folder behind,
# and must still give the reader of the result line a step name (REQ-060). Further signals are
# ignored while this runs. OpenOCD is stopped by PID: TERM at once, KILL after KILL_GRACE_S.
#
# @param $1  signal name for the message (INT, TERM or HUP)
# @stdout "RESTORE FAILED: <last step or "interrupted"> (interrupted by SIG<name>)"
# @return exits the script with status 1
on_signal() {
    trap '' INT TERM HUP
    step=$(last_step)
    [ -n "$step" ] || step=interrupted
    stop_watchdog
    if [ "$CHILD_RUNNING" -eq 1 ]; then
        start_watchdog 0
        wait "$CHILD_PID" 2>/dev/null || true   # status of a killed child is not needed
        CHILD_RUNNING=0
        stop_watchdog
    fi
    fail "$step" "interrupted by SIG$1" 1
}

# ---- Image checks (REQ-118) --------------------------------------------------------
# The decisive check is the updater's check-only mode: magic, format, kind, target,
# length, body CRC-32, both Ed25519 signatures (src/onboard-computer/updater/). The size
# test here only refuses files that cannot hold a body, before the updater is asked.

# @brief  Copy one input image into the work folder and check the copy.
#
# check_copy <path> <kind name> <max bytes> <name> <copy file>
# Why: the file that is checked must be the file that is written (REQ-118). The copy is made
# once, checked once, and only the copy is ever named in the OpenOCD script, so the input file
# cannot change between check and write. Images of HDR + SIGS bytes or less (empty body) are
# refused here too (audit item 17). On any problem the script ends through fail.
#
# @param $1  path of the input image
# @param $2  kind the image must have: bootloader, main or last-resort
# @param $3  largest allowed file size in bytes
# @param $4  name used in messages
# @param $5  path of the copy in the work folder
# @stdout nothing (a check line goes to stderr)
# @return 0 if the copy passed; otherwise exits the script through fail
check_copy() {
    [ -f "$1" ] || fail "check $4" "file not found: $1" 1
    cp "$1" "$5" || fail "check $4" "cannot copy $1" 1
    size=$(( $(wc -c < "$5") ))
    [ "$size" -gt "$MIN_IMAGE_EXCLUSIVE" ] \
        || fail "check $4" "size $size: an image of $MIN_IMAGE_EXCLUSIVE bytes or less has no body" 1
    [ "$size" -le "$3" ] || fail "check $4" "size $size above $3" 1
    run_bounded "$CHECK_TIMEOUT_S" "$WORK/check.out" "$FSIR_UPDATER" --check-image "$5" --kind "$2"
    rc=$RB_STATUS
    msg=$(head -n 1 "$WORK/check.out" | tr -d '\r')
    if [ "$RB_TIMEDOUT" -eq 1 ]; then
        fail "check $4" "time-out after $CHECK_TIMEOUT_S s" 1
    fi
    case $rc in
        0) ;;
        1) fail "check $4" "refused: $msg" 1 ;;
        2) fail "check $4" "updater usage error: $msg" 2 ;;
        3) fail "check $4" "updater keys unusable: $msg" 1 ;;
        *) fail "check $4" "updater status $rc: $msg" 1 ;;
    esac
    echo "check: $4 $1 $size bytes: $msg" >&2
}

# @brief  Cut the bootloader body out of its checked image and test its vector table.
#
# cut_bootloader <checked image> <body file>
# Why: the chip's flash at 0x08000000 holds the bootloader as linked, with no header and no
# signatures (Interfaces Sec. 5.5; layout.h); only the body is written. The body length is the
# little-endian u32 at header offset 12 of the CHECKED copy. The vector table test is a second,
# additional check on the extracted body: initial SP in RAM, reset handler odd (Thumb) and
# inside the bootloader region [Arm DUI 0553A, Sec. 2.3.4, p. 2-24].
#
# @param $1  checked bootloader image (in the work folder)
# @param $2  file that receives the body
# @return 0 if the body was cut and passed; otherwise exits the script through fail
cut_bootloader() {
    len=$(le32 "$1" "$OFFSET_LENGTH")
    [ "$len" -ge 8 ] && [ "$len" -le "$BOOTLOADER_BYTES" ] \
        || fail "check bootloader image" "body length $len outside 8..$BOOTLOADER_BYTES" 1
    tail -c +$(( HDR + 1 )) "$1" | head -c "$len" > "$2" || fail "check bootloader image" "cannot cut the body" 1
    got=$(( $(wc -c < "$2") ))
    [ "$got" -eq "$len" ] || fail "check bootloader image" "body is $got bytes, header says $len" 1
    sp=$(le32 "$2" 0); rv=$(le32 "$2" 4)
    [ "$sp" -gt "$RAM_START" ] && [ "$sp" -le $(( RAM_START + RAM_BYTES )) ] \
        || fail "check bootloader image" "initial SP $(hex8 "$sp") not in RAM" 1
    [ $(( rv % 2 )) -eq 1 ] && [ "$rv" -ge $(( BOOTLOADER_START )) ] \
        && [ "$rv" -lt $(( BOOTLOADER_START + len )) ] \
        || fail "check bootloader image" "reset handler $(hex8 "$rv") not in the bootloader body" 1
}

# ---- OpenOCD run -------------------------------------------------------------------
# Command sequence per OpenOCD User's Guide 0.12.0:
#  reset_config srst_only srst_nogate connect_assert_srst  (chapter "Reset Configuration":
#      connect_assert_srst = assert SRST while connecting, so a running watchdog or a hung
#      program cannot interfere; srst_nogate = keep the debug logic alive during SRST.)
#  init; reset halt                          (the chip is held at its reset vector)
#  flash erase_address ADDR LEN              (chapter "Flash Commands"; page-aligned)
#  flash write_image FILE ADDR bin           (writes; the pages were erased before)
#  flash verify_image FILE ADDR bin          (compares flash with the file)
#  verify_image of an all-0xFF file          (reads the erased region back and compares)
#  clear backup registers (REQ-057)          (modes restore and full: read-modify-write of
#      RCC_APB1ENR1 bit PWREN and PWR_CR1 bit DBP, "mww" 0 to each of the 16 TAMP backup
#      registers, "read_memory" of all 16 back, DBP cleared; any value not 0 is a failure; see
#      fsir_clear_backup. The chip is halted by "reset halt". "mww" and "read_memory" are
#      OpenOCD commands; the chapter of the User's Guide 0.12.0 that lists them was not checked
#      here. Their behaviour is not yet measured on the board: Proposal (TBR), awaiting the
#      owner's acceptance.)
#  reset run                                 (start the program)
#  shutdown                                  (leave OpenOCD; "shutdown error" = exit non-zero)
# Each command is wrapped in "fsir_step" so the log names the step that failed. The name carries a
# prefix because OpenOCD registers its own command "step [address]" (single-step the CPU) when
# "init" runs; a procedure named "step" is replaced by it and the next call fails (measured on the
# MP257 with OpenOCD 0.12.0).

# Tcl procedure put at the head of the generated OpenOCD script (the block below is Tcl text).
TCL_HEAD='
# @brief  Run one OpenOCD command block under a step name and stop with an error if it fails.
#
# Named fsir_step, not step: "init" registers the OpenOCD built-in "step [address]", which would
# replace a procedure of that name. The log then names the step: "STEP: <name>" before,
# "STEPFAIL: <name>: <error>" on failure,
# "STEPDONE: <name>" on success (failed_step and reset_ran read them). "shutdown error" makes
# OpenOCD exit non-zero.
#
# @param name  step name shown in the log and in the RESTORE FAILED line
# @param body  Tcl commands to run in the calling scope
# @return nothing; on an error OpenOCD shuts down with a non-zero exit status
proc fsir_step {name body} {
    puts "STEP: $name"
    if {[catch {uplevel 1 $body} err]} {
        puts "STEPFAIL: $name: $err"
        shutdown error
    }
    puts "STEPDONE: $name"
}
'

# Tcl procedure of modes restore and full (put after the head; mode reset does not carry it).
# The addresses come from the "set" lines written before the step (see the steps file below).
TCL_BACKUP='
# @brief  Clear the 16 TAMP backup registers over SWD and check that they read 0 (REQ-057).
#
# Why: the registers survive a system reset (RM0440 Rev 9, Sec. 38.6.8, p. 1623), so a restore
# that leaves them keeps the kept data of the old firmware. Sequence: set PWREN, set DBP (both by
# read-modify-write, other bits kept), write 0 to each register, read all back, clear DBP. DBP is
# cleared whatever happened, then an error is raised if a write failed or a value is not 0.
# Uses the global variables fsir_pwr_cr1, fsir_dbp, fsir_rcc_enr, fsir_pwren, fsir_bkp0r and
# fsir_bkp_n, which the step file sets from restore.sh.
#
# @return nothing; raises an error (the step fails) if a register does not read 0
proc fsir_clear_backup {} {
    global fsir_pwr_cr1 fsir_dbp fsir_rcc_enr fsir_pwren fsir_bkp0r fsir_bkp_n
    mww $fsir_rcc_enr [expr {[lindex [read_memory $fsir_rcc_enr 32 1] 0] | $fsir_pwren}]
    mww $fsir_pwr_cr1 [expr {[lindex [read_memory $fsir_pwr_cr1 32 1] 0] | $fsir_dbp}]
    set rc [catch {
        for {set i 0} {$i < $fsir_bkp_n} {incr i} {
            mww [expr {$fsir_bkp0r + 4 * $i}] 0
        }
        set values [read_memory $fsir_bkp0r 32 $fsir_bkp_n]
        if {[llength $values] != $fsir_bkp_n} {
            error "read back [llength $values] of $fsir_bkp_n registers"
        }
        set i 0
        foreach v $values {
            if {$v != 0} { error "TAMP_BKP${i}R reads $v after the write of 0" }
            incr i
        }
    } err]
    mww $fsir_pwr_cr1 [expr {[lindex [read_memory $fsir_pwr_cr1 32 1] 0] & ~$fsir_dbp}]
    if {$rc != 0} { error $err }
}
'

# @brief  Start OpenOCD with the ST-LINK and STM32G4 scripts and the generated step file,
#         under the time limit OPENOCD_TIMEOUT_S.
#
# run_openocd <tcl steps file>
# Command sequence and documentation references: see the OpenOCD comment above. Output goes
# to $LOG. Sets RB_STATUS and RB_TIMEDOUT (see run_bounded).
#
# @param $1  path of the Tcl steps file
# @stdout nothing (OpenOCD output goes to the file $LOG)
# @return 0 if every step passed in time; 1 otherwise
run_openocd() {
    if [ -n "$STLINK_SERIAL" ]; then
        run_bounded "$OPENOCD_TIMEOUT_S" "$LOG" "$OPENOCD" -s "$OCD_SCRIPTS" \
            -f interface/stlink.cfg -c "adapter serial $STLINK_SERIAL" \
            -f target/stm32g4x.cfg \
            -c "reset_config srst_only srst_nogate connect_assert_srst" -f "$1"
    else
        run_bounded "$OPENOCD_TIMEOUT_S" "$LOG" "$OPENOCD" -s "$OCD_SCRIPTS" \
            -f interface/stlink.cfg \
            -f target/stm32g4x.cfg \
            -c "reset_config srst_only srst_nogate connect_assert_srst" -f "$1"
    fi
}

# @brief  Tell whether the reset of mode reset has already run, from the OpenOCD log.
#
# Why: after "STEPDONE: reset and run" the controller is running again, so a later failure (a
# time-out, a signal, a non-zero exit of OpenOCD) must be reported as "after reset": the updater
# sends EVENT 0x0106 only when the reset happened (decision H).
#
# @return 0 if mode is reset and the log holds "STEPDONE: reset and run"; 1 otherwise
reset_ran() {
    [ "$mode" = reset ] && [ "$OCD_STARTED" -eq 1 ] && [ -f "$LOG" ] || return 1
    grep -q '^STEPDONE: reset and run$' "$LOG"
}

# @brief  Find the last step OpenOCD announced in the log.
#
# Why: names the step in the result line when a run is interrupted or times out (REQ-060). In
# mode reset the name is "after reset" once the reset has run (see reset_ran).
#
# @stdout the name of the last "STEP:" line ("after reset" in mode reset once it ran), or nothing
#         if there is none (or no log)
# @return 0
last_step() {
    [ "$OCD_STARTED" -eq 1 ] && [ -f "$LOG" ] || return 0
    if reset_ran; then echo "after reset"; return 0; fi
    grep '^STEP: ' "$LOG" | tail -1 | sed 's/^STEP: //'
    return 0
}

# @brief  Find the name of the step that failed in the OpenOCD log.
#
# Uses the last STEPFAIL line; if there is none, the last STEP line; if there is none, OpenOCD
# never got as far as the first step (adapter or connection problem).
#
# @stdout the step name
# @return 0
failed_step() {
    s=
    if reset_ran; then echo "after reset"; return 0; fi
    if [ "$OCD_STARTED" -eq 1 ] && [ -f "$LOG" ]; then
        s=$(grep '^STEPFAIL: ' "$LOG" | tail -1 | sed 's/^STEPFAIL: //; s/: .*//')
    fi
    [ -n "$s" ] || s=$(last_step)
    [ -n "$s" ] || s="connect (see $LOG)"
    echo "$s"
}

# ---- Modes -------------------------------------------------------------------------
trap cleanup EXIT
trap 'on_signal INT' INT
trap 'on_signal TERM' TERM
trap 'on_signal HUP' HUP

mode=${1:-}; [ -n "$mode" ] || usage "no mode"
shift
BL=; LR=; MAIN=; STLINK_SERIAL=
while [ $# -gt 0 ]; do
    case $1 in
        --bootloader)  [ $# -ge 2 ] || usage "--bootloader needs a value"; BL=$2; shift 2 ;;
        --last-resort) [ $# -ge 2 ] || usage "--last-resort needs a value"; LR=$2; shift 2 ;;
        --main)        [ $# -ge 2 ] || usage "--main needs a value"; MAIN=$2; shift 2 ;;
        --serial)      [ $# -ge 2 ] || usage "--serial needs a value"; STLINK_SERIAL=$2; shift 2 ;;
        *) usage "unknown argument: $1" ;;
    esac
done

case $mode in
    restore) ;;
    full) ;;
    reset) ;;
    *) usage "unknown mode: $mode" ;;
esac
if [ "$mode" = reset ]; then
    [ -z "$BL$LR$MAIN" ] || usage "mode reset takes no images"
else
    [ -n "$BL" ] && [ -n "$LR" ] || usage "--bootloader and --last-resort are required"
fi
case $mode in
    full) [ -n "$MAIN" ] || usage "mode full needs --main" ;;
    restore) [ -z "$MAIN" ] || usage "mode restore takes no --main" ;;
    reset) ;;
    *) usage "unknown mode: $mode" ;;
esac
# Paths: refuse characters that are syntax in Tcl (see path_ok).
for p in ${BL:+"$BL"} ${LR:+"$LR"} ${MAIN:+"$MAIN"}; do   # mode reset has none
    path_ok "$p" || usage "path contains a space, [ ] { } \$ \" \\ or a newline: refused (paths)"
done
case $OPENOCD_TIMEOUT_S in
    ''|0|*[!0-9]*) usage "OPENOCD_TIMEOUT_S must be a whole number of seconds, at least 1" ;;
    *) ;;
esac
case $STLINK_SERIAL in
    *[!A-Za-z0-9]*) usage "--serial may hold letters and digits only" ;;
    *) ;;
esac

[ -x "$OPENOCD" ] || fail "openocd" "not executable: $OPENOCD" 2
[ -d "$OCD_SCRIPTS" ] || fail "openocd" "script directory not found: $OCD_SCRIPTS" 2
if [ "$mode" != reset ]; then   # the image checks are the only use of fsir-updater
    [ -x "$FSIR_UPDATER" ] || fail "check tool" "fsir-updater not executable: $FSIR_UPDATER" 2
fi

# Private work folder: mktemp makes an unpredictable name (the old $$ name could be guessed
# and pre-created by another user). TMPDIR convention kept.
WORK=$(mktemp -d "${TMPDIR:-/tmp}/restore.XXXXXX") || fail "work dir" "mktemp failed" 2
path_ok "$WORK" || fail "usage" "work folder path contains a character not allowed in a Tcl path (paths): $WORK" 2

if [ "$mode" != reset ]; then
    # REQ-118: nothing is written before every image has passed. The updater checks the COPIES.
    check_copy "$BL" bootloader $(( BOOTLOADER_BYTES + HDR + SIGS )) "bootloader image" "$WORK/bootloader.img"
    cut_bootloader "$WORK/bootloader.img" "$WORK/bootloader-body.bin"
    check_copy "$LR" last-resort "$LAST_RESORT_BYTES" "last-resort image" "$WORK/last-resort.img"
    if [ "$mode" = full ]; then
        check_copy "$MAIN" main "$SLOT_BYTES" "main firmware image" "$WORK/main.img"
    fi
    BL_FILE=$WORK/bootloader-body.bin
    LR_FILE=$WORK/last-resort.img
    MAIN_FILE=$WORK/main.img

    # BENCH VERSION vs REQ-120. REQ-120 puts the last-resort firmware first, in the
    # bootloader's place, so a restored chip runs it at once. On this bench the bootloader is
    # plain and small (it starts the last-resort itself when no slot is valid), so the chip
    # gets the bootloader at the start of flash and the last-resort image in its own region
    # (0x0801A000). The chip then boots: bootloader, no valid slot, last-resort runs.
    # Order inside the run: last-resort region first (REQ-120's intent), then bootloader,
    # so a power cut in between leaves a chip that does not start the bootloader at all
    # rather than one that starts without its fallback.
    # All-0xFF reference files for the erase read-back (an erased flash word reads 0xFF).
    head -c "$SLOT_BYTES" /dev/zero | tr '\000' '\377' > "$WORK/ff-slot.bin"
    head -c "$BOOT_STATE_BYTES" /dev/zero | tr '\000' '\377' > "$WORK/ff-state.bin"
fi
# Every path is in braces in the Tcl text (and none holds a Tcl special character).
if [ "$mode" = reset ]; then
    # Mode reset (decision H): connect with the chip held in reset, then release it. Nothing is
    # erased or written. The same reset_config as the other modes (run_openocd).
    {
        printf '%s\n' "$TCL_HEAD"
        cat <<EOT
fsir_step "connect" { init }
fsir_step "reset and run" { reset run }
shutdown
EOT
    } > "$WORK/steps.tcl" || fail "step file" "cannot write $WORK/steps.tcl" 2
else
    {
        printf '%s\n' "$TCL_HEAD"
        printf '%s\n' "$TCL_BACKUP"
        cat <<EOT
fsir_step "connect" { init }
fsir_step "halt under reset" { reset halt }
fsir_step "erase last-resort region" { flash erase_address $LAST_RESORT_START $LAST_RESORT_BYTES }
fsir_step "write last-resort" { flash write_image {$LR_FILE} $LAST_RESORT_START bin }
fsir_step "verify last-resort" { flash verify_image {$LR_FILE} $LAST_RESORT_START bin }
fsir_step "erase bootloader region" { flash erase_address $BOOTLOADER_START $BOOTLOADER_BYTES }
fsir_step "write bootloader" { flash write_image {$BL_FILE} $BOOTLOADER_START bin }
fsir_step "verify bootloader" { flash verify_image {$BL_FILE} $BOOTLOADER_START bin }
fsir_step "erase slot B" { flash erase_address $SLOT_B_START $SLOT_BYTES }
fsir_step "read back slot B erased" { flash verify_image {$WORK/ff-slot.bin} $SLOT_B_START bin }
fsir_step "erase slot A" { flash erase_address $SLOT_A_START $SLOT_BYTES }
fsir_step "read back slot A erased" { flash verify_image {$WORK/ff-slot.bin} $SLOT_A_START bin }
fsir_step "erase boot state (REQ-057)" { flash erase_address $BOOT_STATE_START $BOOT_STATE_BYTES }
fsir_step "read back boot state erased" { flash verify_image {$WORK/ff-state.bin} $BOOT_STATE_START bin }
EOT
        if [ "$mode" = full ]; then
            cat <<EOT
fsir_step "write main firmware (REQ-121)" { flash write_image {$MAIN_FILE} $SLOT_A_START bin }
fsir_step "verify main firmware" { flash verify_image {$MAIN_FILE} $SLOT_A_START bin }
EOT
        fi
        # REQ-057: the backup registers are cleared last, after every write and verify, so a
        # failed write leaves them as they were and the restore reports that step.
        cat <<EOT
set fsir_pwr_cr1 $PWR_CR1_ADDR
set fsir_dbp $PWR_CR1_DBP
set fsir_rcc_enr $RCC_APB1ENR1_ADDR
set fsir_pwren $RCC_PWREN
set fsir_bkp0r $BKP0R_ADDR
set fsir_bkp_n $BKP_COUNT
fsir_step "clear backup registers" { fsir_clear_backup }
fsir_step "reset and run" { reset run }
shutdown
EOT
    } > "$WORK/steps.tcl" || fail "step file" "cannot write $WORK/steps.tcl" 2
fi

# The script must end with shutdown, else OpenOCD would stay running after the last step.
last_line=$(grep -v '^[[:space:]]*$' "$WORK/steps.tcl" | tail -n 1)
[ "$last_line" = shutdown ] || fail "step file" "does not end with shutdown" 1

OCD_STARTED=1
if run_openocd "$WORK/steps.tcl"; then
    REPORTED=1
    if [ "$mode" = reset ]; then
        echo "RESTORE DONE: reset"
    elif [ "$mode" = full ]; then
        echo "RESTORE DONE: full: bootloader, last-resort, main firmware in slot A written and verified; slot B and boot state erased; chip running"
    else
        echo "RESTORE DONE: restore: bootloader and last-resort written and verified; slots A, B and boot state erased; chip running"
    fi
    exit 0
fi
if [ "$RB_TIMEDOUT" -eq 1 ]; then
    step=$(last_step); [ -n "$step" ] || step=connect
    fail "$step" "time-out after $OPENOCD_TIMEOUT_S s, OpenOCD stopped; log: $LOG" 1
fi
fail "$(failed_step)" "log: $LOG" 1
