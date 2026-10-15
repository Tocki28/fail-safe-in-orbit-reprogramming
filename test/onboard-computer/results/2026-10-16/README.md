# Bench results — 16 October 2026 (night), onboard computer (STM32MP257F-DK)

| Run | Status | Measured | Log |
|---|---|---|---|
| Updater built at 4d6f5e8 installed | — | sha256 `6b0888f8…`; installed 21:00:15 UTC; the earlier binary kept as `/usr/local/bin/fsir-updater.e5f46cc6` | `req-057-restore-keeps-backup.log` (journal part) |
| Full restore with the separated bootloader (1ba9c79) | PASS | 17 steps done; all images checked first; the controller started on the new bootloader: OPERATE, slot A, self-test PASSED | `req-057-restore-keeps-backup.log` |
| The restore clears the kept data (REQ-057) | FAIL (F-3) | a fault-location record planted over SWD in TAMP_BKP10R/11R before the restore was reported after it: EVENT 0x0005 at 0x08, found by the bootloader | `req-057-restore-keeps-backup.log` |

How the record was planted: OpenOCD over SWD without a reset: PWR_CR1 DBP set, `mww 0x40002528
0x00000008`, `mww 0x4000252C 0xFFFFFFF7`, DBP cleared; read back before the restore.
