# Verification campaign — final report, 15 and 16 October 2026

This report answers one question: which activities of the Verification Plan were measured on the
bench in the campaign of 15 and 16 October 2026, with what result, and what is still open. It is a
summary. Every row, every expected result and every log path is in `test/fault-matrix.md`.

The bench is a lab prototype: the STM32MP257F-DK as onboard computer and the NUCLEO-G431KB as
controller, joined by one USB serial link (data link A). It is not flight hardware.

## Build under test

| Unit | Commit | Since |
|---|---|---|
| Controller bootloader, firmware A and B, console, command sender, images | 49314cc | 15 Oct 20:10 UTC, full restore (`test/ground/results/2026-10-15/README.md`) |
| Same, with the separated bootloader | 4d6f5e8 | 15 Oct 21:00 UTC, full restore (`test/ground/results/2026-10-16/README.md`) |
| Onboard computer updater | d95a67a | 15 Oct 17:16 UTC (`test/onboard-computer/results/2026-10-15/README.md`) |
| Onboard computer updater, sha256 `6b0888f8…` | 4d6f5e8 | 15 Oct 21:00 UTC (`test/onboard-computer/results/2026-10-16/README.md`) |
| Controller and console with decision 30 (FALLBACK, START FAILED, LEAVE FALLBACK, SWITCH refuses a given-up image) | d3bb74a | 16 Oct, board start 06:01:56 UTC, full restore (`test/ground/results/2026-10-16/decision-30/decision-30-hardware.log`) |

Every run of 16 Oct up to 08:39 EEST ran on 4d6f5e8; the decision 30 runs (09:01 to 09:04 EEST) ran on d3bb74a. Rows whose evidence is from an earlier commit are NOT RUN
with the reason EARLIER BUILD.

## Counts

Source: `test/fault-matrix.md`, section "Counts" (250 rows: Part A 133, Part B 117).

| Status | Rows |
|---|---|
| PASS | 70 |
| FAIL | 2 |
| NOT RUN | 139 |
| NOT ON BENCH | 7 |
| N/A | 32 |

## Findings

The two FAIL rows come from two open findings (F-2, F-3). F-1 is fixed. Each finding is described
in full under "Findings" in `test/fault-matrix.md`.

| No. | Rows | Reproduce | State |
|---|---|---|---|
| F-1 | A5.4, B8.11 (both now PASS) | Deliver a firmware to slot B, `switch`, `return` (it is given up), then `switch` again with no delivery. Expected: refused. Measured: DONE and a new trial of the given-up firmware (`test/ground/results/2026-10-15/finding-37-switch-to-given-up.log`). | FIXED: decision 30, commit d3bb74a; SWITCH to a given-up image REFUSED 0x13 on the bench (`test/ground/results/2026-10-16/decision-30/decision-30-hardware.log`, steps C, D, F) |
| F-2 | A22.3 | Deliver `slotB-v2-trial-forever.img`, `switch`, wait 5 minutes. Expected: trial given up. Measured: one reset 150.04 s after the trial start, no second reset, still in TRIAL 723 s later (`test/ground/results/2026-10-15/trial-forever-decision-h.log`). | OPEN: fix approved 16 Oct (decision 45); being built |
| F-3 | A25.1 | Over SWD set PWR_CR1 DBP, write BKP10R 0x00000008 and BKP11R 0xfffffff7, clear DBP; run `restore.sh full`; listen for EVENT 0x0005. Expected: restore clears the kept data (REQ-057). Measured: EVENT 0x0005 at 0x08 reported after the restore (`test/onboard-computer/results/2026-10-16/req-057-restore-keeps-backup.log`). | OPEN: fix approved 16 Oct (decision 46); being built |

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
| PLANNED-2 | 57 | Scripted runs (console, ssh, systemctl, sysfs) not yet done |
| PLANNED-3 | 3 | The owner's hands on a cable or button |
| PLANNED-4 | 24 | Fault injection over SWD; the injection design is shown to the owner first |
| EARLIER BUILD | 15 | A re-run on 4d6f5e8 or later; each row names the earlier commit |
| NOT BUILT-ACCEPTED | 4 | Nothing: the owner accepted that the function is not built |
| NOT BUILT-OPEN | 18 | A decision or the function; the rows cite `src/` of the source before d3bb74a and are not re-checked against it |
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
| Command checks | refusals 0x01, 0x02, 0x04, 0x05, 0x06, 0x0A, 0x0B, 0x12 each measured; a repeated command answered ALREADY DONE | `test/ground/results/2026-10-16/command-checks/command-checks.log` |

## Where the evidence is

| What | Path |
|---|---|
| Rows, findings, observations O-1 to O-7, decisions | `test/fault-matrix.md` |
| Ground-side runs, 15 Oct | `test/ground/results/2026-10-15/README.md` |
| Ground-side runs, 16 Oct | `test/ground/results/2026-10-16/README.md` |
| Onboard computer runs | `test/onboard-computer/results/2026-10-15/README.md`, `test/onboard-computer/results/2026-10-16/README.md` |
| Controller runs of 12 and 13 Oct (earlier builds) | `test/controller/results/` |
