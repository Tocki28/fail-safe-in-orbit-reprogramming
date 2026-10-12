# Restore path (onboard computer -> controller, SWD)

Rewrites a bricked controller (NUCLEO-G431KB) through the Nucleo's on-board ST-LINK, which is
plugged into the MP257's USB. Bench stand-in for reprogramming link A (Interfaces Sec. 6.4):
in the final design the MP257's own pins drive SWD. Requirements: REQ-054/055/056/057/060/118/120/121.

## Files
- `restore.sh` - modes `restore` and `full`; prints one `RESTORE DONE: ...` / `RESTORE FAILED: <step>` line.
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
    D=src/onboard-computer/restore; I=test/controller/build/images
    ssh root@192.168.7.1 mkdir -p /home/root/restore
    scp -r $D/dist/openocd $D/dist/tcl $D/restore.sh test/onboard-computer/brick.sh \
        src/controller/bootloader/build/bootloader.bin $I/last-resort-v1.img $I/slotA-v1.img \
        root@192.168.7.1:/home/root/restore/

## Run (on the MP257)
    cd /home/root/restore
    ./restore.sh restore --bootloader bootloader.bin --last-resort last-resort-v1.img
    ./restore.sh full    --bootloader bootloader.bin --last-resort last-resort-v1.img --main slotA-v1.img
    sh brick.sh        # test only: erase everything first
If the ST-LINK is not seen, check `lsusb` (0483:374b) and that the USB port is host mode.

Expected output ends with one of:
    RESTORE DONE: restore: bootloader and last-resort written and verified; slots A, B and boot state erased; chip running
    RESTORE DONE: full: bootloader, last-resort, main firmware in slot A written and verified; slot B and boot state erased; chip running
    RESTORE FAILED: <step> (log: /tmp/restore-openocd.log)
Exit code 0 / 1 (step failed) / 2 (usage, tool missing).

## Checks before writing, and what is left to the updater
Done here: size, header magic/format/kind/target, header length vs file size, body CRC-32
(computed with gzip's trailer, no python needed), bootloader vector table and "BL: " string.
Not done here (integration point with `src/onboard-computer/updater/`): the two Ed25519
signatures and the version rule, which REQ-118 also asks for.

## Bench versions differing from the design
- REQ-120 (last-resort firmware first, in the bootloader's place): here the plain bootloader
  stays at 0x08000000 and the last-resort image sits in its own region at 0x0801A000.
- Last-resort image is the test firmware (`last-resort-v1.img` from `test/controller`).
- The layout constants in `restore.sh` are a copy of `layout.h`; nothing checks them yet.
