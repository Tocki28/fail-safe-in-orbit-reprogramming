# Bench results — 15 October 2026, the whole chain after the restore

Bench: Mac (`src/ground/console.py`) → USB-C network → STM32MP257F-DK (`fsir-updater` as a
systemd service) → the NUCLEO-G431KB's ST-LINK serial port → controller. Controller (bootloader,
firmware A and B), console and images at commit 49314cc, installed by a full restore at 20:10 UTC
(`onboard-computer/results/2026-10-15/restore-full-49314cc.log`); updater sha256 `e5f46cc6…`,
built at d95a67a. Board times are UTC; Mac times EEST. Rows of the verification campaign are in
`test/fault-matrix.md`; findings F-1 and F-2 are described there.

| Run | Status | Measured | Log |
|---|---|---|---|
| RETURN TO PREVIOUS in OPERATE | PASS | REFUSED 0x05 "Not allowed in the current mode" | `return-in-operate.log` |
| RETURN TO PREVIOUS in a trial | PASS | passed on at 10552.650 s; EVENT 0x0008 at 10552.679 s; DONE at 10552.681 s; then OPERATE on slot A | `return-in-trial.log` |
| SWITCH to a given-up firmware, no new delivery | FAIL (F-1) | DONE; the given-up firmware started a new trial (HEALTH TRIAL, slot B, failed starts 1) | `finding-37-switch-to-given-up.log` |
| Trial past its time (trial-forever test firmware) | FAIL (F-2) | one reset 150.04 s after the trial started (EVENT 0x0106 reason 0x01); no second reset; still in TRIAL 723 s later | `trial-forever-decision-h.log` |
| Three failed starts give the trial up | PASS | after two operator resets (`restore.sh reset`), EVENT 0x0002 refused copy B, check 0x0c; OPERATE on slot A | `trial-forever-decision-h.log` |
| Radio loss 0.1, seeds 1 to 3 | PASS | 3 of 3 updates confirmed; 30 to 44 s; 7 to 12 of about 100 datagrams dropped | `radio-loss/summary.txt` |
| Radio loss 0.3, seeds 1 to 3 | PASS | 3 of 3 updates confirmed; 113 to 189 s; 27 to 35 of about 120 dropped | `radio-loss/summary.txt` |
| Radio loss 0.5, seeds 1 to 3 | PASS | 1 of 3 updates confirmed (324 s); 2 stopped after 6 unanswered tries of one chunk, as designed, with the offset to resume | `radio-loss/summary.txt` |
| Upload resumed after loss | PASS | stopped at offset 2560 by 0.5 loss; `upload --offset 2560` completed in 0.93 s; UPLOAD END checks passed | `radio-loss/resume-after-loss.log` |

In every completed radio-loss run the controller reported one trial start and one confirmation:
no resent command ran twice.

Note: in the radio-loss logs the path of the local checkout was written as
`<checkout of 49314cc>/`. Only that path text was changed; every other byte of each log is as
recorded.
