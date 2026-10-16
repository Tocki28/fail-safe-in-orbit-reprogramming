# Verification campaign — final report, 15 and 16 October 2026

This report answers one question: which activities of the Verification Plan were measured on the
bench in the campaign of 15 and 16 October 2026, with what result, and what is still open. It is a
summary. Every row, every expected result and every log path is in `test/fault-matrix.md`.

The bench is a lab prototype: the STM32MP257F-DK as onboard computer and the NUCLEO-G431KB as
controller, joined by one USB serial link (data link A). It is not flight hardware.

## Build under test

Final state at the end of the campaign, 16 Oct 2026:

| Unit | Commit |
|---|---|
| Controller (bootloader, firmware A and B) | d3bb74a |
| `restore.sh` (fix of F-3) | ae40af0 |
| Onboard computer updater on the board, sha256 `5f0c5f49…` | bc34e8d |
| Console | bc34e8d, with the name of reason 0x02 from 615a83f (O-8) |

Host suites at 615a83f: controller `make check` exit 0 (mode manager 147, bootloader 37, flow 60,
separation 22, others as their lines say); onboard computer exit 0 (unit 610 of 610, restore test
468 PASS 0 FAIL); ground exit 0 (46 + 41 + 8 + 23 tests OK). The host suites are not counted in
the rows below.

Bench state at the end: OPERATE, slot A. Slot B holds the d3bb74a firmware B marked as given up
(decision 30), so the next update to that same image needs START FAILED.

Builds used during the campaign, in order:

| Unit | Commit | Since |
|---|---|---|
| Controller bootloader, firmware A and B, console, command sender, images | 49314cc | 15 Oct 20:10 UTC, full restore (`test/ground/results/2026-10-15/README.md`) |
| Same, with the separated bootloader | 4d6f5e8 | 15 Oct 21:00 UTC, full restore (`test/ground/results/2026-10-16/README.md`) |
| Onboard computer updater | d95a67a | 15 Oct 17:16 UTC (`test/onboard-computer/results/2026-10-15/README.md`) |
| Onboard computer updater, sha256 `6b0888f8…` | 4d6f5e8 | 15 Oct 21:00 UTC (`test/onboard-computer/results/2026-10-16/README.md`) |
| Controller and console with decision 30 (FALLBACK, START FAILED, LEAVE FALLBACK, SWITCH refuses a given-up image) | d3bb74a | 16 Oct, board start 06:01:56 UTC, full restore (`test/ground/results/2026-10-16/decision-30/decision-30-hardware.log`) |
| `restore.sh`, sha256 `b8b1e5a87142129f…` | ae40af0 | 16 Oct about 06:16 UTC (`test/onboard-computer/results/2026-10-16/f3-restore-clears-backup.log`) |
| Onboard computer updater, sha256 `2b799dc2…` | d12db66 | 16 Oct about 06:55 UTC (`test/onboard-computer/results/2026-10-16/updater-d12db66-on-board.log`) |
| Onboard computer updater, sha256 `5f0c5f49…` (decision 29 added) | bc34e8d | 16 Oct 07:25 UTC (`test/onboard-computer/results/2026-10-16/updater-bc34e8d-on-board.log`) |

Every run of 16 Oct up to 08:39 EEST ran on 4d6f5e8; the decision 30 runs (09:01 to 09:09 EEST) ran on d3bb74a; the later runs ran with `restore.sh` ae40af0 and the updater d12db66 on the d3bb74a controller images. Rows whose evidence is from an earlier commit are NOT RUN
with the reason EARLIER BUILD.

## Counts

Source: `test/fault-matrix.md`, section "Counts" (269 rows: Part A 133, Part B 117, Part C 19).
Part C holds the stress audit of 16 Oct 2026 (section "Stress audit, 16 October 2026" below).
Before the stress audit the 250 rows of Parts A and B counted PASS 80, FAIL 0, NOT RUN 131,
NOT ON BENCH 7 and N/A 32.

| Status | Part A | Part B | Part C | Total |
|---|---|---|---|---|
| PASS | 59 | 28 | 9 | 96 |
| FAIL | 0 | 1 | 7 | 8 |
| NOT RUN | 69 | 54 | 2 | 125 |
| NOT ON BENCH | 5 | 2 | 0 | 7 |
| N/A | 0 | 32 | 1 | 33 |

## Findings

Before the stress audit no row was FAIL. The three findings F-1, F-2 and F-3 are fixed and measured on the bench. Each finding is described
in full under "Findings" in `test/fault-matrix.md`. The FAIL rows of the stress audit and its
findings ST-001 to ST-017 are in the section "Stress audit, 16 October 2026" below.

| No. | Rows | Reproduce | State |
|---|---|---|---|
| F-1 | A5.4, B8.11 (both now PASS) | Deliver a firmware to slot B, `switch`, `return` (it is given up), then `switch` again with no delivery. Expected: refused. Measured: DONE and a new trial of the given-up firmware (`test/ground/results/2026-10-15/finding-37-switch-to-given-up.log`). | FIXED: decision 30, commit d3bb74a; SWITCH to a given-up image REFUSED 0x13 on the bench (`test/ground/results/2026-10-16/decision-30/decision-30-hardware.log`, steps C, D, F) |
| F-2 | A22.3 (now PASS) | Deliver `slotB-v2-trial-forever.img`, `switch`, wait 5 minutes. Expected: trial given up. Measured: one reset 150.04 s after the trial start, no second reset, still in TRIAL 723 s later (`test/ground/results/2026-10-15/trial-forever-decision-h.log`). | FIXED: decision 45, updater d12db66; 3 trial-watch resets 150 s apart, then the bootloader's give-up and FALLBACK, slot A (`test/onboard-computer/results/2026-10-16/updater-d12db66-on-board.log`, Part 5) |
| F-3 | A25.1 (now PASS) | Over SWD set PWR_CR1 DBP, write BKP10R 0x00000008 and BKP11R 0xfffffff7, clear DBP; run `restore.sh full`; listen for EVENT 0x0005. Expected: restore clears the kept data (REQ-057). Measured: EVENT 0x0005 at 0x08 reported after the restore (`test/onboard-computer/results/2026-10-16/req-057-restore-keeps-backup.log`). | FIXED: decision 46, `restore.sh` ae40af0; BKP10R and BKP11R read 0 after the restore, no EVENT 0x0005 (`test/onboard-computer/results/2026-10-16/f3-restore-clears-backup.log`) |

Bench re-run of decision 30, 16 Oct 2026, controller and console d3bb74a (`test/ground/results/2026-10-16/decision-30/decision-30-hardware.log`):

| Step | Measured |
|---|---|
| A | full restore with the d3bb74a images: RESTORE DONE: full |
| B | RETURN TO PREVIOUS in a trial: EVENT 0x0008, then HEALTH FALLBACK, slot A |
| C | SWITCH to the given-up image: REFUSED 0x13 |
| D | LEAVE FALLBACK: DONE, then OPERATE; again in OPERATE: REFUSED 0x05; SWITCH: REFUSED 0x13 |
| E | START FAILED copy B: DONE, trial of slot B (EVENT 0x0006, EVENT 0x000a); RETURN TO PREVIOUS: FALLBACK, slot A |
| F | trial-forever image (3002), 3 power cuts: failed starts 1, 2, 3, then FALLBACK, slot A at 06:03:58.93 UTC; SWITCH: REFUSED 0x13 |
| G | `update --yes` from FALLBACK (image 3003): confirmed in 7.5 s; OPERATE, slot B |

A restart and a power cut with the given-up mark set (`test/ground/results/2026-10-16/decision-30/a5.4-given-up-across-restart.log`) each left FALLBACK, and SWITCH was refused 0x13 each time. After the third power cut of step F no EVENT of the bootloader's give-up reached the journal (observation O-7).

## Not run

Counts are rows with Status NOT RUN, by the Reason column of `test/fault-matrix.md`.

| Reason | Rows | What unblocks it |
|---|---|---|
| PLANNED-2 | 52 | Scripted runs (console, ssh, systemctl, sysfs) not yet done |
| PLANNED-3 | 1 | The owner's hands on a cable or button |
| PLANNED-4 | 19 | Fault injection over SWD; the injection design is shown to the owner first |
| EARLIER BUILD | 15 | A re-run on 4d6f5e8 or later; each row names the earlier commit |
| NOT BUILT-ACCEPTED | 3 | Nothing: the owner accepted that the function is not built |
| NOT BUILT-OPEN | 15 | A decision or the function; the rows cite `src/` of the source before d3bb74a and are not re-checked against it |
| NEEDS TOOL | 7 | The link tool of Verification Plan Sec. 3.0; a kill of a process id read in advance; a test firmware with a broken update receiver; a reset that stops a flash write (B4.2, B7.2, SB5.2) |
| NEEDS DECISION | 0 | A8.4 was answered on 16 Oct 2026: the owner accepted the 7.58 h run (decision 47) |
| DESK | 13 | Inspections and analyses, no bench run |

NOT ON BENCH (7 rows):

| Missing on the bench | Rows |
|---|---|
| Data link B | A8.3, A19.1 |
| Factory-loader restore | A25.3, A29.5 |
| Controller power separate from the onboard computer's USB | A8.1, B5.4, B8.4 |

On this bench the Nucleo takes its power from the MP257's USB. An onboard computer restart restarts
the controller (clock 67 s to 17 s, `test/ground/results/2026-10-16/onboard-restart/onboard-restart.log`
lines 5 and 16), so REQ-024 cannot be shown here.

## Measured numbers

| Quantity | Value | Source |
|---|---|---|
| Full restore | 17 steps in 4.154 s | `test/onboard-computer/results/2026-10-15/restore-full-49314cc.log` |
| 14 restores back to back | each RESTORE DONE in 4.059 to 4.069 s | `test/onboard-computer/results/2026-10-16/restore-14-back-to-back.log` |
| Cable pulled during restores (B9.1) | run 2 RESTORE FAILED: connect, 0.93 s into the run, before any flash write; runs 3 to 8 failed while unplugged; runs 9 to 23 RESTORE DONE | `test/onboard-computer/results/2026-10-16/hand-b9.1-power-cut-during-restore.log` |
| Reset button pressed during restores (B9.2) | run 1 RESTORE FAILED: read back slot A erased; runs 2 to 18 RESTORE DONE | `test/onboard-computer/results/2026-10-16/hand-b9.2-reset-button-during-restore.log` |
| Brick, then restore (A25.1) | BRICK DONE: flash mass-erased; the restore cleared BKP10R and BKP11R and the controller ran in OPERATE, slot A | `test/onboard-computer/results/2026-10-16/a25.1-brick-then-restore.log` |
| Silent controller (data link A cut 75 s) | resets 1 and 2 of 3, 60.0 s apart; "controller heard again after 76 s" | `test/onboard-computer/results/2026-10-16/updater-d12db66-on-board.log`, Part 4 |
| Trial-forever image | trial-watch resets at 06:59:55, 07:02:25, 07:04:55 UTC (150 s apart), then FALLBACK, slot A at 07:04:57 | same, Part 5 |
| Endurance | 100 of 100 updates confirmed; fastest 7.5 s, median 7.9 s, slowest 12.3 s | `test/ground/results/2026-10-16/endurance/summary.txt` |
| Run without commands | 7.58 h (27,291 s) of the 12 h planned; 27,360 HEALTH reports; longest gap 1.08 s; 0 controller restarts; OPERATE, slot A, self-test PASSED throughout | `test/ground/results/2026-10-16/overnight/summary.txt` |
| Controller clock rate | +2509 ppm against the Mac (whole-second resolution, ±73 ppm) | same |
| Onboard computer clock rate | +17 ppm against the Mac | same |
| Radio loss 0.1, 3 seeds | 3 of 3 updates confirmed, 30 to 44 s | `test/ground/results/2026-10-15/radio-loss/summary.txt` |
| Radio loss 0.3, 3 seeds | 3 of 3 confirmed, 113 to 189 s | same |
| Radio loss 0.5, 3 seeds | 1 of 3 confirmed (324 s); 2 stopped after 6 unanswered tries of one chunk, as designed | same |
| Power cut during a delivery | 3 cuts, 1.0, 0.3 and 2.3 s into DELIVER; slot B and the last-resort firmware read back byte-identical over SWD | `test/ground/results/2026-10-16/power-cuts/pc1-deliver-1.log`, `-2.log`, `-3.log` |
| Three power cuts in one trial | failed starts 1, 2, 3, then OPERATE on slot B; slot B and the last-resort firmware byte-identical | `test/ground/results/2026-10-16/power-cuts/pc3-trial-three-cuts.log` |
| Updater killed | 5 runs, 4 measured; restart in 1.17 s in OPERATE; the kill during the test image missed its 136 ms window (NOT RUN) | `test/ground/results/2026-10-16/kills/k5-operate.log`, `k4-test-image.log` |
| Trial not confirmed | given up 119.742 s after it started (set length 120 s) | `test/onboard-computer/results/2026-10-15/README.md` |
| Data link A cut for 20 s | controller clock 28134 s to 28166 s, no restart; "controller silent" after 5 s; "controller heard again after 21 s of silence" | `test/ground/results/2026-10-16/link-cut/link-cut.log` |
| Reset over SWD during a link cut | RESTORE DONE: reset; controller clock 28169 s to 16 s | same |
| Onboard computer restart | back in 33.1 s | `test/ground/results/2026-10-16/onboard-restart/onboard-restart.log` |
| Onboard computer restart during an upload | updater active again 31.4 s after the order; the 296 bytes kept; the upload resumed at byte 256 and its UPLOAD END checks passed | `test/ground/results/2026-10-16/onboard-restart/onboard-restart-during-upload.log` |
| Stored reports (decision 29) | SEND REPORTS from 1: 53 reports, 2 events first; after a restart of the onboard computer 2 EVENT and 68 HEALTH reports stored, 86 sent, events first; the 1 MB overflow is host-tested only | `test/onboard-computer/results/2026-10-16/updater-bc34e8d-on-board.log`, Parts 3 and 6 |
| SWITCH during a trial | REFUSED 0x05 | `test/ground/results/2026-10-16/decision-30/switch-in-trial.log` |
| Command checks | refusals 0x01, 0x02, 0x04, 0x05, 0x06, 0x0A, 0x0B, 0x12 each measured; a repeated command answered ALREADY DONE | `test/ground/results/2026-10-16/command-checks/command-checks.log` |

## Stress audit, 16 October 2026

The owner ordered a stress audit on 16 Oct 2026. Its conditions are Part C of
`test/fault-matrix.md`: rows SA1 to SA8 ran as harnesses on the Mac, rows SB1 to SB8 on the bench.
The audit also changed rows of Parts A and B. A8.4, B2.5, B2.10, B2.11, B3.1, B4.1 and B7.1 are
PASS. B2.6 is FAIL. B4.2 and B7.2 are NOT RUN with the reason NEEDS TOOL.

Build under test (`test/ground/results/2026-10-16/stress/session.log`, lines 10 to 15):

| Unit | Commit |
|---|---|
| Controller bootloader, firmware A and B, last-resort firmware, installed by `restore.sh full` at 11:51 EEST | a504b0d (`src/controller` identical to d3bb74a; only the build id string and the signatures differ) |
| Onboard computer updater on the board, sha256 `5f0c5f49…` | bc34e8d |
| `restore.sh`, sha256 `b8b1e5a8…` | ae40af0 |
| Console | a504b0d (`src/ground` identical to 615a83f) |
| Host harnesses | a504b0d |

Part C counts 19 rows: PASS 9, FAIL 7, NOT RUN 2 (SB2.1 PLANNED-2, SB5.2 NEEDS TOOL), N/A 1 (SB6).

| Quantity | Value | Source |
|---|---|---|
| 1,000 updates (SB1) | 997 of 1,000 confirmed; console time fastest 7.4 s, median 8.2 s, slowest 14.5 s; 3 deliveries refused 0xff (cycles 171, 329, 704), each followed by a passing cycle | `test/ground/results/2026-10-16/stress/sb1-endurance/summary.txt`, `endurance-cycles.txt` |
| Updater during the 1,000 updates | 8 "controller silent" lines and 2 resets of the controller in the board journal; 0 restarts of the updater | same directory, `board-at-end.txt` |
| Radio loss 0.5 to 0.7 with resume (SB2.1, stopped, NOT RUN) | loss 0.5: seed 1 not confirmed within the 600 s run bound (6 attempts, upload reached 13312 bytes), seed 2 confirmed after 3 attempts in 806 s, seed 3 after 2 attempts in 315 s; loss 0.6 seed 1 not confirmed within 600 s (9 attempts, 16128 bytes); the "resumed at" field is empty in every line and does not show whether an upload resumed, because the driver looked for "resuming at byte" in the console's output, which the updater writes only to its journal (the coordinator's account); stopped by the coordinator at 16:07:16 EEST on the loss of the USB network link between the Mac and the onboard computer; loss 0.6 seeds 2 and 3 and loss 0.7 not run | `test/ground/results/2026-10-16/stress/sb2-loss/sb2-loss-summary.txt` |
| Floods during a delivery (SB2.2) | 4 of 4 deliveries completed; slot A, slot B and the last-resort firmware read back IDENTICAL in each run; switch and confirm exit 0 in each | `test/ground/results/2026-10-16/stress/sb2-floods/sb2-flood-summary.txt` |
| Data link A flapping (SB3) | 0 of 1,144 updates confirmed in 600 s of 3 s unbound, 3 s bound; afterwards 77 failed sends in 5.7 min, no silence noticed; 2 of 2 updates confirmed (7.7 s, 8.7 s) after a hub power cycle | `test/ground/results/2026-10-16/stress/sb3-flap/sb3-flap.log`, `stuck-link-evidence.txt` |
| Onboard computer restarted during uploads (SB4) | 10 of 10 uploads completed after the restart; updater active again 28.4 to 40.3 s after the order (runs 2 to 10); 7 resumed at byte 256 to 2304, 2 started at byte 0; 3026 EVENT reports stored throughout, 0 dropped | `test/ground/results/2026-10-16/stress/sb4-reboot/sb4-reboot-summary.txt`, `sb4-reboot.log` |
| Power cut at the 9 flash operations of an update (SB5.1) | 9 of 9 cut points: OPERATE slot A after hits 1 to 3; TRIAL slot B, failed starts 2 after hits 4 to 6; TRIAL slot B, failed starts 3 after hits 7 to 9; a complete boot-state copy after each cut | `test/onboard-computer/results/2026-10-16/stress/sb5-flash-cuts/cuts-summary.txt`, `cuts.log` |
| Restores overlapping the updater's resets (SB7) | 10 of 10 RESTORE DONE: full, 4.06 to 4.21 s each; 5 resets by the updater, each RESTORE DONE: reset, each on a controller heard again 33 to 43 ms after the reset started | `test/onboard-computer/results/2026-10-16/stress/sb7-overlap.log` |
| Data directory full (SB8) | 22 uploads stored before the first refusal; then REFUSED 0xfe (storage) at UPLOAD END, at a chunk and at UPLOAD BEGIN; 0 restarts of the updater; 45 reports not stored in 44 s | `test/onboard-computer/results/2026-10-16/stress/sb8-disk.log` |
| fsync on the idle board | 1,200 writes 0.1 s apart: longest 301 ms, 0 over 500 ms | `test/onboard-computer/results/2026-10-16/stress/fsync-probe.log` |
| Updater fuzz (SA1) | 7 targets: frame, parser, imagefile, records and reports 1,000,000 inputs each, image 1,676,512, updater 1,119,449 frames in 500 sessions; 0 findings; 8 of 8 deliberate defects caught; mutation-based, without coverage feedback | `test/onboard-computer/results/2026-10-16/stress-host/fuzz/` |
| Controller fuzz (SA2) | 5 targets: frame, image, boot state and backup registers 1,000,000 inputs each, command 1,000,386; 0 failures; 20 of 20 deliberate defects caught; mutation-based, without coverage feedback | `test/controller/results/2026-10-16/stress-host/fuzz/` |
| Report store cut at every byte (SA3) | 340 cuts, 0 failed | `test/onboard-computer/results/2026-10-16/stress-host/store/a2.log` |
| Durable records cut at every step (SA4) | 336 cases, 0 failed | `test/onboard-computer/results/2026-10-16/stress-host/store/a3.log` |
| Boot state cut at every flash step (SA5) | 162,901 cases: old 93,107, new 68,660, blank to damaged 1,134, third 0, lost 0; first save from blank flash: 1,114 of 2,000 random cuts damaged | `test/controller/results/2026-10-16/stress-host/powercut/powercut_bootstate.log` |
| Wrap, boundary and repeat cases (SA6, SA8) | updater: 244 OK, 28 FINDING, 5 NOTE and 1 DIED lines; the data-link sequence through its full 2^32 wrap in 1,310 s; controller: 24 and 3 checks, 0 failed | `test/onboard-computer/results/2026-10-16/stress-host/boundaries/boundaries.log`, `wrap-full.log`; `test/controller/results/2026-10-16/stress-host/powercut/boundaries_mode.log`, `clock_wrap.log` |
| Floods against the host updater (SA7) | 31 scenarios at about 10,000 frames a second, the updater alive after each; valid flood: 40 to 45 % answered DONE, median latency 0.90 to 1.02 s, CPU mean 80 to 92 %, 3 of 9 uploads failed; from another address: no DONE reached the ground for 9 deliveries; 0.9 to 2.1 MB of log a second | `test/onboard-computer/results/2026-10-16/stress-host/flood/summary.txt` and its scenario logs |

Findings of the stress audit. Each is described in full under "Stress findings" in
`test/fault-matrix.md`. All are OPEN.

| No. | Severity (TBR) | Summary | Rows | Log |
|---|---|---|---|---|
| ST-001 | 1 | After 63 other ARMs, a resent ARM pushes its own SWITCH out of the 64-entry repeat memory, and the SWITCH is passed on a second time. | SA8 | `test/onboard-computer/results/2026-10-16/stress-host/boundaries/boundaries.log` |
| ST-002 | 1 | The 64 entries protect only the last 32 ARM and command pairs, so an old dangerous command runs again after 32 pairs or an updater restart. | SA8 | same |
| ST-003 | 1 | An old SET ONBOARD TIME, exempt from expiry, runs again once it has left the repeat memory. | SA8 | same |
| ST-004 | 2 | A power cut during the first boot-state save after a full restore leaves both copies damaged, and only the last-resort firmware starts. | SA5 | `test/controller/results/2026-10-16/stress-host/powercut/powercut_bootstate.log` |
| ST-005 | 2 | Answers go to the most recent sender, so any second sender of valid commands takes the downlink. | SA7, SB2.2 | `test/onboard-computer/results/2026-10-16/stress-host/flood/summary.txt`; `test/ground/results/2026-10-16/stress/sb2-floods/sb2-flood.log` |
| ST-006 | 2 | Under 10,000 valid commands a second the updater answers 40 to 45 % of them about 1 s late, and 3 of 9 uploads fail. | SA7 | `test/onboard-computer/results/2026-10-16/stress-host/flood/summary.txt` |
| ST-007 | 3 | Each accepted command writes a log line with no rate limit: 0.9 to 2.1 MB of log a second under a flood. | SA7 | the scenario logs in `test/onboard-computer/results/2026-10-16/stress-host/flood/` |
| ST-008 | 3 | Each repeat is answered ALREADY DONE, so a repeat flood is mirrored on the downlink. | SA7 | `test/onboard-computer/results/2026-10-16/stress-host/flood/upload-repeat.log` |
| ST-009 | 3 | Single uploads stalled for 43.4 s and 34.6 s under repeat and mixed floods, then completed; the cause is not traced. | SA7 | `test/onboard-computer/results/2026-10-16/stress-host/flood/summary.txt` |
| ST-010 | 2 | After a pause in reading the controller, the updater resets a controller that is heard again at once and refuses deliveries 0xff; the cause is open. | SB1, SB5.1, SB7 | `test/ground/results/2026-10-16/stress/sb1-endurance/`; `test/onboard-computer/results/2026-10-16/stress/sb7-overlap.log` |
| ST-011 | 3 | The report store is not wrap-aware: numbers are reused after 0xFFFFFFFF, and unsent reports after the wrap can be deleted. | SA6 | `test/onboard-computer/results/2026-10-16/stress-host/store/a4.log` |
| ST-012 | 4 | A boot-state write at sequence 0xFFFFFFFF answers success and has no effect. | SA6 | `test/controller/results/2026-10-16/stress-host/powercut/powercut_bootstate.log` |
| ST-013 | 4 | The updater has a signed overflow for a stored clock offset that no command can store. | SA6 | `test/onboard-computer/results/2026-10-16/stress-host/boundaries/boundaries.log` |
| ST-014 | 4 | The firmware's image check underflows for a region under 192 bytes, which no caller passes. | SA2 | `test/controller/results/2026-10-16/stress-host/fuzz/fz_image.log` |
| ST-015 | 4 | Code rule C8 (lines of 100 characters or fewer) is not checked, and committed lines break it. | — | the coordinator's count; no log |
| ST-016 | 2 | After rapid unbind and bind of data link A the updater stayed blind for 5.7 min: no silence noticed, no reset, no reopen. | B2.6, SB3 | `test/ground/results/2026-10-16/stress/sb3-flap/stuck-link-evidence.txt` |
| ST-017 | 3 | On a full disk every report is lost from the store, with one log line per report; whether they count as dropped is not shown. | SB8.2 | `test/onboard-computer/results/2026-10-16/stress/sb8-disk.log` |

Severity (TBR): 1, a safety function can be defeated; 2, a function is lost (crash, hang,
unbounded growth, lost records, an update that can never complete, a timer late by more than its
period); 3, degraded; 4, hygiene. Proposal (TBR), awaiting the owner's acceptance.

Questions Q-1 to Q-13, on behaviour that no source specifies, are listed after "Decisions" in
`test/fault-matrix.md`. Observations O-12 to O-15 cover the stress audit; O-14 lists its
test-method notes.

Note: the result logs under `test/controller/results/2026-10-16/` and
`test/onboard-computer/results/2026-10-16/stress-host/` were changed by the coordinator on
16 Oct 2026 at about 15:18 EEST in one way only. The absolute path of the local checkout was
replaced by `<checkout of a504b0d>` (745 places in 120 files). In 21 lines where a fixed line
width had cut that path short, it reads `<workspace>/failsafe-remot`. Every other byte of each log
is as recorded.

## Where the evidence is

| What | Path |
|---|---|
| Rows, findings, stress findings ST-001 to ST-017, observations O-1 to O-15, decisions, questions Q-1 to Q-13 | `test/fault-matrix.md` |
| Stress audit runs, 16 Oct | `test/ground/results/2026-10-16/stress/`, `test/onboard-computer/results/2026-10-16/stress/`, `test/onboard-computer/results/2026-10-16/stress-host/`, `test/controller/results/2026-10-16/stress-host/` |
| Ground-side runs, 15 Oct | `test/ground/results/2026-10-15/README.md` |
| Ground-side runs, 16 Oct | `test/ground/results/2026-10-16/README.md` |
| Onboard computer runs | `test/onboard-computer/results/2026-10-15/README.md`, `test/onboard-computer/results/2026-10-16/README.md` |
| Controller runs of 12 and 13 Oct (earlier builds) | `test/controller/results/` |

## Open items for the owner

| Item | What is needed |
|---|---|
| A8.4 | Accept the 7.58 h run without commands, or order the 12 h run (decision 4); answered 16 Oct 2026: the 7.58 h run accepted (decision 47) |
| Severity scale (TBR) | Accept or change the severity scale of the stress findings, a proposal awaiting the owner's acceptance (`test/fault-matrix.md`, "Stress findings") |
| Questions Q-1 to Q-13 | State the expected behaviour for each open question of the stress audit (`test/fault-matrix.md`, after "Decisions") |
| O-10 | Decide on a lock shared by every SWD user and on the updater pausing its silence resets during an ordered restore (proposal marked TBR) |
| O-11 | Decide whether to fix the -6 s case of `testClockDifferenceEvent` |
| TBR proposals | Accept or change each proposal still marked TBR |
| Decision 20 | The method of the restore after 3 silent resets |
| Decision 28 | The real-time clock design |
