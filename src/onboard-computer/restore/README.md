# Restore path (onboard computer -> controller, SWD)

Rewrites a bricked controller (NUCLEO-G431KB) through the Nucleo's on-board ST-LINK, which is
plugged into the MP257's USB. Bench stand-in for reprogramming link A (Interfaces Sec. 6.4):
in the final design the MP257's own pins drive SWD. Requirements: REQ-054/055/056/057/060/118/120/121.

## Files
- `restore.sh` - modes `restore`, `full` and `reset`; prints one `RESTORE DONE: ...` / `RESTORE FAILED: <step>` line.
- `fsir-updater` (board build of `src/onboard-computer/updater`) - deployed next to `restore.sh`; its check-only mode
  (`--check-image FILE --kind bootloader|main|last-resort`) is the check before any write (REQ-118).
- `../../../test/onboard-computer/restore_test.sh` - tests of `restore.sh` and `brick.sh` with a mock OpenOCD.
- `../../../test/onboard-computer/brick.sh` - test function (REQ-101): mass-erases the chip; not part of this folder.
- `build/` - Dockerfile + scripts that build a static aarch64 OpenOCD. Output goes to `dist/` (not committed).

## Build OpenOCD (on the Mac, Docker, slow under emulation)
    sh src/onboard-computer/restore/build/build.sh
Result: `dist/openocd`, `dist/tcl/`, `dist/VERSIONS.txt`. OpenOCD v0.12.0 (commit
9ea7f3d647c8ecf6b0f1424002dfc3f4504a162c, tarball
https://downloads.sourceforge.net/project/openocd/openocd/0.12.0/openocd-0.12.0.tar.bz2),
libusb 1.0.26 built from source with `--disable-udev`, ST-LINK adapter only. The binary is fully
static (`file`: ELF 64-bit LSB executable, ARM aarch64, statically linked); the target needs nothing
but the kernel and the ST-LINK's USB access (run as root).

## Deploy
The bootloader input is a signed image of kind bootloader, made by `make -C src/controller` (its default
`images` target) as `src/controller/build/images/bootloader.img` from the linked `bootloader/build/bootloader.bin`,
signed with the same development keys as the firmware images (`tools/bench.mk`; development keys only).
The last-resort image of the bench is `test/controller/build/images/last-resort-v1.img` (made by
`make -C test/controller`; no other last-resort image exists in the repository yet).

    D=src/onboard-computer/restore; I=test/controller/build/images; C=src/controller/build/images
    U=src/onboard-computer/updater
    ssh root@192.168.7.1 mkdir -p /home/root/restore
    scp -r $D/dist/openocd $D/dist/tcl $D/restore.sh test/onboard-computer/brick.sh \
        $U/build/board/fsir-updater $C/bootloader.img $I/last-resort-v1.img $I/slotA-v1.img \
        root@192.168.7.1:/home/root/restore/

## Steps of modes restore and full (one OpenOCD run)
connect; halt under reset; erase, write and verify the last-resort region; erase, write and verify the
bootloader; erase slot B and read it back erased; erase slot A and read it back erased; erase the boot state
(REQ-057) and read it back erased; (mode full only) write and verify the main firmware (REQ-121);
**clear backup registers (REQ-057)**; reset and run. A failed step ends the run with `RESTORE FAILED: <step name>`.

## Clear backup registers (REQ-057)
The 16 TAMP backup registers (TAMP_BKP0R to TAMP_BKP15R, 0x40002500 to 0x4000253C, `src/controller/common/backup_layout.h`)
survive a system reset and hold the bootloader's and the firmware's kept data (fault location, restart counts).
The step runs after all flash writes and verifies and before the final reset and run, with the chip halted:
it turns on the power interface clock (RCC_APB1ENR1 bit PWREN), sets DBP (PWR_CR1 bit 8), writes 0 to each of
the 16 registers, reads all 16 back and clears DBP again (also when a write or the read-back failed). A register
that does not read 0 gives `RESTORE FAILED: clear backup registers (log: ...)` and exit 1. Mode reset writes nothing.
Sources: RM0440 Rev 9, Sec. 38.3.3 p. 1612, Sec. 38.6.8 p. 1623, Sec. 6.4.1 p. 253, Sec. 7.4.17 p. 309.
Proposal (TBR), awaiting the owner's acceptance: switching on PWREN before DBP is taken from the manual and is
not yet measured on the board (the bench run that found F-3 planted the record with the firmware running, clock on).
Not yet measured on the board either: the OpenOCD commands `mww` and `read_memory` in this step.

## Mode reset (decision H, accepted by the owner, 13 Oct 2026)
    ./restore.sh reset [--serial S]
Writes nothing: OpenOCD `init; reset run; shutdown` under the same reset configuration and time limit
(`OPENOCD_TIMEOUT_S`). Needs no image and no `fsir-updater`. The updater runs it when a trial overruns
(`../updater/README.md`). One result line: `RESTORE DONE: reset`; `RESTORE FAILED: after reset (...)` when the
reset ran and OpenOCD then did not end cleanly (time-out, signal, non-zero exit); `RESTORE FAILED: <step> (...)`
otherwise (the reset is not shown to have happened).

## Run (on the MP257)
    cd /home/root/restore
    ./restore.sh restore --bootloader bootloader.img --last-resort last-resort-v1.img
    ./restore.sh full    --bootloader bootloader.img --last-resort last-resort-v1.img --main slotA-v1.img
    sh brick.sh        # test only: erase everything first
If the ST-LINK is not seen, check `lsusb` (0483:374e, STLINK-V3) and that the USB port is host mode.

Expected output ends with one of:
    RESTORE DONE: restore: bootloader and last-resort written and verified; slots A, B and boot state erased; chip running
    RESTORE DONE: full: bootloader, last-resort, main firmware in slot A written and verified; slot B and boot state erased; chip running
    RESTORE FAILED: <step> (log: /tmp/restore-openocd.log)
Exit code 0 / 1 (step failed, image refused, time-out, interrupted) / 2 (usage, tool missing).

Environment: `OPENOCD`, `OCD_SCRIPTS`, `FSIR_UPDATER` (default: next to the script), `LOG`,
`OPENOCD_TIMEOUT_S` (time limit of the OpenOCD run; see below). Paths must not contain a space,
`[ ] { } $ "` or `\`, nor a newline (they go into a Tcl script); such a path gives `RESTORE FAILED: usage`.

## Time limit
OpenOCD runs under a watchdog written in sh (`timeout` differs between GNU, busybox and the Mac). On expiry
OpenOCD gets TERM, then KILL after `KILL_GRACE_S`, and the line is `RESTORE FAILED: <step> (time-out after N s ...)`.
An interrupt (INT, TERM, HUP) also stops OpenOCD and prints one `RESTORE FAILED: <step> (interrupted by SIG...)` line.
`OPENOCD_TIMEOUT_S` is TBD: to be set from a measured full restore on the bench. The 300 s in the script is a generous
PLACEHOLDER, not a measured value. `CHECK_TIMEOUT_S` (30 s, image check) and `KILL_GRACE_S` (5 s) are placeholders too.

## Checks before writing, and what is left open
Every input is copied into a private work folder (`mktemp -d` under `$TMPDIR`), and the updater's check-only mode
runs on each copy before the first OpenOCD step: magic, format, kind, target, length, body CRC-32 and both Ed25519
signatures (REQ-118). Only the checked copies are written. A file of 192 bytes or less (empty body) is refused.
The bootloader image is cut to its body (bytes 64 .. 64+length, length = u32 at header offset 12) and only the body
is written at 0x08000000; the body also gets a vector table check (initial SP in RAM, reset handler inside the body).
Last-resort and main images are written whole. Nothing is written unless every check passed.
Open (TBD): the version rule that REQ-118 also names; this script does not apply it, and the check-only mode's header documents the signature, check value, length, target, kind and region checks only.
The work folder is removed on every way out of the script.

## Bench versions differing from the design
- REQ-120 (last-resort firmware first, in the bootloader's place): here the plain bootloader
  stays at 0x08000000 and the last-resort image sits in its own region at 0x0801A000.
- Last-resort image is the test firmware (`last-resort-v1.img` from `test/controller`).
- The layout constants in `restore.sh` are a copy of `layout.h`; nothing checks them yet.
