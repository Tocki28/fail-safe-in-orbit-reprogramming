# Bench results — 13 October 2026, the whole chain: ground → onboard computer → controller

Bench: Mac (`src/ground/console.py`) → USB-C network → STM32MP257F-DK running `fsir-updater` as a
systemd service → the NUCLEO-G431KB's ST-LINK serial port → controller (bootloader and firmware
installed through the onboard computer's restore path, commit 9ddd64a; see
`test/onboard-computer/results/2026-10-13/`).

| Run | Result | Logs |
|---|---|---|
| `update --yes` (automatic confirm) | PASS: upload 0.85 s, deliver 3.04 s; ARM + SWITCH passed on as a pair; trial started; the onboard computer sent the test image; trial tests passed; ARM + CONFIRM; firmware confirmed; 7.3 s in all | `chain-update-yes.log`, `chain-update-yes-mp257.log` |
| `update`, operator answers **y** | PASS: upload 0.75 s, deliver 3.10 s; trial; tests passed; "Confirm? [y/N] y"; EVENT 0x0007; slot B in OPERATE, failed starts 0; 10.1 s in all | `chain-operator-y-then-n-mp257.log` (onboard computer side); console output seen by the operator |
| `update`, operator answers **n** | PASS: nothing sent; a further `update` during the trial is refused by the console before sending (no new firmware during a trial); EVENT 0x0008 119.6 s after EVENT 0x0006; the controller returns to slot B in OPERATE by itself | `chain-operator-y-then-n-mp257.log` |

The onboard computer's log shows the pairing of Interfaces Sec. 5.2: the ground's ARM held, then
ARM and the dangerous command sent back to back with consecutive data-link sequence numbers.
