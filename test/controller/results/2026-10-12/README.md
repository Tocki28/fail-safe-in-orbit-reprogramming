# Bench results — 12 October 2026, bootloader (build step 1)

Board: NUCLEO-G431KB on the Mac's USB. Code: commit 4389c88. Each log is the controller's serial
output (USART2 through the ST-LINK, 115200 8N1), captured from one second before the scenario was
written to the board, so a log can start with lines from the previous scenario's program.

| Scenario | Set-up | Result | Log |
|---|---|---|---|
| 1 | Good image in slot A, no boot state | PASS: slot A accepted and started | `scenario-1.log` |
| 2 | Hanging trial in slot B | PASS: 3 trial starts ended by the watchdog; trial given up; slot A started | `scenario-2.log` |
| 3 | Slot A with one body byte flipped | PASS: slot A refused (body check value); last-resort started | `scenario-3.log` |
| 4 | Slot B (trial) and slot A signed with unknown keys | PASS: slot B refused (signature B), trial given up; slot A refused (signature A); last-resort started | `scenario-4.log` |
| 5 | Both slots erased | PASS: slot A refused (empty); last-resort started | `scenario-5.log` |
| 6 | Bootloader only | PASS: "no firmware can run" | `scenario-6.log` |
| 7 | Good trial in slot B | PASS: B confirmed itself | `scenario-7.log` |
| 7, restart | Reset after scenario 7 | PASS: boot state confirmed B; slot B started as confirmed | `scenario-7-after-reset.log` |

Measured: two Ed25519 signature checks of a 2,660-byte test image take 37.8 to 39.6 ms at 170 MHz
(6.4 to 6.7 million cycles), Monocypher 4.0.2, bootloader built with -O2.

## Build step 2a — data link (same day, commit after 1452557)

Board: bootloader + firmware A (confirmed). `make -C test/controller link-probe`:
PASS — GET STATUS answered RECEIVED then DONE (3 times); bad check value dropped with no answer;
unknown command refused 0x06 only; frame for another destination ignored; the controller's sequence
numbers go up by one; the bootloader's lines appear once (the watchdog is reloaded).
Log: `step-2a-link-probe.log`. Host: frame unit tests 16/16.
