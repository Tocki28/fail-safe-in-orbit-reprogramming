# Fault matrix — verification campaign, 15 October 2026

This page lists every verification activity of the Verification Plan (Part A) and every pair of
an update moment and a fault (Part B). Each row names the source that asks for it, how the fault
is caused, its status, and its evidence or the reason it has none. Rows close as the campaign
runs. Values marked TBR are proposals awaiting the owner's acceptance (section "Open decisions").

## Build under test

| Unit | Program | Identified by |
|---|---|---|
| Controller | bootloader, firmware A and B | images built at commit 9ddd64a (build ids "9ddd64a firmware A", "9ddd64a firmware B"), installed 13 Oct through the restore path |
| Onboard computer | `fsir-updater` | until 15 Oct 17:16 UTC: sha256 `e49e7d32…4768`, commit not known; since then: sha256 `e5f46cc6…6eae`, built at d95a67a (two builds byte-identical; decision 1) |
| Onboard computer | clock, journal | since 15 Oct 17:16 UTC: clock set from the Mac (decision 3); journal kept on the SD card (decision 2) |
| Onboard computer | `restore.sh`, `brick.sh`, `fsir-updater.service` | byte-identical to commit d95a67a (sha256 compared 15 Oct) |
| Ground | `src/ground/console.py` | commit d95a67a |

State at the start (15 Oct, 18:59 EEST): OPERATE, slot A, failed starts 0, self-test PASSED
(`onboard-computer/results/2026-10-15/phase0-status-console.log`). Host baseline at d95a67a:
controller `make check` exit 0, onboard computer `make check` exit 0 (unit 324/324, bench test
all passed, image tool 11/0, restore 372/0), ground `make check` exit 0 (45 tests).

## Legend

**Status** (one per row):
- PASS: measured on the build above; the expected result held in every run.
- FAIL: measured; the expected result did not hold. Listed under "Findings".
- NOT RUN: not measured on this build. The Reason column says why.
- NOT ON BENCH: needs equipment the 12 Oct build declaration left out (data link B, CAN FD or
  RS-422, the factory-loader restore, SWD driven by the onboard computer's own pins). The
  automatic power switch, also left out then, is on the bench since 15 Oct: the MP257's USB2422
  hub switches the Nucleo's port power (`onboard-computer/results/2026-10-15/power-switch-check.log`).
- N/A: the pair has no meaning. The Reason column says why.

**Reason** (rows that are not PASS):
- PLANNED-2, PLANNED-3, PLANNED-4: will run in that phase (2 scripted, 3 by Tom's hands,
  4 injection over SWD, design shown to the owner first).
- EARLIER BUILD: passed on an earlier commit (named in the evidence column); the code has
  changed since, so it runs again.
- NOT BUILT-ACCEPTED: the function is not built, and the owner's acceptance of that is on
  record (the last column names where).
- NOT BUILT-OPEN: the function is not built, and no decision on it is on record; the last column
  names the file and line that says it is missing (decision 13, 15 Oct 2026).
- NEEDS TOOL: needs a test tool that does not exist yet (decision D4).
- NEEDS DECISION: needs a value or a choice from the owner (section "Open decisions").
- DESK: an inspection or analysis; no bench run.
- DUPLICATE: the same fault on the same code path as the row named.
- NO TRAFFIC: nothing crosses the faulted link or runs at that moment.

**Method**: SCRIPTED (console, ssh, systemctl or sysfs on the onboard computer; no hands);
HAND (the owner acts on a cable or button); OPENOCD (SWD from the onboard computer: read or
write memory, breakpoint, reset); PROBE (a `test/controller` probe, Nucleo on the Mac's USB);
INSPECTION; ANALYSIS; "—" when N/A.

**Evidence paths** are relative to `test/`. "tom-y-n" is
`onboard-computer/results/2026-10-15/tom-update-y-n-mp257.log` (the owner's two runs of
15 Oct, saved from the onboard computer's volatile journal at 18:58); the number after the
colon is its line.

## Part A — Verification Plan activities

One row per condition of each success criterion (Verification Plan Sec. 6.0: "one per
condition").

| ID | Activity | Condition | Requirement | Method | Status | Reason | Evidence, or where the reason is written |
|---|---|---|---|---|---|---|---|
| A1.1 | Test-1 Modes | Exactly one mode in every HEALTH report over a long run | REQ-001 | SCRIPTED | NOT RUN | PLANNED-2 | recorded in the Test-8 run (A8.4) |
| A1.2 | Test-1 Modes | OPERATE to TRIAL on ARM and SWITCH | REQ-003 | SCRIPTED | PASS | — | tom-y-n:106-111 (SWITCH passed on, DONE, EVENT 0x0006, HEALTH TRIAL); controller 9ddd64a |
| A1.3 | Test-1 Modes | TRIAL to OPERATE on ARM and CONFIRM | REQ-003 | SCRIPTED | PASS | — | tom-y-n:118-125 (EVENT 0x0007, DONE, HEALTH OPERATE slot A) |
| A1.4 | Test-1 Modes | Transitions into and out of FALLBACK | REQ-003, REQ-117 | — | NOT RUN | NOT BUILT-OPEN | `src/controller/firmware/mode-manager/mode_manager.hpp:7` ("FALLBACK is a later step"); a given-up trial reports OPERATE |
| A1.5 | Test-1 Modes | RECOVERY: no firmware can run, bootloader parks | REQ-001 | OPENOCD | NOT RUN | EARLIER BUILD | `controller/results/2026-10-13/scenario-6.log` (a1f43ec) |
| A1.6 | Test-1 Modes | CONFIRM in OPERATE and SWITCH in TRIAL refused 0x05 | REQ-002, REQ-027 | SCRIPTED | NOT RUN | PLANNED-2 | — |
| A1.7 | Test-1 Modes | LEAVE FALLBACK (0x23) | REQ-002 | — | NOT RUN | NOT BUILT-OPEN | `src/controller/firmware/main.cpp:20-22` |
| A2.1 | Test-2 Safe state | Outputs safe at every start before any command | REQ-004 | — | NOT RUN | NOT BUILT-OPEN | no controlled outputs: EQUIPMENT 0x40 is TBD-equip (`docs/interfaces/main.tex:365`) |
| A2.2 | Test-2 Safe state | Safe state after every function that ends | REQ-102 | — | NOT RUN | NOT BUILT-OPEN | as A2.1 |
| A2.3 | Test-2 Safe state | A safe-state command reaches the safe state from every mode | REQ-106 | — | NOT RUN | NOT BUILT-OPEN | no such command in the command table (`docs/interfaces/main.tex:345-366`) |
| A3.1 | Test-3 Image checks | Stored image with one bit changed is not run; the current copy keeps running | REQ-005 | OPENOCD | NOT RUN | EARLIER BUILD | `controller/results/2026-10-13/scenario-3.log` (a1f43ec) |
| A3.2 | Test-3 Image checks | Signature A invalid: not run | REQ-006 | OPENOCD | NOT RUN | EARLIER BUILD | `controller/results/2026-10-13/scenario-4.log` (9efca6f plus fix) |
| A3.3 | Test-3 Image checks | Signature B invalid: not run | REQ-006 | OPENOCD | NOT RUN | EARLIER BUILD | `controller/results/2026-10-13/scenario-4.log` |
| A3.4 | Test-3 Image checks | Receiver refuses at UPLOAD END: flipped byte 0x08, unknown key A 0x09, other slot 0x0E | REQ-005, REQ-006 | PROBE | NOT RUN | EARLIER BUILD | `controller/results/2026-10-12/step-2c-cpp-upload-probe.log` (bd82bab) |
| A4.1 | Test-4 Trial | Trial of the set length (120 s, bench value); given up when not confirmed in time | REQ-011, REQ-013 | SCRIPTED | PASS | — | tom-y-n:223 EVENT 0x0006 at 334.617 s, tom-y-n:235 EVENT 0x0008 at 454.359 s: 119.742 s |
| A4.2 | Test-4 Trial | Trial tests run and their result reported | REQ-139 | SCRIPTED | PASS | — | tom-y-n:114 and :227 (EVENT 0x000a) |
| A4.3 | Test-4 Trial | CONFIRM before the trial tests passed refused 0x0F | REQ-012, REQ-020 | PROBE | NOT RUN | EARLIER BUILD | `controller/results/2026-10-13/step-2d-trial-confirm.log` (5e9bdbc) |
| A4.4 | Test-4 Trial | Confirmed on the operator's ARM and CONFIRM | REQ-012 | SCRIPTED | PASS | — | tom-y-n:118-123 |
| A4.5 | Test-4 Trial | Given up at once when a trial test fails (bad test image) | REQ-140 | PROBE | NOT RUN | EARLIER BUILD | `controller/results/2026-10-13/step-2d-trial-bad-image.log` (5e9bdbc) |
| A4.6 | Test-4 Trial | Given up when the new firmware hangs | REQ-014 | SCRIPTED | NOT RUN | PLANNED-2 | `slotB-v2-hang.img` through the whole chain (B5.12) |
| A4.7 | Test-4 Trial | Given up on the operator's order (RETURN TO PREVIOUS 0x21) | REQ-113 | SCRIPTED | PASS | — | `ground/results/2026-10-15/return-in-trial.log` (EVENT 0x0008 at 10552.679 s, DONE at 10552.681 s, then OPERATE slot A); refused 0x05 in OPERATE: `ground/results/2026-10-15/return-in-operate.log`; firmware 49314cc |
| A4.8 | Test-4 Trial | Switch at the set moment (bench: on the operator's SWITCH; TBD-switch open) | REQ-021 | SCRIPTED | PASS | — | tom-y-n:106-110 |
| A5.1 | Test-5 Failed starts | After 3 failed starts the previous copy runs | REQ-015 | SCRIPTED | PASS | — | `ground/results/2026-10-15/trial-forever-decision-h.log`: starts 1, 2, 3 of slot B (two of them after operator resets), then EVENT 0x0002 refused copy B, check 0x0c, OPERATE slot A; controller 49314cc |
| A5.2 | Test-5 Failed starts | A power cut during a trial counts as a failed start | REQ-016 | SCRIPTED | PASS | — | B5.1: `ground/results/2026-10-16/power-cuts/pc3-trial-three-cuts.log` |
| A5.3 | Test-5 Failed starts | The count survives restarts and power cuts (HEALTH and the boot state read back) | REQ-017 | SCRIPTED | PASS | — | B5.1 (failed starts 2 and 3 read in HEALTH after each power cut); `ground/results/2026-10-16/power-cuts/pc2-trial-cut-1.log` |
| A5.4 | Test-5 Failed starts | Given-up firmware is not started again by a restart or a SWITCH without a new delivery | REQ-019 | SCRIPTED | FAIL | — | Finding F-1: `ground/results/2026-10-15/finding-37-switch-to-given-up.log` |
| A5.5 | Test-5 Failed starts | Failed firmware started again on order (START FAILED 0x22) | REQ-019 | — | NOT RUN | NOT BUILT-OPEN | `src/controller/firmware/main.cpp:20-22` |
| A6.1 | Test-6 Fall-back | No update changes the previous copy or the last-resort firmware, by reading memory back | REQ-007, REQ-008 | OPENOCD | PASS | — | slot B and the last-resort read back over SWD after 3 power cuts in deliveries and 3 in a trial, byte-identical to their images (`ground/results/2026-10-16/power-cuts/`) |
| A6.2 | Test-6 Fall-back | Previous copy also fails: the last-resort firmware runs | REQ-018 | OPENOCD | NOT RUN | EARLIER BUILD | `controller/results/2026-10-13/scenario-4.log`, `scenario-5.log` |
| A6.3 | Test-6 Fall-back | The last-resort firmware receives new firmware | REQ-010 | — | NOT RUN | NOT BUILT-OPEN | the bench last-resort is the test application without a data link (`controller/test-firmware/main.c:1-12`) |
| A6.4 | Test-6 Fall-back | Stays in FALLBACK until the operator acts | REQ-117 | — | NOT RUN | NOT BUILT-OPEN | `src/controller/firmware/mode-manager/mode_manager.hpp:7` |
| A7.1 | Test-7 Restart | Firmware made to hang restarts on its own and resumes operation | REQ-022 | OPENOCD | NOT RUN | PLANNED-4 | B8.12 |
| A7.2 | Test-7 Restart | Restart time measured (limit TBD-restart) | REQ-023 | OPENOCD | NOT RUN | PLANNED-4 | B8.2, B8.12 |
| A8.1 | Test-8 Independence | The controller keeps working while the onboard computer restarts | REQ-024 | SCRIPTED | NOT RUN | PLANNED-2 | B8.4 |
| A8.2 | Test-8 Independence | The controller keeps working with data link A cut | REQ-025 | SCRIPTED | NOT RUN | PLANNED-2 | B8.6 |
| A8.3 | Test-8 Independence | Both data links cut | REQ-025 | — | NOT ON BENCH | — | data link B not on the bench |
| A8.4 | Test-8 Independence | A run of TBD-runlen without commands | REQ-026 | SCRIPTED | NOT RUN | NEEDS DECISION | D3 |
| A9.1 | Test-9 Command checks | Command not allowed in the mode refused 0x05 | REQ-027 | SCRIPTED | NOT RUN | PLANNED-2 | A1.6, B8.11 |
| A9.2 | Test-9 Command checks | Impossible values or unknown command refused 0x06 | REQ-028 | SCRIPTED | NOT RUN | NEEDS TOOL | D4 tool T1; earlier `controller/results/2026-10-12/step-2a-link-probe.log` |
| A9.3 | Test-9 Command checks | Dangerous command not armed refused 0x01 (onboard computer and controller) | REQ-029, REQ-112 | SCRIPTED | NOT RUN | NEEDS TOOL | D4 tool T1; earlier `controller/results/2026-10-13/step-2d-trial-confirm.log` |
| A9.4 | Test-9 Command checks | Armed too long ago refused 0x02 | REQ-030 | SCRIPTED | NOT RUN | NEEDS TOOL | D4 tool T1 |
| A9.5 | Test-9 Command checks | Past its expiry refused 0x03 | REQ-133 | — | NOT RUN | NOT BUILT-OPEN | `src/onboard-computer/updater/command_gate.hpp:19-20`; `src/controller/firmware/main.cpp:221-225` |
| A9.6 | Test-9 Command checks | Scheduled while the clocks disagree refused 0x0C | REQ-134 | — | NOT RUN | NOT BUILT-OPEN | `src/controller/firmware/main.cpp:221-225` |
| A9.7 | Test-9 Command checks | ARM or dangerous command sent again runs once, answered ALREADY DONE | REQ-031 | SCRIPTED | NOT RUN | PLANNED-2 | B3.10, B7.10 |
| A9.8 | Test-9 Command checks | Any other command sent again runs once | REQ-031 | — | NOT RUN | NOT BUILT-ACCEPTED | `src/onboard-computer/updater/command_gate.hpp:14-19` |
| A9.9 | Test-9 Command checks | Held command changed before its run time refused | REQ-032 | — | NOT RUN | NOT BUILT-OPEN | no SCHEDULE (0x0A) in `src/onboard-computer/updater/protocol.hpp:20-35` |
| A9.10 | Test-9 Command checks | No single changed bit of the stored arm state arms it | REQ-137 | OPENOCD | NOT RUN | PLANNED-4 | — |
| A10.1 | Test-10 Answers | RECEIVED, then DONE, for every command | REQ-033 | SCRIPTED | NOT RUN | PLANNED-2 | the journal logs DONE and REFUSED only; RECEIVED needs a ground-side record |
| A10.2 | Test-10 Answers | DONE only after the effect is seen (CONFIRM: EVENT 0x0007 before DONE) | REQ-034 | SCRIPTED | PASS | — | tom-y-n:123 (EVENT 0x0007, 249.102 s) before tom-y-n:124 (DONE, 249.105 s) |
| A10.3 | Test-10 Answers | REFUSED with its reason | REQ-035 | SCRIPTED | NOT RUN | PLANNED-2 | with A9 |
| A11.1 | Test-11 Reports and faults | HEALTH at the set rate (1 s, bench value; TBD-rate) | REQ-037 | SCRIPTED | NOT RUN | PLANNED-2 | measured over the Test-8 run |
| A11.2 | Test-11 Reports and faults | Each injected fault gives its EVENT report | REQ-038 | OPENOCD | NOT RUN | PLANNED-4 | — |
| A11.3 | Test-11 Reports and faults | Self-test at every start; a failed one reported and the chip restarted | REQ-039 | OPENOCD | NOT RUN | PLANNED-4 | self-test = CRC-32 of the running image (`mode_manager.cpp:126`, `:145-161`); periodic self-test (TBD-selftest) not built |
| A11.4 | Test-11 Reports and faults | After a fall-back, the next reports state which firmware runs | REQ-040 | SCRIPTED | PASS | — | tom-y-n:235-236 (EVENT 0x0008, then HEALTH slot A); bootloader fall-back in B5.12 |
| A11.5 | Test-11 Reports and faults | A changed bit in the controller's memory is detected and leads to the safe state | REQ-041 | OPENOCD | NOT RUN | PLANNED-4 | B8.8 |
| A11.6 | Test-11 Reports and faults | Controller traffic never stops the onboard computer reaching it | REQ-042 | SCRIPTED | PASS | — | `ground/results/2026-10-16/endurance/summary.txt`: 100 of 100 updates confirmed while HEALTH reports streamed (median 7.9 s, slowest 12.3 s); every command answered |
| A12.1 | Test-12 Clock | Controller clock keeps time from its own source while the onboard clock is wrong (it reads 2000-01-01) | REQ-131 | SCRIPTED | NOT RUN | PLANNED-2 | controller clock field against the Mac's clock |
| A12.2 | Test-12 Clock | Controller clock changes only on SET CONTROLLER TIME | REQ-132 | — | NOT RUN | NOT BUILT-OPEN | `src/controller/firmware/main.cpp:20-22` |
| A12.3 | Test-12 Clock | Every report carries both times | REQ-135 | SCRIPTED | NOT RUN | PLANNED-2 | HEALTH and EVENT payloads decoded |
| A13.1 | Test-13 Image checks | Not released by the operator (unknown key A) refused 0x09 | REQ-043 | SCRIPTED | NOT RUN | EARLIER BUILD | `controller/results/2026-10-12/milestone-2-updater.log` (updater 9da60f3) |
| A13.2 | Test-13 Image checks | Damaged refused 0x08 | REQ-044 | SCRIPTED | NOT RUN | EARLIER BUILD | as A13.1 |
| A13.3 | Test-13 Image checks | Older than the minimum version refused 0x0A | REQ-045 | — | NOT RUN | NOT BUILT-OPEN | `src/onboard-computer/updater/image_checker.hpp:29-31` |
| A13.4 | Test-13 Image checks | Built for another controller refused 0x0B | REQ-046 | SCRIPTED | NOT RUN | PLANNED-2 | target bytes 6-7 changed with `tools/flip_byte.py` |
| A13.5 | Test-13 Image checks | Image changed after its last transfer refused before delivery | REQ-047 | SCRIPTED | NOT RUN | PLANNED-2 | B2.8 |
| A14.1 | Test-14 Upload and delivery | Upload cut at random chunks resumes where it stopped | REQ-049 | SCRIPTED | PASS | — | `ground/results/2026-10-15/radio-loss/resume-after-loss.log`: stopped at offset 2560 by 50 % loss, resumed with `upload --offset 2560`, UPLOAD END checks passed; ground/results/2026-10-15/radio-loss/summary.txt |
| A14.2 | Test-14 Upload and delivery | Delivery runs while the controller works | REQ-048 | SCRIPTED | PASS | — | HEALTH reports #120-#122 arrived during the delivery of `ground/results/2026-10-16/power-cuts/pc1-deliver-1.log`; 100 deliveries in `ground/results/2026-10-16/endurance/summary.txt` |
| A14.3 | Test-14 Upload and delivery | Delivery during a trial refused 0x07 | REQ-050 | SCRIPTED | NOT RUN | PLANNED-2 | B5.11 |
| A14.4 | Test-14 Upload and delivery | A cancelled update leaves working firmware running and frees the storage | REQ-051, REQ-052 | SCRIPTED | NOT RUN | NEEDS TOOL | the console has no CANCEL (`src/ground/console.py:1110-1125`); D4 tool T1 |
| A14.5 | Test-14 Upload and delivery | Storage freed when an update ends | REQ-052 | — | PASS | — | built at df3aced; `ground/results/2026-10-16/already-done-short-timeout.log`: `the delivered image 40 was deleted from storage` on EVENT 0x0007 (observation O-2 for a restart in between) |
| A15.1 | Test-15 Limits | SET LIMITS changes the minimum version and controller identity after an ARM | REQ-053 | — | NOT RUN | NOT BUILT-OPEN | `src/onboard-computer/updater/image_checker.hpp:29-31` |
| A16.1 | Test-16 Command intake | Frames not encrypted with the right key or with a bad signature refused | REQ-064, REQ-127, REQ-129 | — | NOT RUN | NOT BUILT-ACCEPTED | `src/onboard-computer/updater/command_gate.hpp:11-13` |
| A16.2 | Test-16 Command intake | Old command sent again refused 0x04 | REQ-065 | SCRIPTED | NOT RUN | NEEDS TOOL | D4 tool T1; ARM and dangerous commands only (`command_gate.hpp:14-15`) |
| A16.3 | Test-16 Command intake | Past its expiry refused | REQ-066 | — | NOT RUN | NOT BUILT-ACCEPTED | `src/onboard-computer/updater/command_gate.hpp:19-20` |
| A16.4 | Test-16 Command intake | Commands not from the radio link refused | REQ-067 | — | NOT RUN | NOT BUILT-ACCEPTED | `src/onboard-computer/updater/command_gate.hpp:21` |
| A16.5 | Test-16 Command intake | A dangerous command without a valid ARM is not passed on | REQ-112 | SCRIPTED | NOT RUN | NEEDS TOOL | D4 tool T1 |
| A17.1 | Test-17 Scheduled commands | Passed on at its run time, once, across a restart and a power cut; a changed held command not passed on | REQ-068, REQ-069, REQ-136 | — | NOT RUN | NOT BUILT-OPEN | no SCHEDULE in `src/onboard-computer/updater/protocol.hpp:20-35` |
| A18.1 | Test-18 Report store | Every report numbered | REQ-071 | SCRIPTED | PASS | — | `onboard-computer/results/2026-10-15/phase0-status-console.log` (#988 to #991) |
| A18.2 | Test-18 Report store | Every report time-tagged | REQ-070 | SCRIPTED | PASS | — | same log (onboard t=946685803 to 946685806) |
| A18.3 | Test-18 Report store | A damaged report is caught on arrival | REQ-079 | — | NOT RUN | NEEDS TOOL | needs the link tool of Verification Plan Sec. 3.0 on data link A |
| A18.4 | Test-18 Report store | Stored between contacts and through power cuts; events first; oldest HEALTH dropped first; count and span reported | REQ-074 to REQ-078 | — | NOT RUN | NOT BUILT-OPEN | `src/onboard-computer/updater/report_store.hpp:14-16` |
| A18.5 | Test-18 Report store | Clock difference reported; a clock correction accepted | REQ-072, REQ-073 | — | NOT RUN | NOT BUILT-OPEN | no SET ONBOARD TIME (0x09) in `protocol.hpp:20-35` |
| A19.1 | Test-19 Data links | With one link cut, all traffic continues on the other | REQ-080 | — | NOT ON BENCH | — | data link B not on the bench |
| A19.2 | Test-19 Data links | Loss and recovery of data link A noticed and logged | REQ-081, REQ-082 | SCRIPTED | NOT RUN | PLANNED-2 | B8.6; to the ground: not built (`controller_watch.hpp:5-7`) |
| A19.3 | Test-19 Data links | With the data links cut, the reprogramming link still works | REQ-083 | SCRIPTED | NOT RUN | PLANNED-2 | data link A cut, then `restore.sh reset` |
| A20.1 | Test-20 Key replacement | New key only in a command proven with the current one; then only the new key; a cut replacement leaves the current key | REQ-084 to REQ-086 | — | NOT RUN | NOT BUILT-OPEN | no REPLACE KEY (0x0C) in `protocol.hpp:20-35`; journal: "keys built in" |
| A21.1 | Test-21 Command log | The ground receives a log of every command acted on | REQ-087 | — | NOT RUN | NOT BUILT-OPEN | no code names REQ-087 in `src/` (searched 15 Oct) |
| A22.1 | Test-22 Recovery | A silent controller is noticed | REQ-061 | OPENOCD | NOT RUN | PLANNED-4 | core halted with the watchdog frozen |
| A22.2 | Test-22 Recovery | A silent controller is brought back, and the attempt recorded | REQ-061 | — | NOT RUN | NOT BUILT-OPEN | `src/onboard-computer/updater/controller_watch.hpp:5-7` |
| A22.3 | Test-22 Recovery | A trial past its time: `restore.sh reset`, then EVENT 0x0106 | REQ-061, REQ-124 | SCRIPTED | FAIL | — | Finding F-2: `ground/results/2026-10-15/trial-forever-decision-h.log` (first reset 150.04 s after the trial start, EVENT 0x0106 reason 0x01; no second reset) |
| A22.4 | Test-22 Recovery | Updater killed: restarted, the update in progress resumes | REQ-062 | SCRIPTED | PASS | — | B1.3, B2.3, B5.3, B8.3 (`ground/results/2026-10-16/kills/`) |
| A22.5 | Test-22 Recovery | Whole onboard computer restarted: the update in progress resumes | REQ-063 | SCRIPTED | NOT RUN | PLANNED-2 | B1.4, B2.4 |
| A23.1 | Test-23 Ground checks | Every release signed twice | REQ-088 | INSPECTION | NOT RUN | PLANNED-2 | header and both signatures of the release images |
| A23.2 | Test-23 Ground checks | Every command signed twice | REQ-088 | — | NOT RUN | NOT BUILT-ACCEPTED | `src/onboard-computer/updater/command_gate.hpp:11-13` |
| A23.3 | Test-23 Ground checks | Malformed commands not sent | REQ-089 | SCRIPTED | NOT RUN | PLANNED-2 | map `ground/test_console.py` cases to this condition |
| A23.4 | Test-23 Ground checks | Releases for another controller, older than the minimum, or not the tested build, not sent | REQ-090 | SCRIPTED | NOT RUN | PLANNED-2 | as A23.3 |
| A23.5 | Test-23 Ground checks | Each signed image passes the onboard computer's checks before it is sent | REQ-114 | SCRIPTED | NOT RUN | PLANNED-2 | as A23.3 |
| A23.6 | Test-23 Ground checks | A planned sequence stops at the first refusal or missing answer | REQ-115, REQ-116 | SCRIPTED | NOT RUN | PLANNED-2 | `update` stopped by a refusal |
| A24.1 | Test-24 Ground display | A damaged report is marked on arrival | REQ-093 | SCRIPTED | NOT RUN | NEEDS TOOL | D4 tool T1 |
| A24.2 | Test-24 Ground display | Every command still waiting for its answers is shown | REQ-094 | SCRIPTED | NOT RUN | PLANNED-2 | — |
| A24.3 | Test-24 Ground display | The running firmware is shown | REQ-095 | SCRIPTED | PASS | — | `onboard-computer/results/2026-10-15/phase0-status-console.log` (slot A, kind 2, version 1) |
| AD1.1 | Demonstration-1 Operator access | Someone without the operator's credentials cannot use the operator interface | REQ-091 | — | NOT RUN | NOT BUILT-OPEN | the console has no credential check (`src/ground/console.py:1097-1125`) |
| AD2.1 | Demonstration-2 Planning | The operator plans commands, uploads and report retrieval | REQ-097 | — | NOT RUN | NOT BUILT-OPEN | as AD1.1 |
| A25.1 | Test-25 Restore | Memory erased: the restore writes only checked images, the last-resort first, and clears the kept data | REQ-054, REQ-057, REQ-118, REQ-120 | SCRIPTED | FAIL | — | Finding F-3 (the kept-data part, REQ-057): `onboard-computer/results/2026-10-16/req-057-restore-keeps-backup.log`; the erased-memory part (brick, then restore) is not run yet |
| A25.2 | Test-25 Restore | Bootloader damaged: the restore brings the controller back | REQ-054 | OPENOCD | NOT RUN | PLANNED-4 | bootloader page erased |
| A25.3 | Test-25 Restore | Restore through reprogramming link B (factory loader) | REQ-122 | — | NOT ON BENCH | — | factory-loader restore not on the bench |
| A25.4 | Test-25 Restore | On order, the bootloader and the main firmware put back | REQ-121 | SCRIPTED | PASS | — | `onboard-computer/results/2026-10-15/restore-full-49314cc.log`: 17 steps done in 4.154 s; images 49314cc; restore.sh d95a67a |
| A25.5 | Test-25 Restore | The onboard computer restarts the controller | REQ-124 | SCRIPTED | NOT RUN | PLANNED-2 | `restore.sh reset` |
| A25.6 | Test-25 Restore | Its copy of the last-resort firmware and the links checked between uses | REQ-059 | — | NOT RUN | NOT BUILT-OPEN | no code names REQ-059 in `src/` (searched 15 Oct) |
| A25.7 | Test-25 Restore | The ground told the result of a restore | REQ-060 | — | NOT RUN | NOT BUILT-OPEN | no RESTORE command in `protocol.hpp:20-35`; only EVENT 0x0106 after a trial overrun |
| AD3.1 | Demonstration-3 Restore on order | The operator orders a restore while firmware runs, and it completes | REQ-055 | SCRIPTED | NOT RUN | PLANNED-2 | mechanism only: `restore.sh full` by ssh; the RESTORE command is not built |
| A26.1 | Test-26 Interfaces | Every message matches the Interface Design byte for byte | REQ-107 | SCRIPTED | NOT RUN | PLANNED-2 | frames recorded on both links, decoded against `docs/interfaces` |
| A26.2 | Test-26 Interfaces | A frame with any byte changed is dropped: radio link | REQ-107 | SCRIPTED | NOT RUN | NEEDS TOOL | D4 tool T1 |
| A26.3 | Test-26 Interfaces | A frame with any byte changed is dropped: data link A | REQ-107 | PROBE | NOT RUN | EARLIER BUILD | `controller/results/2026-10-12/step-2a-link-probe.log` |
| A27.1 | Test-27 Fault injection | Every fault of the Table of faults injected, handled as its scenario says | REQ-105 | — | NOT RUN | PLANNED-2 | status follows the rows in "Test-27 and Test-28 map" |
| A28.1 | Test-28 Scenario runs | Every ConOps scenario run from start to end | — | — | NOT RUN | PLANNED-2 | status follows the rows in "Test-27 and Test-28 map" |
| A29.1 | Test-29 Hardware measurements | Two signatures checked at start within the restart time | REQ-023 | SCRIPTED | NOT RUN | EARLIER BUILD | 68.0 to 68.1 ms, `controller/results/2026-10-13/README.md` (5e9bdbc); limit TBD-restart |
| A29.2 | Test-29 Hardware measurements | Software cannot stop the watchdog | Architecture Sec. 8.0 | OPENOCD | NOT RUN | PLANNED-4 | — |
| A29.3 | Test-29 Hardware measurements | The controller clock runs from its own source, at a measured rate | Architecture Sec. 8.0 | SCRIPTED | NOT RUN | PLANNED-2 | A12.1 |
| A29.4 | Test-29 Hardware measurements | The debug port reaches all memory with the controller held in reset | Architecture Sec. 8.0 | SCRIPTED | PASS | — | `onboard-computer/results/2026-10-15/restore-full-49314cc.log`: written and verified with the chip held in reset |
| A29.5 | Test-29 Hardware measurements | The factory loader reaches all memory with the controller held in reset | Architecture Sec. 8.0 | — | NOT ON BENCH | — | factory-loader restore not on the bench |
| AI1 | Inspection-1 Separate code | Link maps and source lists share no file, library or symbol | REQ-009, REQ-099, REQ-119, REQ-123, REQ-126, REQ-130 | INSPECTION | NOT RUN | DESK | known bench limit against REQ-119: `src/README.md` ("share controller/common") |
| AI2 | Inspection-2 Records | Logs hold every command and answer; ground archive holds every report; the key copy restores the keys | REQ-036, REQ-092, REQ-096 | INSPECTION | NOT RUN | DESK | — |
| AI3 | Inspection-3 Shared list | Ground and onboard software take commands and refusal reasons from one source file | REQ-100 | INSPECTION | NOT RUN | DESK | — |
| AI4 | Inspection-4 Operating builds | No fault-injection function in the operating builds | REQ-101 | INSPECTION | NOT RUN | DESK | symbol scan not yet in the repo (code rule D4) |
| AI5 | Inspection-5 Code rules | Complexity 15 or lower, coding standard, no open static-analysis finding | REQ-109 to REQ-111 | INSPECTION | NOT RUN | DESK | static analysis not yet in the repo (code rule D3) |
| AI6 | Inspection-6 Ways back in | No firmware writes the option bytes or boot settings; the loader driver never sends Readout or Write Protect | REQ-125 | INSPECTION | NOT RUN | DESK | — |
| AI7 | Inspection-7 Keys and links | Three keys in three places; two data links of different kinds | REQ-128, REQ-138 | INSPECTION | NOT RUN | DESK | — |
| AI8 | Inspection-8 Hardware documents | Every hardware need met by the datasheets, with pages | Architecture Sec. 8.0 | INSPECTION | NOT RUN | DESK | — |
| AI9 | Inspection-9 Flight reprogramming code | No general-purpose debug tool in the onboard computer's flight software | REQ-141 | INSPECTION | NOT RUN | DESK | bench stand-in OpenOCD accepted (commit b5329d7) |
| AA1 | Analysis-1 Fault tolerance | Hazard analysis re-run on the built System | REQ-098, REQ-103 | ANALYSIS | NOT RUN | DESK | — |
| AA2 | Analysis-2 Response times | Each response time of Test-27 shorter than the time to the hazard | REQ-104 | ANALYSIS | NOT RUN | DESK | needs the Test-27 times |
| AA3 | Analysis-3 Restore independence | The restore uses no controller software | REQ-056 | ANALYSIS | NOT RUN | DESK | — |
| AA4 | Analysis-4 Coverage | 100 % modified condition/decision coverage of the safety-critical software | REQ-108 | ANALYSIS | NOT RUN | DESK | no coverage tool in the repo |

### Test-27 and Test-28 map

Each fault of the Verification Plan's Table of faults, and each ConOps scenario, with the rows
that cover it. Their status is the status of those rows.

| Fault (Verification Plan Table of faults) or scenario | Rows |
|---|---|
| Radio link cut during an upload; Scenario-200 | B1.5, B1.7 |
| Long gap fills the report storage; Scenario-210 | A18.4 (NOT BUILT) |
| Forged command, or old command sent again; Scenario-300 | A16.1 (NOT BUILT), A16.2 |
| Firmware damaged in transfer; Scenario-400 | B1.8 |
| Firmware not released by the operator; Scenario-410 | B1.9 |
| Firmware older than the minimum version; Scenario-420 | A13.3 (NOT BUILT) |
| New firmware during a trial; Scenario-425 | B5.11 |
| New firmware hangs; Scenario-430 | B5.12 |
| New firmware restarts over and over; Scenario-440 | B5.2, B5.12 |
| New firmware fails a trial test; Scenario-445 | B6.8 |
| New firmware never confirmed; Scenario-450 | A4.1 (PASS), A22.3 |
| Previous firmware also unusable; Scenario-460 | A6.2 |
| Operator cancels an update; Scenario-470 | A14.4 |
| No firmware can run; Scenario-480 | A25.1 |
| One data link cut, then restored; both cut; Scenario-500 to 510 | A19.1 (NOT ON BENCH), B8.6, A19.3 |
| Controller goes silent; Scenario-520 | A22.1 |
| Program on the onboard computer stopped; Scenario-530 | B2.3 |
| Whole onboard computer restarted; Scenario-535 | B5.4, B8.4 |
| Power cut during a firmware write; Scenario-600 | B2.1 |
| Power cut during a trial; Scenario-605 | B5.1 |
| Bit changed in stored firmware; Scenario-610 | B3.8 |
| Bit changed in the memory the controller runs from; Scenario-620 | B8.8, B8.12 |
| Scenario-100 Firmware update | A1.2, A1.3, A4.4 (PASS) |
| Scenario-110 Telemetry between contacts | A18.4 (NOT BUILT) |
| Scenario-120 Command during a contact | A10.1 |
| Scenario-130 Scheduled command | A17.1 (NOT BUILT) |
| Scenario-310 to 350 Operator errors | A9.1 to A9.7 |
| Scenario-360 Firmware for a different controller | A13.4 |
| Scenario-370 Key replacement | A20.1 (NOT BUILT) |

## Part B — update moments and faults

Moments: 1 upload (Mac to MP257); 2 deliver (flash write in the controller); 3 ARM and SWITCH;
4 bootloader counts the trial start (flash write); 5 trial running; 6 test image during the
trial; 7 confirm (boot-state flash write); 8 OPERATE; 9 restore.

Faults: 1 controller power cut; 2 controller reset; 3 updater killed; 4 MP257 restart;
5 Mac to MP257 link lost; 6 MP257 to controller link lost, power kept; 7 radio loss;
8 corrupted image; 9 forged image; 10 command sent twice; 11 command in the wrong mode,
expired or not armed; 12 new firmware hangs; 13 new firmware runs but cannot receive an update.

Cell Bm.f is moment m, fault f.

How some faults are caused (no flight code changed):
- Updater killed: `kill` of the exact MainPID read from `systemctl show -p MainPID --value
  fsir-updater` at that moment, announced to the owner first (D7).
- Data link A lost, power kept: the ST-LINK's serial interface unbound from the `cdc_acm` driver
  in sysfs on the MP257; SWD and the Nucleo's power stay up.
- Mac to MP257 link lost: the MP257's USB network interface taken down for a set time by a
  command detached from the ssh session.
- Controller reset: `restore.sh reset` (OpenOCD `reset run`), or a reset at a breakpoint.

| Cell | Source | Expected | Method | Status | Reason | Evidence, or where the reason is written |
|---|---|---|---|---|---|---|
| B1.1 | REQ-061; Test-8 | The controller restarts on slot A; the upload completes; the updater logs "controller silent", then "heard again" | HAND | NOT RUN | PLANNED-3 | — |
| B1.2 | REQ-061, REQ-124 | As B1.1 | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B1.3 | Test-22; REQ-062 | The updater restarts; the upload resumes from its stored part | SCRIPTED | PASS | — | `ground/results/2026-10-16/kills/k1-upload.log`: killed at offset 16896; 17152 bytes kept on the card; `--offset` refused 0x06 after the restart (no open upload), a plain `upload` resumed at byte 17152 without rewriting, END checks passed (observation O-1) |
| B1.4 | Test-22; REQ-063 | The stored part survives; the upload resumes | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B1.5 | Test-14; REQ-049; Scenario-200 | The upload resumes where it stopped | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B1.6 | — | — | — | N/A | NO TRAFFIC | an upload does not use data link A; the link loss is B8.6 |
| B1.7 | Test-14; REQ-049; Scenario-200 | Resent chunks are answered DONE and not written twice; the image passes UPLOAD END | SCRIPTED | PASS | — | `ground/results/2026-10-15/radio-loss/summary.txt`: loss 0.1 and 0.3, 6 of 6 updates completed (30-189 s); 0.5, 1 of 3 (324 s), 2 stopped after 6 unanswered tries as designed; one trial start and one confirm per completed run; updater e5f46cc6 |
| B1.8 | Test-13; REQ-044; Scenario-400 | Refused 0x08 at UPLOAD END | SCRIPTED | NOT RUN | EARLIER BUILD | `controller/results/2026-10-12/milestone-2-updater.log` (updater 9da60f3) |
| B1.9 | Test-13; REQ-043; Scenario-410 | Refused 0x09 at UPLOAD END | SCRIPTED | NOT RUN | EARLIER BUILD | as B1.8 |
| B1.10 | REQ-031 | A chunk inside the bytes stored is answered DONE without writing | SCRIPTED | NOT RUN | PLANNED-2 | `update_manager.hpp:16-18` |
| B1.11 | REQ-028 | A chunk past the bytes stored refused 0x06 | SCRIPTED | NOT RUN | NEEDS TOOL | D4 tool T1 |
| B1.12 | — | — | — | N/A | NO TRAFFIC | no new firmware runs during an upload |
| B1.13 | — | — | — | N/A | NO TRAFFIC | as B1.12 |
| B2.1 | Test-6; REQ-007; Scenario-600; H-1 control 1 | Slot A runs after power returns; slot A and the last-resort read back unchanged; the update can be repeated | SCRIPTED | PASS | — | `ground/results/2026-10-16/power-cuts/pc1-deliver-1.log`, `-2.log`, `-3.log`: hub power off 1.0, 0.3, 2.3 s into DELIVER; slot B ran again each time; slot B and the last-resort read back byte-identical over SWD; the repeated delivery completed |
| B2.2 | Test-6; REQ-007 | As B2.1 | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B2.3 | Test-22; REQ-062; Scenario-530 | The updater restarts; the update resumes or is reported | SCRIPTED | PASS | — | `ground/results/2026-10-16/kills/k2-deliver.log`: killed 1.4 s into DELIVER; restarted; the console's resend after 30 s ran the delivery again; the controller accepted UPLOAD END; 33.1 s in all |
| B2.4 | Test-22; REQ-063; Scenario-535 | As B2.3; the controller keeps slot A running | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B2.5 | REQ-031, REQ-049 | The delivery completes; the resent DELIVER runs again (only ARM and dangerous commands are recognised as repeats) | SCRIPTED | NOT RUN | PLANNED-2 | `command_gate.hpp:14-19` |
| B2.6 | REQ-025, REQ-081 | The delivery fails and is reported; slot A keeps running; a later delivery succeeds | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B2.7 | REQ-049 | As B2.5 | SCRIPTED | PASS | — | `ground/results/2026-10-15/radio-loss/summary.txt`: loss 0.1 and 0.3, 6 of 6 updates completed (30-189 s); 0.5, 1 of 3 (324 s), 2 stopped after 6 unanswered tries as designed; one trial start and one confirm per completed run; updater e5f46cc6 |
| B2.8 | Test-13; REQ-047 | The stored image changed after UPLOAD END is refused before delivery | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B2.9 | Test-13; REQ-043, REQ-047 | A stored image replaced by a forged one after UPLOAD END is refused 0x09 before delivery | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B2.10 | REQ-031 | A second DELIVER during the first does not corrupt the delivery | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B2.11 | Scenario-310; REQ-027 | ARM and SWITCH during a delivery are refused, or the bootloader refuses the half-written slot and slot A keeps running | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B2.12 | — | — | — | N/A | NO TRAFFIC | the new firmware does not run yet |
| B2.13 | — | — | — | N/A | NO TRAFFIC | as B2.12 |
| B3.1 | Test-5; REQ-007; code rule B7 | Power cut between the data and the check-value writes of the SWITCH boot-state write: the boot state stays valid (slot A, or trial B) | OPENOCD | NOT RUN | PLANNED-4 | reset at a breakpoint stands in for the power cut (D6) |
| B3.2 | REQ-029 | Reset between ARM and SWITCH: SWITCH refused 0x01 or done; never half done | OPENOCD | NOT RUN | PLANNED-4 | — |
| B3.3 | REQ-062, REQ-112 | Updater killed between ARM and SWITCH: the held ARM is lost; SWITCH refused 0x01; a new ARM and SWITCH succeed | SCRIPTED | NOT RUN | NEEDS TOOL | D4 tool T1 (gap between ARM and SWITCH) |
| B3.4 | REQ-063, REQ-112 | As B3.3 with an MP257 restart | SCRIPTED | NOT RUN | NEEDS TOOL | D4 tool T1 |
| B3.5 | REQ-030; Scenario-335 | Link down longer than 30 s after ARM: SWITCH refused 0x02 | SCRIPTED | NOT RUN | NEEDS TOOL | D4 tool T1 |
| B3.6 | REQ-081, REQ-025 | SWITCH not delivered; the ground told; slot A keeps running | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B3.7 | REQ-031; Scenario-350 | Resent ARM and SWITCH run once; answers ALREADY DONE | SCRIPTED | NOT RUN | NEEDS DECISION | radio-loss levels and short-timeout run (D3) |
| B3.8 | Test-3; REQ-005; Scenario-610 | Bit changed in slot B after delivery: the bootloader refuses slot B, gives up the trial, slot A runs | OPENOCD | NOT RUN | PLANNED-4 | — |
| B3.9 | Test-3; REQ-006 | Slot B overwritten with an image signed by an unknown key: refused, slot A runs | OPENOCD | NOT RUN | PLANNED-4 | — |
| B3.10 | Test-9; REQ-031 | SWITCH sent again: ALREADY DONE; one restart, one trial start | SCRIPTED | PASS | — | `ground/results/2026-10-16/already-done-short-timeout.log`: SWITCH sent 3 times 50 ms apart, passed on once, one EVENT 0x0006 (the ALREADY DONE answer itself is not in the log; host test testRepeatController covers it) |
| B3.11 | Test-9; REQ-029, REQ-030, REQ-027 | SWITCH not armed 0x01; arm expired 0x02; in TRIAL 0x05 | SCRIPTED | NOT RUN | NEEDS TOOL | D4 tool T1; earlier `controller/results/2026-10-13/step-2d-trial-confirm.log` (0x01) |
| B3.12 | — | — | — | N/A | NO TRAFFIC | the new firmware starts at moment 4 |
| B3.13 | — | — | — | N/A | NO TRAFFIC | as B3.12 |
| B4.1 | Test-5; REQ-016, REQ-017; code rule B7 | Cut between the data and the check-value writes of the start count: the count survives; the boot state stays valid | OPENOCD | NOT RUN | PLANNED-4 | a log-triggered power cut lands about 0.5 s after the command is passed on (journal 0.30 s, uhubctl 0.19 s; `ground/results/2026-10-16/power-cuts/pc4-switch-count-write.log`), after the 25 ms write; needs the breakpoint design (D6) or a trigger without the log |
| B4.2 | Test-5; REQ-017 | As B4.1 | OPENOCD | NOT RUN | PLANNED-4 | — |
| B4.3 | REQ-062 | Updater killed while the controller restarts into its trial: the new updater starts its trial watch from HEALTH TRIAL | SCRIPTED | NOT RUN | PLANNED-2 | `trial_watch.hpp:5-9` |
| B4.4 | REQ-024, REQ-063 | As B4.3 with an MP257 restart | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B4.5 | — | — | — | N/A | NO TRAFFIC | the bootloader uses no ground traffic; link loss during the trial is B5.5 |
| B4.6 | — | — | — | N/A | NO TRAFFIC | the bootloader does not use data link A for the count |
| B4.7 | — | — | — | N/A | NO TRAFFIC | as B4.5 |
| B4.8 | REQ-005; Scenario-610 | Bit changed in slot B between the SWITCH write and the bootloader's check: slot B refused, slot A runs | OPENOCD | NOT RUN | PLANNED-4 | — |
| B4.9 | — | — | — | N/A | DUPLICATE | a forged image reaches slot B only before SWITCH: B3.9 |
| B4.10 | — | — | — | N/A | NO TRAFFIC | the controller is in its bootloader and takes no commands |
| B4.11 | — | — | — | N/A | NO TRAFFIC | as B4.10 |
| B4.12 | — | — | — | N/A | DUPLICATE | the hang starts after the count: B5.12 |
| B4.13 | — | — | — | N/A | DUPLICATE | B5.13 |
| B5.1 | Test-5; REQ-016, REQ-017; Scenario-605 | Each cut counts as a failed start; after 3, slot A runs; the count survives | SCRIPTED | PASS | — | `ground/results/2026-10-16/power-cuts/pc3-trial-three-cuts.log`: three hub power cuts in one trial; failed starts 1, 2, 3; after the third, OPERATE on slot B; slot B and the last-resort identical (observations O-3, O-4) |
| B5.2 | Test-5; REQ-015; Scenario-440 | As B5.1 with resets | SCRIPTED | PASS | — | `ground/results/2026-10-15/trial-forever-decision-h.log` (resets by restore.sh reset at 20:28:33 and 20:28:41 UTC) |
| B5.3 | REQ-062 | The updater restarts; the trial ends by itself (confirm or give-up) and is seen | SCRIPTED | PASS | — | `ground/results/2026-10-16/kills/k3-trial.log`: killed 5 s into the trial; CONFIRM through the restarted updater: EVENT 0x0007, OPERATE slot A (observation O-2: the delivered image is not freed) |
| B5.4 | Test-8; REQ-024; Scenario-535 | The controller keeps its trial running (controller clock continuous); the trial ends correctly | SCRIPTED | NOT RUN | PLANNED-2 | the Nucleo is powered from the MP257's USB; a restart may cut it (observation) |
| B5.5 | Scenario-450; REQ-013 | No CONFIRM arrives: the trial is given up at 120 s; a CONFIRM after the trial time refused | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B5.6 | REQ-025, REQ-013 | Test image not received: trial test fails or the trial runs out; slot A runs | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B5.7 | — | — | — | N/A | DUPLICATE | radio loss touches only the CONFIRM: B7.7 |
| B5.8 | REQ-041; Scenario-620 | Bit changed in the running slot B: detected by the self-test at the next start; slot B not confirmed | OPENOCD | NOT RUN | PLANNED-4 | — |
| B5.9 | — | — | — | N/A | DUPLICATE | no image reaches the controller during a trial (B5.11) |
| B5.10 | — | — | — | N/A | DUPLICATE | the command of this moment is CONFIRM: B7.10 |
| B5.11 | Test-14; REQ-050; Scenario-425 | DELIVER during the trial refused 0x07 | SCRIPTED | NOT RUN | PLANNED-2 | earlier `controller/results/2026-10-12/step-2c-cpp-upload-probe-in-trial.log` |
| B5.12 | Test-4, Test-5; REQ-014, REQ-015; Scenario-430, 440 | `slotB-v2-hang.img` delivered and switched: 3 watchdog restarts, trial given up, slot A runs, the reports name slot A | SCRIPTED | NOT RUN | PLANNED-2 | earlier `controller/results/2026-10-13/scenario-2.log` |
| B5.13 | H-3; REQ-058, REQ-121 | The test application (no data link) confirms itself: the controller goes silent; only the restore brings working firmware back | SCRIPTED | NOT RUN | PLANNED-2 | only after a restore has passed this session |
| B6.1 | REQ-016, REQ-139 | — | HAND | NOT RUN | PLANNED-4 | the transfer lasts 136 ms (tom-y-n:112-113); not reachable by hand; B6.2 stands in |
| B6.2 | REQ-016, REQ-139 | Reset during the test image: a failed start counted; no slot written; the trial restarts or is given up | OPENOCD | NOT RUN | PLANNED-4 | — |
| B6.3 | REQ-062, REQ-139 | Updater killed during the test image: the trial ends correctly (test failed or test image sent again) | SCRIPTED | NOT RUN | NEEDS TOOL | `ground/results/2026-10-16/kills/k4-test-image.log`: the kill landed 0.45 s after the log match, 0.32 s after the test image ended; the trigger is slower than the 136 ms window; needs a kill of a PID read in advance, without the log in the loop |
| B6.4 | REQ-063, REQ-139 | As B6.3 with an MP257 restart | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B6.5 | — | — | — | N/A | NO TRAFFIC | the onboard computer sends the test image without the ground (tom-y-n:112) |
| B6.6 | REQ-140, REQ-013 | Link cut during the test image: trial test fails or the trial runs out; slot A runs | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B6.7 | — | — | — | N/A | NO TRAFFIC | as B6.5 |
| B6.8 | Test-4; REQ-140; Scenario-445 | Test image with a wrong check value: refused 0x08, EVENT 0x000B, given up at once | PROBE | NOT RUN | EARLIER BUILD | `controller/results/2026-10-13/step-2d-trial-bad-image.log` (5e9bdbc) |
| B6.9 | — | — | — | N/A | NO TRAFFIC | the test image carries a check value only, no signature (`docs/interfaces/main.tex:333-337`) |
| B6.10 | REQ-031 | A resent chunk of the test image is not taken twice | — | NOT RUN | NEEDS TOOL | needs the link tool of Verification Plan Sec. 3.0 |
| B6.11 | REQ-027 | A test image (number 0xFFFF) in OPERATE refused | PROBE | NOT RUN | PLANNED-2 | Nucleo on the Mac (D5) |
| B6.12 | — | — | — | N/A | DUPLICATE | a hanging firmware takes no test image: B5.12 |
| B6.13 | — | — | — | N/A | DUPLICATE | B7.13 |
| B7.1 | Test-5; REQ-012; code rule B7 | Cut between the data and the check-value writes of CONFIRM: the boot state stays valid (trial B or confirmed B) | OPENOCD | NOT RUN | PLANNED-4 | a log-triggered power cut lands about 0.5 s after the command is passed on (journal 0.30 s, uhubctl 0.19 s; `ground/results/2026-10-16/power-cuts/pc4-switch-count-write.log`), after the 25 ms write; needs the breakpoint design (D6) or a trigger without the log |
| B7.2 | REQ-012 | As B7.1 | OPENOCD | NOT RUN | PLANNED-4 | — |
| B7.3 | REQ-062, REQ-112 | Updater killed between ARM and CONFIRM: CONFIRM refused 0x01; a new ARM and CONFIRM succeed within the trial time | SCRIPTED | NOT RUN | NEEDS TOOL | D4 tool T1 |
| B7.4 | REQ-063 | CONFIRM after the MP257 is back, within the trial time: DONE | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B7.5 | REQ-031 | Link down while CONFIRM is resent: confirmed once; EVENT 0x0007 once | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B7.6 | REQ-081 | CONFIRM cannot be passed on: the ground told; the trial runs out; slot A runs | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B7.7 | REQ-031; Scenario-350 | Resent ARM and CONFIRM run once; answers ALREADY DONE | SCRIPTED | NOT RUN | NEEDS DECISION | D3 |
| B7.8 | REQ-005; Scenario-610 | Bit changed in slot B before CONFIRM: confirmed now; at the next start slot B refused and slot A runs | OPENOCD | NOT RUN | PLANNED-4 | — |
| B7.9 | — | — | — | N/A | DUPLICATE | B7.8 (any change to slot B is caught by the same start check) |
| B7.10 | Test-9; REQ-031 | CONFIRM sent again: ALREADY DONE; one EVENT 0x0007 | SCRIPTED | PASS | — | `ground/results/2026-10-16/already-done-short-timeout.log`: CONFIRM sent 3 times 20 ms apart, passed on once, one EVENT 0x0007 (answer kind as for B3.10) |
| B7.11 | Test-4, Test-9; REQ-012, REQ-027, REQ-029, REQ-030 | CONFIRM not armed 0x01; arm expired 0x02; in OPERATE 0x05; before the tests 0x0F; after the trial time refused | SCRIPTED | NOT RUN | NEEDS TOOL | D4 tool T1; earlier `controller/results/2026-10-13/step-2d-trial-confirm.log` |
| B7.12 | — | — | — | N/A | DUPLICATE | a hanging firmware never reaches CONFIRM: B5.12 |
| B7.13 | H-3 control 2; REQ-139 | A firmware that answers but cannot take the test image fails its trial test and cannot be confirmed (0x0F) | — | NOT RUN | NEEDS TOOL | needs a test firmware with a broken update receiver (D4 tool T2) |
| B8.1 | REQ-017, REQ-022 | Slot A runs again after power returns; boot state unchanged | SCRIPTED | PASS | — | `onboard-computer/results/2026-10-15/power-switch-check.log`: hub port off 5 s; controller clock 15081 s to 6 s; OPERATE slot A, failed starts 0 |
| B8.2 | REQ-023, REQ-124 | Slot A runs again; restart time measured | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B8.3 | Test-22; REQ-062; Scenario-530 | The updater restarts; the controller is not affected (controller clock continuous) | SCRIPTED | PASS | — | `ground/results/2026-10-16/kills/k5-operate.log`: PID 17465 killed, restarted in 1.17 s; controller clock 45 s to 71 s in 26 s (no restart); all 4d6f5e8 |
| B8.4 | Test-8; REQ-024; Scenario-535 | The controller keeps running (controller clock continuous) | SCRIPTED | NOT RUN | PLANNED-2 | the Nucleo is powered from the MP257's USB (observation) |
| B8.5 | Test-8 | Nothing changes onboard; reports while the link is down are lost (bench limit) | SCRIPTED | NOT RUN | PLANNED-2 | `report_store.hpp:14-15` |
| B8.6 | Test-8, Test-19; REQ-025, REQ-081, REQ-082 | The controller keeps running; "controller silent", then "controller heard again" | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B8.7 | Test-14 | Commands resent and answered; nothing runs twice | SCRIPTED | PASS | — | `ground/results/2026-10-15/radio-loss/summary.txt`: loss 0.1 and 0.3, 6 of 6 updates completed (30-189 s); 0.5, 1 of 3 (324 s), 2 stopped after 6 unanswered tries as designed; one trial start and one confirm per completed run; updater e5f46cc6 |
| B8.8 | Test-11; REQ-041; Scenario-610, 620 | Bit changed in running slot A: at the next start, refused; slot B or the last-resort runs; reported | OPENOCD | NOT RUN | PLANNED-4 | — |
| B8.9 | — | — | — | N/A | DUPLICATE | a forged image is refused at UPLOAD END (B1.9) or at the start check (B3.9) |
| B8.10 | Test-9; REQ-031 | ARM sent again: ALREADY DONE | SCRIPTED | NOT RUN | PLANNED-2 | other commands: A9.8 (NOT BUILT) |
| B8.11 | Test-1, Test-9; REQ-019, REQ-027; Scenario-310 | CONFIRM in OPERATE refused 0x05; SWITCH with no new delivery since the given-up trial refused | SCRIPTED | FAIL | — | Finding F-1 (the SWITCH part); the CONFIRM-in-OPERATE part is not run yet |
| B8.12 | Test-7; REQ-022, REQ-023; Scenario-620 | Core halted with the watchdog running: restart, slot A resumes; restart time measured | OPENOCD | NOT RUN | PLANNED-4 | — |
| B8.13 | REQ-058, REQ-121 | A confirmed firmware that cannot take an update is replaced through the restore | SCRIPTED | NOT RUN | PLANNED-2 | follows B5.13 |
| B9.1 | REQ-054, REQ-056 | Power cut during `restore.sh full`: RESTORE FAILED; a second run passes | HAND | NOT RUN | PLANNED-3 | — |
| B9.2 | REQ-056 | Reset button during the restore: RESTORE FAILED or the verify catches it; a second run passes | HAND | NOT RUN | PLANNED-3 | — |
| B9.3 | REQ-062 | `restore.sh` killed (exact PID) during the write: a second run passes | SCRIPTED | NOT RUN | PLANNED-2 | D7 |
| B9.4 | REQ-063 | MP257 restarted during the restore: a second run passes | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B9.5 | REQ-060 | Mac link lost during the restore: the restore completes; its result line is kept | SCRIPTED | NOT RUN | PLANNED-2 | — |
| B9.6 | — | — | — | N/A | NO TRAFFIC | the restore uses SWD, not data link A |
| B9.7 | — | — | — | N/A | NO TRAFFIC | no ground traffic during a restore (no RESTORE command) |
| B9.8 | REQ-118 | Damaged image given to the restore: refused before the first OpenOCD step | SCRIPTED | NOT RUN | PLANNED-2 | host: `onboard-computer` restore test 372/0 (15 Oct) |
| B9.9 | REQ-118 | Forged image given to the restore: refused before the first OpenOCD step | SCRIPTED | NOT RUN | PLANNED-2 | as B9.8 |
| B9.10 | — | — | — | N/A | NO TRAFFIC | no RESTORE command to send twice (A25.7) |
| B9.11 | — | — | — | N/A | NO TRAFFIC | the restore works in every mode (`docs/interfaces/main.tex:351`) |
| B9.12 | — | — | — | N/A | NO TRAFFIC | no new firmware runs during a restore |
| B9.13 | — | — | — | N/A | NO TRAFFIC | as B9.12 |

## Findings

| No. | Row | Expected (source) | Actual | Reproduce | Evidence | State |
|---|---|---|---|---|---|---|
| F-1 | A5.4, B8.11 | A SWITCH when no new firmware has arrived is refused with the reason (ConOps Scenario-310; REQ-019, REQ-027) | SWITCH answered DONE; the given-up firmware started a new trial (HEALTH TRIAL, slot B, failed starts 1) and passed its trial tests | deliver a firmware to slot B, `switch`, `return` (it is given up), then `switch` again with no delivery | `ground/results/2026-10-15/finding-37-switch-to-given-up.log`; cause by reading: `mode_manager.cpp:648-672` checks only that slot B holds a valid image | OPEN: fix accepted as decision 30 |
| F-2 | A22.3 | A trial not confirmed in time is given up and the previous firmware runs (REQ-013; decision H: the onboard computer resets an overrunning trial) | The onboard computer reset the controller once, 150.04 s after the trial started; the firmware came back still on trial (failed starts 2) and the onboard computer did not reset it again: 723 s later it was still in TRIAL on slot B | deliver `slotB-v2-trial-forever.img`, `switch`, wait 5 minutes | `ground/results/2026-10-15/trial-forever-decision-h.log`; cause by reading: `trial_watch.cpp:72-79` stops timing after the first overrun, and only EVENT 0x0006 re-arms it (`:51-57`); a firmware that does not send 0x0006 is never timed again | OPEN: fix is the owner's decision |
| F-3 | A25.1 | The restore clears the data the controller keeps over restarts (REQ-057) | A fault-location record planted in the backup registers before `restore.sh full` was reported by the controller after the restore as a real fault: EVENT 0x0005 at 0x08, found by the bootloader | over SWD: set PWR_CR1 DBP, write BKP10R 0x00000008 and BKP11R 0xfffffff7, clear DBP; run `restore.sh full`; listen for EVENT 0x0005 | `onboard-computer/results/2026-10-16/req-057-restore-keeps-backup.log`; cause by reading: `restore.sh:583-611` erases the boot state but never the TAMP backup registers, and a reset keeps them | OPEN: fix is the owner's decision |

Legend. State: OPEN (reported, not fixed), FIXED (fix and regression test committed; the commit is named).

Observations (not failures; for the owner):
- O-1: after the updater restarts, the console's first resume advice (`upload --offset N`) is refused 0x06, because an open upload lives only in the updater's memory; its second advice (a plain `upload`) works and keeps the stored bytes (`ground/results/2026-10-16/kills/k1-upload.log`).
- O-2: a delivered image is freed on EVENT 0x0007 or 0x0008 only if the updater did not restart in between: the last delivered image number is held in memory (a TBR choice of decision 22's build) (`ground/results/2026-10-16/kills/k3-trial.log`).
- O-3: the controller's first reports after a power-on are lost on this bench: it boots in about 0.2 s, while the ST-LINK's serial port needs 1 to 2 s to come back and the updater reopens it once a second; EVENT 0x0006 and EVENT 0x0002 after a power cut did not arrive (`ground/results/2026-10-16/power-cuts/pc2-trial-cut-1.log`, `pc3-trial-three-cuts.log`). A bench effect of the USB link; an RS-422 link would not disappear.
- O-4: after a controller restart inside a trial, the updater does not send the test image again, so the trial tests cannot pass and the trial cannot be confirmed; it runs out and is given up (`ground/results/2026-10-16/power-cuts/pc2-trial-summary.log`). Safe in direction; the owner decides whether a trial should survive one restart.
- O-5: one upload of the 100-cycle endurance run took 5.10 s instead of about 0.8 s: the board answered a run of chunks slowly (up to 2.5 s between chunks, `ground/results/2026-10-16/endurance/endurance-log.txt` cycle 100). No resend and no failure; the likely cause, not proven, is the SD card stalling under the image writes and the journal, kept on the card since decision 2.


## Decisions

The owner answered the decision list of 15 Oct 2026 the same evening. Numbers are those of that
list. Answer: ACCEPTED (the recommendation stands), CHANGED (the owner's own choice, stated),
OPEN (not yet answered). Each answer is recorded once, here and in the code it governs.

| No. | Decision | Answer | What applies |
|---|---|---|---|
| 1 | Updater build on the board | ACCEPTED | built at d95a67a (byte-identical in two builds, sha256 `e5f46cc6…6eae`), installed 15 Oct 17:16 UTC; old binary kept as `/usr/local/bin/fsir-updater.e49e7d32` |
| 2 | Board journal | CHANGED | persistent, as asked: `/var/log` moved from tmpfs to the SD card, `/etc/tmpfiles.d/00-create-volatile.conf` drops the volatile link, `/etc/systemd/journald.conf.d/50-fsir-persistent.conf` sets Storage=persistent, SystemMaxUse=200M |
| 3 | Board clock | CHANGED | the owner: it must be calibrated, the ground truth of the satellite. Set from the Mac with `date -s` and `hwclock -w` at 17:16:28 UTC (manual stand-in for SET ONBOARD TIME, REQ-073, not built); drift measured in the overnight run |
| 4 | Overnight run without commands (Test-8) | ACCEPTED | 12 h; every report and every event logged so the run can be read afterwards |
| 5 | Endurance | ACCEPTED | 100 update cycles; fastest, typical, slowest time and pass count |
| 6 | Radio loss | ACCEPTED | loss 0.1, 0.3, 0.5 with seeds 1, 2, 3; `switch` and `confirm` with `--timeout 0.05` |
| 7 | Cut durations | ACCEPTED | data link A 30 s; Mac link 10 s and 30 s |
| 8 | Hand cases | CHANGED | 3 runs each to start; more later |
| 9 | Kills | ACCEPTED | exact MainPID read at that moment; announced first |
| 10 | Power cut at a flash write | CHANGED | the owner: cut power at every possible occasion. A real power cut wherever it can be placed; the reset at a breakpoint only where no cut can hit the moment |
| 11 | Debug probe | ACCEPTED (a) | OpenOCD through the ST-LINK; the shared tool with the restore path is written into the results |
| 12 | Nucleo on the Mac | ACCEPTED | at the end of a bench session |
| 13 | Not-built rows | ACCEPTED | NOT BUILT-ACCEPTED (acceptance on record) and NOT BUILT-OPEN (no decision on record) |
| 14 | RETURN TO PREVIOUS in OPERATE | ACCEPTED (a) | refused 0x05; the Interfaces row 0x21 says TRIAL |
| 15 | RETURN TO PREVIOUS, failed boot-state write | ACCEPTED (a) | REFUSED 0x11, no give-up request |
| 16 | EVENT 0x0008 without a reason | ACCEPTED (a) | no detail byte |
| 17 | Four ground refusal rules | ACCEPTED | 0xFFFF, length 0, offset past the end, reason from the checker's first line |
| 18 | Old upload test meets the image check | ACCEPTED (a) | the test uploads the signed `firmware-B.img` |
| 19 | Ground uses the onboard checker | ACCEPTED | — |
| 44 | Command sender's 60 s gap bound | ACCEPTED | — |
| 20 | Restart a silent controller | CHANGED | 3 resets 60 s apart; if the controller is still silent after the third, the onboard computer restores it (how: proposal awaiting the owner) |
| 21 | CANCEL stops a running delivery | ACCEPTED | new refusal 0x12 "cancelled by the operator" in the Interfaces |
| 22 | Free the storage when an update ends | ACCEPTED | EVENT 0x0007 or 0x0008 for the delivered image frees it |
| 23 | Self-test while running | ACCEPTED | every 60 s (bench value for TBD-selftest); the owner: document it, it is important |
| 24 | Minimum version and SET LIMITS | ACCEPTED | start at 1; lowering needs an ARM; stored with a check value; includes ARM for onboard dangerous commands |
| 25 | Onboard clock by command, and expiry | ACCEPTED | SET ONBOARD TIME stores an offset; difference above 5 s reported; expiry 0 refused; SET ONBOARD TIME exempt from expiry |
| 26 | Repeats recognised for every command | ACCEPTED | 64 entries for ARM and dangerous commands, 256 for the rest; not kept across a restart |
| 27 | Link-loss events to the ground | ACCEPTED | lost = 5 s of silence; link id 0x01 |
| 28 | Controller time | CHANGED | the owner: a real-time clock, so date and time are known and everything is synced; design proposal awaiting the owner |
| 29 | Report storage between contacts | ACCEPTED | 1 MB; events never dropped; oldest HEALTH first; count and span reported |
| 30 | FALLBACK, START FAILED, SWITCH refusing a given-up image | ACCEPTED | boot-state spare fields; leave FALLBACK only by LEAVE FALLBACK, SWITCH, START FAILED; copy codes 0x01 A, 0x02 B |
| 31 | Command checks at the controller | ACCEPTED | Interfaces numbering rule fixed first |
| 32 | Drop scheduled commands and planning from this phase | ACCEPTED | requirements revision |
| 33 | Command encryption and signatures, key replacement | ACCEPTED | replacement covers command and encryption keys, not the image-signing keys |
| 34 | Real last-resort firmware | ACCEPTED | REQ-009 amended to let it read the boot state; built later |
| 35 | Separate the bootloader from the shared code | CHANGED | the owner: separate them now (not kept as a bench limit) |
| 36 | Safe state | ACCEPTED | the controller's safe state is a restart; no outputs until there is equipment |
| 37 | SWITCH back to a given-up firmware | ACCEPTED | measured on the bench, then fixed in 30 |
| 38 | Verification Plan Sec. 7.0 claims | ACCEPTED | corrected |
| 39 | REQ-100, one shared command list | ACCEPTED | bench limit now; generator later |
| 40 | The console's --radio-loss | ACCEPTED | bench limit now; test relay later |
| 41 | REQ-087 and REQ-059 not built | ACCEPTED | bench limits for this phase |
| 42 | Stale comment in command_gate.hpp:19-21 | ACCEPTED | fixed with the next updater change |
| 43 | Bench LEDs | OPEN | the owner: "we will see" |
