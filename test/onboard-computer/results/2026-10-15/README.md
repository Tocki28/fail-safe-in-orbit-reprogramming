# Bench results — 15 October 2026, onboard computer (STM32MP257F-DK)

Board state at the start: `fsir-updater` sha256 `e49e7d32…`, commit not known; clock at
2000-01-01; journal kept in memory only. Rows of the verification campaign are in
`test/fault-matrix.md`.

| Run | Status | Measured | Log |
|---|---|---|---|
| The owner's runs: `update` answered y, then n | PASS | y: slot A confirmed (EVENT 0x0007); n: trial of slot B given up 119.742 s after it started (EVENT 0x0006 at 334.617 s, EVENT 0x0008 at 454.359 s); slot A in OPERATE | `tom-update-y-n-mp257.log` (saved from the volatile journal) |
| State at the start of the campaign | — | OPERATE, slot A, failed starts 0, self-test PASSED | `phase0-status-console.log` |
| Updater installed at d95a67a | — | built twice, byte-identical (sha256 `e5f46cc6…`); installed 17:16 UTC; the earlier binary is kept as `/usr/local/bin/fsir-updater.e49e7d32` | `journal-before-install-mp257.log` |
| Hub switches the controller's power | PASS | `uhubctl -l 1-1 -p 1 -a cycle` (USB2422 hub, 5 s off): controller clock 15081 s, then 6 s (a power loss); OPERATE, slot A, failed starts 0 | `power-switch-check.log` |
| Full restore with the 49314cc images | PASS | 17 steps done in 4.154 s; all three images checked before writing; backup registers afterwards: only LAST_START set | `restore-full-49314cc.log` |

Board changes made on 15 Oct (owner decisions 1 to 3): the updater above installed; `/var/log`
moved from tmpfs to the SD card, `/etc/tmpfiles.d/00-create-volatile.conf` and
`/etc/systemd/journald.conf.d/50-fsir-persistent.conf` keep the journal across restarts; the
clock and the RTC set from the Mac at 17:16:28 UTC.
