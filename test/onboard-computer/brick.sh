#!/bin/sh
# brick.sh - BENCH-ONLY TEST FUNCTION (REQ-101). NOT part of the operating software.
# Erases the controller's whole flash through the ST-LINK so that no firmware can run,
# to stage the "bricked controller" for the restore demo. Nothing in the flight design
# ever does this.
#
# OpenOCD User's Guide 0.12.0, chapter "Flash Commands", entry "Flash Driver stm32l4x":
# "stm32l4x mass_erase num" mass erases the entire stm32l4x device (stm32g4x.cfg uses the
# stm32l4x driver). Reset configuration as in restore.sh (chapter "Reset Configuration").
#
# Usage: brick.sh [--serial ST-LINK-serial]     env: OPENOCD, OCD_SCRIPTS, LOG as restore.sh
# Result line: "BRICK DONE: ..." (exit 0) or "BRICK FAILED: <step>" (exit 1).
set -u
# OpenOCD is looked up next to this script (deployed on the onboard computer), else in
# src/onboard-computer/restore/dist (this file in test/onboard-computer/ in the repository).
here=$(cd "$(dirname "$0")" && pwd)
[ -x "$here/openocd" ] || here=$(cd "$here/../../src/onboard-computer/restore/dist" 2>/dev/null && pwd) || here=.
OPENOCD=${OPENOCD:-$here/openocd}
OCD_SCRIPTS=${OCD_SCRIPTS:-$here/tcl}
LOG=${LOG:-/tmp/brick-openocd.log}
ser=
[ "${1:-}" = "--serial" ] && ser=${2:-}
[ -x "$OPENOCD" ] || { echo "BRICK FAILED: openocd not executable: $OPENOCD"; exit 2; }

# Private temp file (mktemp: an unpredictable name), removed on every way out of the script.
tcl=$(mktemp "${TMPDIR:-/tmp}/brick.XXXXXX") || { echo "BRICK FAILED: temp file (mktemp)"; exit 2; }

# @brief  Remove the temporary OpenOCD script.
#
# Runs on every exit and on INT, TERM and HUP (EXIT trap), so an interrupt leaves no file behind.
#
# @return 0
cleanup() { rm -f "$tcl"; }
trap cleanup EXIT
trap 'echo "BRICK FAILED: interrupted"; exit 1' INT TERM HUP
cat > "$tcl" <<'EOT'
# @brief  Run one OpenOCD command block under a step name and stop with an error if it fails.
#
# Named fsir_step, not step: "init" registers OpenOCD's own "step [address]", which would replace
# a procedure of that name (measured on the MP257, OpenOCD 0.12.0).
#
# The log then names the step: "STEP: <name>" before, "STEPFAIL: <name>: <error>" on failure
# (read below to build the result line). "shutdown error" makes OpenOCD exit non-zero.
#
# @param name  step name shown in the log and in the BRICK FAILED line
# @param body  Tcl commands to run in the calling scope
# @return nothing; on an error OpenOCD shuts down with a non-zero exit status
proc fsir_step {name body} {
    puts "STEP: $name"
    if {[catch {uplevel 1 $body} err]} { puts "STEPFAIL: $name: $err"; shutdown error }
}
fsir_step "connect" { init }
fsir_step "halt under reset" { reset halt }
fsir_step "mass erase" { stm32l4x mass_erase 0 }
# A mass erase leaves every word 0xFFFFFFFF; any other value at the start of flash means the
# erase did not happen (audit item 18). read_memory returns a list of words [OpenOCD User's
# Guide 0.12.0, chapter "General Commands", read_memory].
fsir_step "check flash start is erased" {
    if {[lindex [read_memory 0x08000000 32 1] 0] != 0xFFFFFFFF} { error "flash start not erased" }
}
shutdown
EOT
set -- -s "$OCD_SCRIPTS" -f interface/stlink.cfg
[ -n "$ser" ] && set -- "$@" -c "adapter serial $ser"
set -- "$@" -f target/stm32g4x.cfg -c "reset_config srst_only srst_nogate connect_assert_srst" -f "$tcl"
if "$OPENOCD" "$@" > "$LOG" 2>&1; then
    echo "BRICK DONE: flash mass-erased; no firmware can run"
    exit 0
fi
s=$(grep '^STEPFAIL: ' "$LOG" | tail -1 | sed 's/^STEPFAIL: //; s/: .*//')
echo "BRICK FAILED: ${s:-connect} (log: $LOG)"
exit 1
