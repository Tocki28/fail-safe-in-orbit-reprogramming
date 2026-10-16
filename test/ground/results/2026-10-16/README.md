# Bench results — 16 October 2026 (night and morning), the whole chain on the final build

Bench: as on 15 Oct. Since 21:00 UTC on 15 Oct everything runs at commit 4d6f5e8: the separated
bootloader (1ba9c79), firmware A and B, console and images, installed by a full restore
(`onboard-computer/results/2026-10-16/req-057-restore-keeps-backup.log`), and the updater built
at 4d6f5e8 (sha256 `6b0888f8…`). The controller's power is cut by the MP257's USB hub (`uhubctl -l
1-1 -p 1`). Board times UTC, Mac times EEST. Rows and findings: `test/fault-matrix.md`. The
morning runs (08:32 to 08:39 EEST) ran on the same build: console, command sender and images at
4d6f5e8, updater sha256 `6b0888f8…`. The decision 30 runs (09:01 to 09:04 EEST) ran on controller
and console images of d3bb74a with the same updater. In the table, "—" in Status marks a run that is a measurement
only, and NOT ON BENCH marks a run the bench cannot show.

| Run | Status | Measured | Log |
|---|---|---|---|
| Two updates through the separated bootloader | PASS | A to B and B to A confirmed; each delivered image deleted on EVENT 0x0007 (decision 22) | `update-new-bootloader-1.log`, `-2.log` |
| SWITCH and CONFIRM resent before their answer | PASS | SWITCH sent 3 times 50 ms apart, passed on once, one EVENT 0x0006; CONFIRM sent 3 times 20 ms apart, passed on once, one EVENT 0x0007 | `already-done-short-timeout.log` |
| Updater killed in OPERATE | PASS | SIGKILL to PID 17465; restarted in 1.17 s; controller clock 45 s to 71 s in 26 s (no restart) | `kills/k5-operate.log` |
| Updater killed during an upload | PASS | killed at offset 16896; 17152 bytes kept; `--offset` refused 0x06 after the restart; plain `upload` resumed at byte 17152, END checks passed | `kills/k1-upload.log` |
| Updater killed during a delivery | PASS | killed 1.4 s in; the console's resend after 30 s ran the delivery again; the controller accepted UPLOAD END; 33.1 s in all | `kills/k2-deliver.log` |
| Updater killed during a trial | PASS | CONFIRM through the restarted updater: EVENT 0x0007, OPERATE on slot A | `kills/k3-trial.log` |
| Updater killed during the test image | NOT RUN | the kill landed 0.45 s after the log match, after the 136 ms transfer had ended | `kills/k4-test-image.log` |
| Power cut during a delivery, 3 runs | PASS | cut 1.0, 0.3, 2.3 s into DELIVER, 3 s off: slot B ran again; slot B and the last-resort read back over SWD byte-identical; the repeated delivery completed | `power-cuts/pc1-deliver-1.log`, `-2.log`, `-3.log` |
| Power cut during a trial, one cut | PASS | failed starts 2 after the cut; the trial ran out at 120 s and was given up (EVENT 0x0008) | `power-cuts/pc2-trial-*.log` |
| Three power cuts in one trial | PASS | failed starts 1, 2, 3, then OPERATE on slot B; slot B and the last-resort byte-identical | `power-cuts/pc3-trial-three-cuts.log` |
| Power cut aimed at the trial-start write | NOT RUN | the cut landed 0.48 s after SWITCH was passed on, after the write (trigger latency) | `power-cuts/pc4-switch-count-write.log` |
| Endurance, 100 updates | PASS | 100 of 100 confirmed; update time fastest 7.5 s, median 7.9 s, slowest 12.3 s; slots alternated every time | `endurance/summary.txt`, `endurance/endurance-log.txt` |
| Overnight run without commands | — | ended at the owner's word after 7.58 h (27,291 s) of the 12 h planned; 27,360 HEALTH reports, 0 event reports; longest gap 1.08 s, 0 gaps over 1.5 s; 0 controller restarts; OPERATE, slot A, failed starts 0, self-test PASSED in every report; controller clock +2509 ppm, onboard clock +17 ppm against the Mac (whole-second resolution, ±73 ppm for the controller) | `overnight/summary.txt`, `overnight/reports.csv`, `overnight/board-at-start.txt` |
| Commands to unknown or impossible targets | PASS | unknown onboard command 0x7F, unknown controller command 0x2F and GET STATUS with one byte too many: REFUSED 0x06 each; destination 0x07 and version 0x02: no answer, journal "dropped" and "not a good frame; dropped" | `command-checks/command-checks.log` lines 5-42, 720-728 |
| GET STATUS with Expiry 1000 s | NOT RUN | answered RECEIVED and DONE, not REFUSED 0x03; the expiry check is not built | `command-checks/command-checks.log` lines 43-51 |
| Frame bytes flipped on the ground link | NOT RUN | bytes 0, 2, 4, 16 and 20 of the 21-byte frame flipped: no answer each, journal "not a good frame; dropped"; the undamaged control answered DONE; the other 16 byte positions are not measured | `command-checks/command-checks.log` lines 53-86, 733-737 |
| Dangerous commands without an ARM | PASS | SWITCH, CONFIRM, START FAILED and SET LIMITS without an ARM: REFUSED 0x01 each; an ARM naming another sequence number, and an ARM naming CONFIRM, then SWITCH: REFUSED 0x01 | `command-checks/command-checks.log` lines 88-147, 744-763 |
| CONFIRM in OPERATE | PASS | ARM then CONFIRM: REFUSED 0x05, twice | `command-checks/command-checks.log` lines 148-159, 248-259 |
| ARM older than 30 s | PASS | SWITCH and CONFIRM 32 s after their ARM: REFUSED 0x02 each | `command-checks/command-checks.log` lines 160-247, 775-780 |
| Used sequence numbers | PASS | a SWITCH and an ARM naming SWITCH sent with a number used before and other bytes: REFUSED 0x04 each | `command-checks/command-checks.log` lines 260-277, 790-793 |
| GET STATUS sent 3 times with one number | PASS | DONE, then ALREADY DONE twice; journal "answered ALREADY DONE, nothing else done" | `command-checks/command-checks.log` lines 278-291, 799-801 |
| SET LIMITS and the refusals it causes | PASS | after an ARM, minimum version 2: the version 1 image refused 0x0A at UPLOAD END; identity 0x0999: the image refused 0x0B at UPLOAD END; limits set back to 1 and 0x0431 | `command-checks/command-checks.log` lines 292-501 |
| Upload chunk past the stored bytes | PASS | UPLOAD CHUNK at offset 1024 with nothing stored: REFUSED 0x06 | `command-checks/command-checks.log` lines 518-537 |
| CANCEL of an open upload | PASS | DONE; stored bytes removed; the image file gone from the board | `command-checks/command-checks.log` lines 538-562, 995 |
| CANCEL 1.0 s into a delivery | PASS | DELIVER REFUSED 0x12 after 3840 of 20424 bytes; image deleted; OPERATE, slot A afterwards; the console printed "unknown code 0x12" (observation O-6) | `command-checks/command-checks.log` lines 666-711, 1086-1091 |
| Data link A cut for 20 s in OPERATE | PASS | cdc_acm unbound 20 s; controller clock 28134 s to 28166 s across 32 s of onboard time, no restart; journal "controller silent" after 5 s and "controller heard again after 21 s of silence" | `link-cut/link-cut.log` lines 3-15, 45, 55 |
| `restore.sh reset` during a data link A cut | PASS | RESTORE DONE: reset; controller clock 28169 s to 16 s; OPERATE, slot A after the link returned (9 s of silence in the journal) | `link-cut/link-cut.log` lines 17-33, 68, 72 |
| Onboard computer restarted in OPERATE (`systemctl reboot`) | NOT ON BENCH | back in 33.1 s (`fsir-updater` active); the controller restarted too, clock 67 s to 17 s, because the Nucleo is powered from the MP257's USB; REQ-024 cannot be shown on this bench | `onboard-restart/onboard-restart.log` lines 3-17 |
| Onboard computer restarted during an upload | NOT RUN | the upload ended in 1.16 s, before the reboot; no upload was in progress; back in 31.0 s | `onboard-restart/onboard-restart.log` lines 19-33 |
| Full restore with the d3bb74a images | PASS | RESTORE DONE: full; three images checked first; OPERATE, slot A, failed starts 0 | `decision-30/decision-30-hardware.log` step A |
| RETURN TO PREVIOUS in a trial (d3bb74a) | PASS | EVENT 0x0008, then HEALTH FALLBACK, slot A | `decision-30/decision-30-hardware.log` step B, journal |
| SWITCH to the given-up image (F-1) | PASS | REFUSED 0x13 "The image was given up; START FAILED starts it again"; again after LEAVE FALLBACK: REFUSED 0x13 | `decision-30/decision-30-hardware.log` steps C and D, journal |
| LEAVE FALLBACK | PASS | DONE, then HEALTH OPERATE; sent again in OPERATE: REFUSED 0x05 | `decision-30/decision-30-hardware.log` step D |
| START FAILED copy B | PASS | DONE; EVENT 0x0006 at 06:02:45.66 UTC and EVENT 0x000a at 06:02:46.79 UTC; trial of slot B, failed starts 1; RETURN TO PREVIOUS: EVENT 0x0008, FALLBACK, slot A | `decision-30/decision-30-hardware.log` step E, journal |
| Bootloader give-up: trial-forever image 3002, 3 power cuts | PASS | SWITCH DONE (a different image from the given-up one); cuts at 06:03:30, :41 and :53 with 3 to 4 s off; HEALTH TRIAL slot B failed starts 1, 2, 3, then FALLBACK slot A at 06:03:58.93 UTC; SWITCH: REFUSED 0x13 at 06:04:14.61 UTC; no EVENT of the give-up reached the journal (observation O-7) | `decision-30/decision-30-hardware.log` step F, journal |
| Update from FALLBACK with `update --yes` | PASS | image 3003: trial started, tests passed, confirmed in 7.5 s; OPERATE, slot B at the end | `decision-30/decision-30-hardware.log` step G, journal |
| Given-up image across a restart and a power cut (A5.4) | PASS | after RETURN TO PREVIOUS: FALLBACK, slot B; `restore.sh reset` (RESTORE DONE: reset): still FALLBACK, SWITCH REFUSED 0x13; hub power cut 06:08:27 to 06:08:30 UTC: still FALLBACK, SWITCH REFUSED 0x13; LEAVE FALLBACK, then OPERATE | `decision-30/a5.4-given-up-across-restart.log` |
| Onboard computer restarted during an upload (d3bb74a) | PASS | upload slowed by `--radio-loss 0.3 --seed 1`; `systemctl reboot` ordered at 06:05:23 UTC with `image-03004.part` at 296 bytes; the console stopped after 6 tries at offset 256; the updater was active again 31.4 s after the order; a plain `upload` resumed at byte 256 ("UPLOAD CHUNK image 3004 at 0: already stored"); UPLOAD END checks passed | `onboard-restart/onboard-restart-during-upload.log` |

Observations O-1 to O-7 (not failures) are listed under the findings of `test/fault-matrix.md`.

Note: the endurance run's own summary used a wrong pattern for the confirm line and logged every
cycle in full; `endurance/summary.txt` is recomputed from the per-cycle records, which are as
written. In the logs the path of the local checkout was written as `<checkout of 4d6f5e8>/`, and in
the d3bb74a logs as `<checkout of d3bb74a>/`. In `decision-30/decision-30-hardware.log` one local
path was written as `<scratch>/`. Only that path text was changed; every other byte of each log is
as recorded.
