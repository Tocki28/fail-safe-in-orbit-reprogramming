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

Source: `test/fault-matrix.md`, section "Counts" (250 rows: Part A 133, Part B 117).

| Status | Rows |
|---|---|
| PASS | 80 |
| FAIL | 0 |
| NOT RUN | 131 |
| NOT ON BENCH | 7 |
| N/A | 32 |

## Findings

No row is FAIL. The three findings F-1, F-2 and F-3 are fixed and measured on the bench. Each finding is described
in full under "Findings" in `test/fault-matrix.md`.

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
| PLANNED-2 | 55 | Scripted runs (console, ssh, systemctl, sysfs) not yet done |
| PLANNED-3 | 1 | The owner's hands on a cable or button |
| PLANNED-4 | 24 | Fault injection over SWD; the injection design is shown to the owner first |
| EARLIER BUILD | 15 | A re-run on 4d6f5e8 or later; each row names the earlier commit |
| NOT BUILT-ACCEPTED | 3 | Nothing: the owner accepted that the function is not built |
| NOT BUILT-OPEN | 15 | A decision or the function; the rows cite `src/` of the source before d3bb74a and are not re-checked against it |
| NEEDS TOOL | 4 | The link tool of Verification Plan Sec. 3.0; a kill of a process id read in advance; a test firmware with a broken update receiver |
| NEEDS DECISION | 1 | A8.4: the owner accepts the 7.58 h run or orders the 12 h run |
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

## Where the evidence is

| What | Path |
|---|---|
| Rows, findings, observations O-1 to O-11, decisions | `test/fault-matrix.md` |
| Ground-side runs, 15 Oct | `test/ground/results/2026-10-15/README.md` |
| Ground-side runs, 16 Oct | `test/ground/results/2026-10-16/README.md` |
| Onboard computer runs | `test/onboard-computer/results/2026-10-15/README.md`, `test/onboard-computer/results/2026-10-16/README.md` |
| Controller runs of 12 and 13 Oct (earlier builds) | `test/controller/results/` |

## Open items for the owner

| Item | What is needed |
|---|---|
| A8.4 | Accept the 7.58 h run without commands, or order the 12 h run (decision 4) |
| O-10 | Decide on a lock shared by every SWD user and on the updater pausing its silence resets during an ordered restore (proposal marked TBR) |
| O-11 | Decide whether to fix the -6 s case of `testClockDifferenceEvent` |
| TBR proposals | Accept or change each proposal still marked TBR |
| Decision 20 | The method of the restore after 3 silent resets |
| Decision 28 | The real-time clock design |
