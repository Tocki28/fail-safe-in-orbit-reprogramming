# Bench results — 13 October 2026, bootloader after the audit fixes

Board: NUCLEO-G431KB on the Mac's USB. Each log is the controller's serial output (USART2 through
the ST-LINK, 115200 8N1), read for a fixed time from one second before the scenario was written.
Scenarios 1, 3, 5 and 6 ran on commit a1f43ec; scenarios 2, 4 and 7 on the watchdog-check fix
committed with this page (logs show 9efca6f plus that fix).

| Scenario | Set-up | Result | Log |
|---|---|---|---|
| 1 | Good image in slot A, no boot state | PASS: "boot state blank", set to confirmed A; slot A started | `scenario-1.log` |
| 2 | Hanging trial in slot B | PASS: 3 trial starts, each ended by the watchdog; trial given up; slot A started | `scenario-2.log` |
| 3 | Slot A with one body byte flipped | PASS: slot A refused (body check value); last-resort started | `scenario-3.log` |
| 4 | Slot B (trial) and slot A signed with unknown keys | PASS: slot B refused (signature B), trial given up; slot A refused (signature A); last-resort started | `scenario-4.log` |
| 5 | Both slots erased | PASS: slot A refused (empty); last-resort started | `scenario-5.log` |
| 6 | Bootloader only | PASS: "no firmware can run"; parked with the watchdog reloaded, no restart in 45 s | `scenario-6.log` |
| 7 | Good trial in slot B | PASS: slot B started on trial and confirmed itself | `scenario-7.log` |
| 7, restart | Reset after scenario 7 | PASS: boot state confirmed B; slot B started as confirmed | `scenario-7-after-reset.log` |

## Bug found on the board and fixed

The first run of scenario 2 (commit a1f43ec) printed "trial slot B not started: watchdog not
confirmed running" and started slot A: no trial could ever start. The diagnostic build (logging
the raw registers) showed `IWDG_SR 0x00000002, RCC_CSR 0x00000002`: the watchdog's clock was
ready (LSIRDY 1) but its new reload value was still being transferred (RVU 1) a few milliseconds
after it was written (`scenario-2-diagnostic.log`). The check now waits, bounded at 50 ms, for the
transfer to finish. Measured settle time: 12.65 to 12.76 ms in all runs of scenarios 2, 4 and 7.

Measured: two signature checks take 37.6 to 39.8 ms at 170 MHz (Monocypher 4.0.2, -O2).

## Build step 2d — trial, trial tests, ARM, SWITCH and CONFIRM (commit 5e9bdbc)

Board: bootloader + firmware A (confirmed), written with `make -C src/controller flash-firmware`
before each run; `make -C test/controller trial-probe SCENARIO=<name>` acts as the onboard
computer over data link A.

| Scenario | Result | Log |
|---|---|---|
| confirm | PASS: firmware-B uploaded (3.0 s); SWITCH without ARM refused 0x01; ARM + SWITCH: DONE, restart, slot B on trial (EVENT 0x0006, HEALTH TRIAL, 119 s left); ARM + CONFIRM before the test image refused 0x0F; test image accepted, EVENT 0x000A; CONFIRM without ARM and with an ARM naming another sequence number refused 0x01; ARM + CONFIRM: EVENT 0x0007 before DONE; HEALTH OPERATE on slot B | `step-2d-trial-confirm.log` |
| bad-image | PASS: test image with a wrong check value refused 0x08 at END; EVENT 0x000B detail 0x03, then EVENT 0x0008; restart; boot state confirmed A, no trial; HEALTH OPERATE on slot A | `step-2d-trial-bad-image.log` |
| timeout | PASS: trial tests passed, no CONFIRM; EVENT 0x0008 119.7 s after EVENT 0x0006 (limit 120 s), no EVENT 0x000B; slot A back in OPERATE | `step-2d-trial-timeout.log` |

Measured: two signature checks of the 17.8 KB firmware image take 68.0 to 68.1 ms at 170 MHz.

Note (13 Oct 2026): in the logs, the path of the local working folder is written as `~/`. Only that
path text was changed; every other byte of each log is as recorded.
