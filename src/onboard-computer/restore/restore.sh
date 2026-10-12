#!/bin/sh
# restore.sh - rewrite the controller (NUCLEO-G431KB, STM32G431KB) over SWD (reprogramming
# link A, Interfaces Sec. 6.4) through the Nucleo's on-board ST-LINK, driven by OpenOCD on
# the onboard computer (STM32MP257F-DK).
#
# BENCH STAND-IN (accepted): on the final design the MP257's own pins drive SWD. Here the
# ST-LINK on the Nucleo does, plugged into the MP257's USB. Nothing else differs for the
# controller: it sees SWD either way.
#
#   restore.sh restore --bootloader BL.bin --last-resort LR.img
#   restore.sh full    --bootloader BL.bin --last-resort LR.img --main FW.img
#
# Output: exactly one result line on stdout at the end, from which the updater will read
# the result (REQ-060):
#   RESTORE DONE: <mode> ...        exit 0
#   RESTORE FAILED: <step> ...      exit 1 (step failed) or 2 (usage / tool missing)
# The OpenOCD log is kept in $LOG (default /tmp/restore-openocd.log).
#
# REQ-056: nothing here talks to the controller's software. SWD reaches the chip core and
# flash controller directly, with the chip held in reset.
#
# Requirements (verification): REQ-054/055 restore the last-resort firmware; REQ-057 clear
# the boot state; REQ-118 write only images that passed the checks; REQ-120 last-resort
# first; REQ-121 bootloader and main firmware back in order; REQ-060 report the result.
#
# OpenOCD references: OpenOCD User's Guide 0.12.0, chapter "Reset Configuration"
# (reset_config), chapter "Flash Commands" (flash erase_address, flash write_image,
# flash verify_image), chapter "General Commands" (shutdown), Flash Driver stm32l4x
# (stm32l4x driver: stm32g4x.cfg uses it; pages are 2 KB, writes are 8-byte words).

set -u

# ---- Layout: the values of src/controller/common/layout.h ---------------------------
# Kept as a copy because this script runs on the MP257 without the repository. If
# layout.h changes, this block must change by hand (nothing checks it yet; see README).
FLASH_START=0x08000000
BOOTLOADER_START=0x08000000; BOOTLOADER_BYTES=24576   # 0x6000
SLOT_A_START=0x08006000;     SLOT_BYTES=40960         # 0xA000
SLOT_B_START=0x08010000
LAST_RESORT_START=0x0801A000; LAST_RESORT_BYTES=16384 # 0x4000
BOOT_STATE_START=0x0801E000;  BOOT_STATE_BYTES=8192   # 0x2000
RAM_START=536870912   # 0x20000000
RAM_BYTES=32768       # 0x8000
# Image header (common/image_format.h): magic "FSIR", format 1, kind at byte 5, target
# 0x0431 at bytes 6-7, body length at 12, body CRC-32 at 16; body starts at byte 64;
# two 64-byte signatures follow the body.
HDR=64; SIGS=128
KIND_MAIN=2; KIND_LAST_RESORT=3

here=$(cd "$(dirname "$0")" && pwd)
OPENOCD=${OPENOCD:-$here/openocd}
OCD_SCRIPTS=${OCD_SCRIPTS:-$here/tcl}
LOG=${LOG:-/tmp/restore-openocd.log}
WORK=${TMPDIR:-/tmp}/restore.$$
CRC_LOG=

# @brief  Print the one RESTORE FAILED result line, remove the work folder and leave the script.
#
# fail <step> <detail>
# The line is the report of REQ-060; the updater reads it later.
#
# @param $1  name of the step that failed
# @param $2  detail text shown in brackets (optional)
# @param $3  exit status (optional, default 1)
# @stdout "RESTORE FAILED: <step> (<detail>)"
# @return exits the script with $3 (default 1)
fail() {
    echo "RESTORE FAILED: $1 ${2:+($2)}"
    rm -rf "$WORK"
    exit "${3:-1}"
}

# @brief  Print the usage text and the RESTORE FAILED: usage result line, then leave the script.
#
# Usage text goes to stderr, the result line to stdout, so the line stays the only stdout
# output of a failed start.
#
# @stdout "RESTORE FAILED: usage"
# @return exits the script with 2
usage() {
    echo "usage: $0 restore|full --bootloader FILE --last-resort FILE [--main FILE] [--serial ST-LINK-serial]" >&2
    echo "  env: OPENOCD=path/to/openocd  OCD_SCRIPTS=path/to/tcl  LOG=path" >&2
    echo "RESTORE FAILED: usage"
    exit 2
}

# ---- Small helpers (POSIX sh + od, dd, tail, head, gzip, wc; all in busybox) ---------
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
# @brief  Compute the zlib CRC-32 of a byte range of a file.
#
# crc32_of_bytes <file> <skip> <count> -> 8 hex digits, zlib CRC-32
# gzip's trailer is the CRC-32 (same polynomial and conventions as zlib) of its input,
# little-endian, followed by the input length. Needs no python on the target.
#
# @param $1  file
# @param $2  number of bytes to skip at the start
# @param $3  number of bytes to checksum
# @stdout the CRC-32 as 8 lower-case hex digits
# @return 0
crc32_of_bytes() {
    tail -c +$(( $2 + 1 )) "$1" | head -c "$3" | gzip -c -n | tail -c 8 | head -c 4 \
        | od -An -tx1 | {
        read -r b0 b1 b2 b3
        echo "$b3$b2$b1$b0"
    }
}
# @brief  Format a number as 8 hex digits.
#
# Used in the failure messages of the bootloader check.
#
# @param $1  decimal number
# @stdout the number as 8 lower-case hex digits
# @return 0
hex8() { printf '%08x' "$1"; }

# ---- Image checks (REQ-118, bench subset) ------------------------------------------
# Here: size, header fields, body CRC-32. NOT here: the two Ed25519 signatures and the
# version rule. That full check is the updater's (src/onboard-computer/updater/); this
# script must call it before the write once it exists. INTEGRATION POINT.

# @brief  Check the bootloader .bin before anything is written (REQ-118, bench subset).
#
# raw .bin from objcopy: no header, no CRC field to compare against. Checks size, vector
# table and the "BL: " string; the CRC-32 is printed, not compared.
#
# @param $1  path of the bootloader .bin
# @stdout nothing (the check line goes to stderr)
# @return 0 if the file passed; otherwise exits the script through fail
check_bootloader() {
    f=$1
    [ -f "$f" ] || fail "check bootloader" "file not found: $f"
    size=$(( $(wc -c < "$f") ))
    [ "$size" -ge 8 ] && [ "$size" -le "$BOOTLOADER_BYTES" ] \
        || fail "check bootloader" "size $size outside 8..$BOOTLOADER_BYTES"
    # Vector table word 0 = initial stack pointer (in RAM), word 1 = reset handler
    # (odd = Thumb, inside the bootloader region) [Arm DUI 0553A, Sec. 2.3.4, p. 2-24].
    sp=$(le32 "$f" 0); rv=$(le32 "$f" 4)
    [ "$sp" -gt "$RAM_START" ] && [ "$sp" -le $(( RAM_START + RAM_BYTES )) ] \
        || fail "check bootloader" "initial SP $(hex8 "$sp") not in RAM"
    [ $(( rv % 2 )) -eq 1 ] && [ "$rv" -ge $(( BOOTLOADER_START )) ] \
        && [ "$rv" -lt $(( BOOTLOADER_START + BOOTLOADER_BYTES )) ] \
        || fail "check bootloader" "reset handler $(hex8 "$rv") not in bootloader region"
    # Proprietary-string check: every bootloader console line starts with "BL: "
    # (bootloader/main.c). Weak, but it rejects a firmware .bin given by mistake.
    grep -aq 'BL: ' "$f" || fail "check bootloader" "no \"BL: \" string: not the bootloader"
    echo "check: bootloader $f $size bytes, CRC-32 $(crc32_of_bytes "$f" 0 "$size")" >&2
}

# @brief  Check a signed image (header, size, body CRC-32) before anything is written (REQ-118, bench subset).
#
# check_image <file> <kind> <region bytes> <name>
# The signatures are not checked here (see the integration point above).
#
# @param $1  path of the image
# @param $2  expected kind byte (2 main, 3 last-resort)
# @param $3  size of the flash region the image goes to, in bytes
# @param $4  name used in the messages
# @stdout nothing (the check line goes to stderr)
# @return 0 if the image passed; otherwise exits the script through fail
check_image() {
    f=$1; kind=$2; max=$3; name=$4
    [ -f "$f" ] || fail "check $name" "file not found: $f"
    size=$(( $(wc -c < "$f") ))
    [ "$size" -ge $(( HDR + SIGS )) ] && [ "$size" -le "$max" ] \
        || fail "check $name" "size $size outside $(( HDR + SIGS ))..$max"
    [ "$(head -c 4 "$f")" = "FSIR" ] || fail "check $name" "magic is not FSIR"
    fmt=$(od -An -tu1 -j 4 -N 1 "$f" | tr -d ' ')
    k=$(od -An -tu1 -j 5 -N 1 "$f" | tr -d ' ')
    tgt=$(od -An -tx1 -j 6 -N 2 "$f" | tr -d ' ')
    [ "$fmt" = 1 ] || fail "check $name" "format $fmt, expected 1"
    [ "$k" = "$kind" ] || fail "check $name" "kind $k, expected $kind"
    [ "$tgt" = "3104" ] || fail "check $name" "target field (LE) $tgt, expected 3104 (0x0431)"
    len=$(le32 "$f" 12)
    [ $(( HDR + len + SIGS )) -eq "$size" ] \
        || fail "check $name" "header length $len does not match file size $size"
    want=$(od -An -tx1 -j 16 -N 4 "$f" | { read -r a b c d; echo "$d$c$b$a"; })
    got=$(crc32_of_bytes "$f" "$HDR" "$len")
    [ "$want" = "$got" ] || fail "check $name" "body CRC-32 $got, header says $want"
    echo "check: $name $f $size bytes, body CRC-32 $got ok" >&2
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
#  reset run                                 (start the program)
#  shutdown                                  (leave OpenOCD; "shutdown error" = exit non-zero)
# Each command is wrapped in "step" so the log names the step that failed.

# Tcl procedure put at the head of the generated OpenOCD script (the block below is Tcl text).
TCL_HEAD='
# @brief  Run one OpenOCD command block under a step name and stop with an error if it fails.
#
# The log then names the step: "STEP: <name>" before, "STEPFAIL: <name>: <error>" on failure
# (failed_step reads them). "shutdown error" makes OpenOCD exit non-zero.
#
# @param name  step name shown in the log and in the RESTORE FAILED line
# @param body  Tcl commands to run in the calling scope
# @return nothing; on an error OpenOCD shuts down with a non-zero exit status
proc step {name body} {
    puts "STEP: $name"
    if {[catch {uplevel 1 $body} err]} {
        puts "STEPFAIL: $name: $err"
        shutdown error
    }
}
'

# @brief  Run OpenOCD with the ST-LINK and STM32G4 scripts and the generated step file.
#
# run_openocd <tcl steps file>
# Command sequence and documentation references: see the OpenOCD comment above.
#
# @param $1  path of the Tcl steps file
# @stdout nothing (OpenOCD output goes to the file $LOG)
# @return exit status of OpenOCD (0 = every step passed)
run_openocd() {
    if [ -n "$STLINK_SERIAL" ]; then
        "$OPENOCD" -s "$OCD_SCRIPTS" -f interface/stlink.cfg -c "adapter serial $STLINK_SERIAL" \
            -f target/stm32g4x.cfg \
            -c "reset_config srst_only srst_nogate connect_assert_srst" -f "$1" > "$LOG" 2>&1
    else
        "$OPENOCD" -s "$OCD_SCRIPTS" -f interface/stlink.cfg \
            -f target/stm32g4x.cfg \
            -c "reset_config srst_only srst_nogate connect_assert_srst" -f "$1" > "$LOG" 2>&1
    fi
}

# @brief  Find the name of the step that failed in the OpenOCD log.
#
# Uses the last STEPFAIL line; if there is none, the last STEP line; if there is none, OpenOCD
# never got as far as the first step (adapter or connection problem).
#
# @stdout the step name
# @return 0
failed_step() {
    s=$(grep '^STEPFAIL: ' "$LOG" | tail -1 | sed 's/^STEPFAIL: //; s/: .*//')
    [ -n "$s" ] || s=$(grep '^STEP: ' "$LOG" | tail -1 | sed 's/^STEP: //')
    [ -n "$s" ] || s="connect (see $LOG)"
    echo "$s"
}

# ---- Modes -------------------------------------------------------------------------
mode=${1:-}; [ -n "$mode" ] || usage
shift
BL=; LR=; MAIN=; STLINK_SERIAL=
while [ $# -gt 0 ]; do
    case $1 in
        --bootloader) BL=${2:-}; shift 2 ;;
        --last-resort) LR=${2:-}; shift 2 ;;
        --main) MAIN=${2:-}; shift 2 ;;
        --serial) STLINK_SERIAL=${2:-}; shift 2 ;;
        *) usage ;;
    esac
done
[ -x "$OPENOCD" ] || fail "openocd" "not executable: $OPENOCD" 2
[ -d "$OCD_SCRIPTS" ] || fail "openocd" "script directory not found: $OCD_SCRIPTS" 2
mkdir -p "$WORK" || fail "work dir" "$WORK" 2

case $mode in
    restore|full) ;;
    *) usage ;;
esac
[ -n "$BL" ] && [ -n "$LR" ] || usage
[ "$mode" = full ] && { [ -n "$MAIN" ] || usage; }
[ "$mode" = restore ] && { [ -z "$MAIN" ] || usage; }

# REQ-118: nothing is written before every image has passed.
check_bootloader "$BL"
check_image "$LR" "$KIND_LAST_RESORT" "$LAST_RESORT_BYTES" "last-resort image"
[ "$mode" = full ] && check_image "$MAIN" "$KIND_MAIN" "$SLOT_BYTES" "main firmware image"

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
{
    printf '%s\n' "$TCL_HEAD"
    cat <<EOT
step "connect" { init }
step "halt under reset" { reset halt }
step "erase last-resort region" { flash erase_address $LAST_RESORT_START $LAST_RESORT_BYTES }
step "write last-resort" { flash write_image $LR $LAST_RESORT_START bin }
step "verify last-resort" { flash verify_image $LR $LAST_RESORT_START bin }
step "erase bootloader region" { flash erase_address $BOOTLOADER_START $BOOTLOADER_BYTES }
step "write bootloader" { flash write_image $BL $BOOTLOADER_START bin }
step "verify bootloader" { flash verify_image $BL $BOOTLOADER_START bin }
step "erase slot B" { flash erase_address $SLOT_B_START $SLOT_BYTES }
step "read back slot B erased" { flash verify_image $WORK/ff-slot.bin $SLOT_B_START bin }
step "erase slot A" { flash erase_address $SLOT_A_START $SLOT_BYTES }
step "read back slot A erased" { flash verify_image $WORK/ff-slot.bin $SLOT_A_START bin }
step "erase boot state (REQ-057)" { flash erase_address $BOOT_STATE_START $BOOT_STATE_BYTES }
step "read back boot state erased" { flash verify_image $WORK/ff-state.bin $BOOT_STATE_START bin }
EOT
    if [ "$mode" = full ]; then
        cat <<EOT
step "write main firmware (REQ-121)" { flash write_image $MAIN $SLOT_A_START bin }
step "verify main firmware" { flash verify_image $MAIN $SLOT_A_START bin }
EOT
    fi
    cat <<EOT
step "reset and run" { reset run }
shutdown
EOT
} > "$WORK/steps.tcl"

if run_openocd "$WORK/steps.tcl"; then
    if [ "$mode" = full ]; then
        echo "RESTORE DONE: full: bootloader, last-resort, main firmware in slot A written and verified; slot B and boot state erased; chip running"
    else
        echo "RESTORE DONE: restore: bootloader and last-resort written and verified; slots A, B and boot state erased; chip running"
    fi
    rm -rf "$WORK"
    exit 0
fi
fail "$(failed_step)" "log: $LOG"
