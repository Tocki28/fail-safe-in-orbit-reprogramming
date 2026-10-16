#!/bin/sh
# selfcheck.sh — proves that the fuzz targets catch defects (CODE_RULES E1: prove that tests catch
# a deliberate break). For each mutant, one source file of the code under test is copied to
# build/mutants/<name>/, one line is changed in the copy, the target is built against the copy and
# run on a short input count; the mutant is caught if the target exits with a failure. Nothing in
# src/ or test/ is edited: the Makefile variable of that file points at the copy.
#
# Usage: sh selfcheck.sh   (run from test/controller/stress/fuzz, through `make selfcheck`)
# Output: one line per mutant, PASS if caught (with the kinds of failure seen) or FAIL if not.
# Exit status: 0 if every mutant was caught, 1 otherwise.

REPO=$(cd ../../../.. && pwd)
FW=$REPO/src/controller/firmware
COMMON=$REPO/src/controller/common
BL=$REPO/src/controller/bootloader
SEED=7
MISSED=0

# @brief Copies a source file, changes one text in the copy, builds the target against it, runs
#        it and reports whether the target caught the change.
# @param $1 mutant name (also the build directory name)
# @param $2 target (fz_frame, fz_command, fz_image, fz_bootstate or fz_backup)
# @param $3 Makefile variable that names the file (FW_FRAME_SRC, ARM_SRC, ...)
# @param $4 path of the original file
# @param $5 text to replace (must occur in the file)
# @param $6 replacement text
# @param $7 number of inputs to run
# @stdout one result line: PASS or FAIL, the name, the failure kinds seen
# @return 0 if caught, 1 if not caught or the mutant could not be made
mutant() {
    name=$1; target=$2; var=$3; original=$4; old=$5; new=$6; count=$7
    dir=build/mutants/$name
    mkdir -p "$dir"
    copy=$dir/$(basename "$original")
    if ! python3 -I - "$original" "$copy" "$old" "$new" <<'PY'
import sys
src, dst, old, new = sys.argv[1:5]
text = open(src).read()
if old not in text:
    sys.exit(1)
open(dst, "w").write(text.replace(old, new))
PY
    then
        echo "FAIL  $name: the text to change was not found in $original"
        return 1
    fi
    if [ "$var" = "BL_MAIN_DIR" ]; then
        value=$dir
    else
        value=$copy
    fi
    if ! make --no-print-directory B="$dir" EXTRA=-Wno-error "$var=$value" "$dir/$target" \
        > "$dir/build.log" 2>&1; then
        echo "FAIL  $name: the mutant did not build (see $dir/build.log)"
        return 1
    fi
    "$dir/$target" --seed $SEED --n "$count" --log "$dir/log" --dir build/images/src \
        --dir build/images/test > "$dir/run.out" 2>&1
    status=$?
    kinds=$(grep '^FAIL ' "$dir/run.out" | sed 's/^FAIL \([^:]*\):.*/\1/' | sort | uniq -c |
        tr '\n' ' ')
    if [ $status -ne 0 ] && [ -n "$kinds" ]; then
        echo "PASS  $name: caught: $kinds"
        return 0
    fi
    echo "FAIL  $name: NOT caught (exit status $status)"
    return 1
}

mutant frame-no-gap-rule fz_frame FW_FRAME_SRC $FW/data-link/frame.cpp \
    '    if ((count_ > 0U) && ((now_ms - last_ms_) > kFrameGapMaxMs)) {
        dropHeld();
    }' '' 200000 || MISSED=1
mutant frame-no-flags-rule fz_frame FW_FRAME_SRC $FW/data-link/frame.cpp \
    'bool good = checkValueOk() && flagsOk();' 'bool good = checkValueOk();' 200000 || MISSED=1
mutant frame-no-crc-rule fz_frame FW_FRAME_SRC $FW/data-link/frame.cpp \
    'bool good = checkValueOk() && flagsOk();' 'bool good = flagsOk();' 200000 || MISSED=1
mutant frame-length-limit fz_frame FW_FRAME_SRC $FW/data-link/frame.cpp \
    'readU16(&bytes_[kFrameOffsetLength]) <= kFramePayloadMax;' \
    'readU16(&bytes_[kFrameOffsetLength]) <= (kFramePayloadMax + 1U);' 200000 || MISSED=1
mutant arm-sequence-ignored fz_command ARM_SRC $FW/command-checker/arm_state.cpp \
    '(command_ != static_cast<uint32_t>(command)) || (sequence_ != sequence)' \
    '(command_ != static_cast<uint32_t>(command))' 150000 || MISSED=1
mutant arm-never-expires fz_command ARM_SRC $FW/command-checker/arm_state.cpp \
    '(now_ms - armed_ms_) >= kArmExpiryMs' 'false' 150000 || MISSED=1
mutant arm-not-used-up fz_command MODE_SRC $FW/mode-manager/mode_manager.cpp \
    '    arm.disarm();
    if ((command == kCmdConfirm) && !trialTestsPassed()) {' \
    '    if ((command == kCmdConfirm) && !trialTestsPassed()) {' 150000 || MISSED=1
mutant dangerous-without-arm fz_command MODE_SRC $FW/mode-manager/mode_manager.cpp \
    'AnswerCode armed = arm.check(command, sequence, now_ms);' \
    'AnswerCode armed = AnswerCode::kSuccess;' 150000 || MISSED=1
mutant upload-trial-allowed fz_command UPDATE_SRC $FW/update-receiver/update_receiver.cpp \
    '    if (toSlot(state.trial) != Slot::kNone) {
        return AnswerCode::kTrial;
    }' '' 300000 || MISSED=1
mutant upload-chunk-offset-unchecked fz_command UPDATE_SRC $FW/update-receiver/update_receiver.cpp \
    'return (cmd.value == written_) && (cmd.data_bytes <= room) &&' \
    'return (cmd.data_bytes <= room) &&' 150000 || MISSED=1
mutant upload-end-early fz_command UPDATE_SRC $FW/update-receiver/update_receiver.cpp \
    '(phase_ == UpdatePhase::kReceiving) && sameTransfer(cmd) && (written_ == length_);' \
    '(phase_ == UpdatePhase::kReceiving) && sameTransfer(cmd);' 150000 || MISSED=1
mutant upload-begin-length fz_command UPDATE_SRC $FW/update-receiver/update_receiver.cpp \
    '(cmd.value >= kImageMinBytes) && (cmd.value <= kSlotBytes)' \
    '(cmd.value <= kSlotBytes)' 150000 || MISSED=1
mutant testimage-crc-unchecked fz_command TESTIMG_SRC $FW/mode-manager/test_image.cpp \
    'if (crc32_compute(bytes_.data(), covered) == stored) {' 'if (true) {' 150000 || MISSED=1
mutant image-signature-b-skipped fz_image IMAGE_CHECK_SRC $COMMON/image_check.c \
    'crypto_ed25519_check(signature_b, PUBLIC_KEY_B, image, signed_bytes) != 0' \
    '0' 20000 || MISSED=1
mutant image-bl-body-check-skipped fz_image BL_IMAGE_CHECK_SRC $BL/bl_image_check.c \
    'if (bl_crc32_compute(&image[IMAGE_HEADER_BYTES], fields.body_bytes) != fields.body_check) {' \
    'if (false) {' 20000 || MISSED=1
mutant bootstate-start-limit fz_bootstate BL_BOOT_STATE_SRC $BL/bl_boot_state.c \
    'if (state->failed_starts > BOOT_STATE_MAX_STARTS) {' \
    'if (state->failed_starts > (BOOT_STATE_MAX_STARTS + 1U)) {' 100000 || MISSED=1
mutant bootstate-common-tie fz_bootstate BOOT_STATE_SRC $COMMON/boot_state.c \
    'copies[0].sequence >= copies[1].sequence' \
    'copies[0].sequence > copies[1].sequence' 100000 || MISSED=1
mutant backup-skip-threshold fz_backup BL_MAIN_DIR $BL/main.c \
    '(count < CONFIRMED_RESTARTS_MAX)' '(count < (CONFIRMED_RESTARTS_MAX + 1U))' 20000 || MISSED=1
mutant backup-first-fault-overwritten fz_backup BL_BACKUP_SRC $BL/backup.c \
    'if (read_pair(BACKUP_PAIR_FAULT_LOCATION * PAIR_WORDS, &stored) && (stored != 0U)) {' \
    'if (false) {' 20000 || MISSED=1
mutant backup-pair-check-skipped fz_backup BL_BACKUP_SRC $BL/backup.c \
    'if (word != ~inverse) {' 'if (false) {' 20000 || MISSED=1

if [ $MISSED -eq 0 ]; then
    echo "selfcheck: every mutant was caught"
else
    echo "selfcheck: at least one mutant was NOT caught"
fi
exit $MISSED
