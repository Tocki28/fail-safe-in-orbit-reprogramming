# Bench results — 13 October 2026, onboard computer (STM32MP257F-DK)

| Test | Result | Log |
|---|---|---|
| Install through the restore path: `restore.sh full --bootloader bootloader.img --last-resort last-resort-v1.img --main firmware-A.img` | PASS after one fix: every image checked by `fsir-updater --check-image` (both signatures), bootloader, last-resort and main firmware written and verified, slot B and boot state erased, chip running; 4.0 s in all. The controller started: "boot state blank", slot A accepted and started | `install-restore-full.log` |

Bug found here first: the Tcl helper named `step` was replaced by OpenOCD's built-in `step`
after `init`, so the second step failed ("RESTORE FAILED: connect"). Renamed `fsir_step`
(commit 21f2f98), with a test that reproduces the failure on the old script.

Measured: a full restore takes 4.0 s (OpenOCD 0.12.0, ST-LINK V3 over the MP257's USB).
