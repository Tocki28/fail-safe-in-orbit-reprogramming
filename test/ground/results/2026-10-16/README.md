# Bench results — 16 October 2026 (night), the whole chain on the final build

Bench: as on 15 Oct. Since 21:00 UTC on 15 Oct everything runs at commit 4d6f5e8: the separated
bootloader (1ba9c79), firmware A and B, console and images, installed by a full restore
(`onboard-computer/results/2026-10-16/req-057-restore-keeps-backup.log`), and the updater built
at 4d6f5e8 (sha256 `6b0888f8…`). The controller's power is cut by the MP257's USB hub (`uhubctl -l
1-1 -p 1`). Board times UTC, Mac times EEST. Rows and findings: `test/fault-matrix.md`.

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

Observations O-1 to O-5 (not failures) are listed under the findings of `test/fault-matrix.md`.

Note: the endurance run's own summary used a wrong pattern for the confirm line and logged every
cycle in full; `endurance/summary.txt` is recomputed from the per-cycle records, which are as
written. In the logs the path of the local checkout was written as `<checkout of 4d6f5e8>/`. Only
that path text was changed; every other byte of each log is as recorded.
