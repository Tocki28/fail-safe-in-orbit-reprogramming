# fsir-updater (onboard computer)

The onboard computer's part of an update: takes firmware from the ground over UDP, checks and
stores it, delivers it to the controller over data link A (serial), passes commands, answers and
reports. Parts: `updater.hpp`. Bench build, STM32MP257F board (Linux, 64-bit ARM).

## Build
    make              # this Mac: build/host/fsir-updater (for the tests)
    make test         # unit tests and the bench test
    make board        # build/board/fsir-updater, static aarch64, in Docker

## Run by hand
    scp build/board/fsir-updater root@192.168.7.1:/usr/local/bin/
    ssh root@192.168.7.1 fsir-updater --device /dev/ttyACM0 --beat-s 2

## Check-only mode (used by the restore script)
    fsir-updater --check-image FILE --kind bootloader|main|last-resort
Runs the checks of an upload (both Ed25519 signatures, damage, target, minimum version, kind, length)
on one file and prints one line. The minimum version and controller identity are the stored ones (the
`limits` record in `--data-dir`) when it can be read, else minimum version 1 and identity 0x0431; the
line ends with which (owner decision 24, 15 Oct 2026; TBR). Exit 0: passed; 1: refused (the line says why); 2: usage; 3: keys unusable.

## Report store (owner decision 29, 15 Oct 2026)
Every report the updater sends to the ground is also kept in the file `reports` in `--data-dir`:
1 MB of reports, the oldest HEALTH reports dropped first when full, events never. SEND REPORTS
(0x0B, first report number) from the ground answers DONE, then sends EVENT 0x0104 (count and
onboard times of the dropped HEALTH reports) if any were dropped, then every stored event, then
every stored HEALTH report, 64 per loop pass. Reports numbered below the first number are deleted.
The file and the report numbering survive a power cut and a restart; a damaged tail is cut off at
start and logged. Layout and open choices (TBR): `report_store.hpp`, `config.hpp`.

## Restart of the controller when a trial overruns (decision H, accepted by the owner, 13 Oct 2026)
If no EVENT 0x0007 or 0x0008 arrives within the trial time plus margin (120 s + 30 s, bench values;
`config.hpp`), the updater runs `restore.sh reset` (`../restore/restore.sh`) as a child process: OpenOCD
`init; reset run; shutdown` over SWD. The main loop keeps serving both links meanwhile. When the script
reports that the controller was reset (`RESTORE DONE: reset`, or `RESTORE FAILED: after reset`), the updater
sends the ground EVENT 0x0106 with detail 0x01 "trial past its time" (Interfaces Sec. 5.4). In every other case
it sends nothing and the log says which step failed.

    --restore-script PATH   the restore script (default: restore.sh in the directory of the fsir-updater file)

`restore.sh` finds `openocd` and `tcl/` next to itself (or via `OPENOCD`, `OCD_SCRIPTS`), so deploy them
together, or give `--restore-script` the path of the restore folder's script. The updater sends the script TERM
after `kRestartScriptMs` (40 s) and KILL to its process group 5 s later; both numbers are TBD placeholders,
not measured (`config.hpp`).

## Install as a service (systemd, journald)
On the board, as root:

    install -m 755 build/board/fsir-updater /usr/local/bin/fsir-updater
    install -m 644 fsir-updater.service /etc/systemd/system/fsir-updater.service
    systemctl daemon-reload
    systemctl enable --now fsir-updater
    journalctl -u fsir-updater -f        # the log

The unit restarts the updater whenever it ends (`Restart=always`) and runs it with the options
the bench uses: `--device /dev/ttyACM0 --udp-port 47001 --data-dir /var/lib/fsir/images
--beat-s 2`. Edit `ExecStart` for another device or port. The updater has no `--host` option: it
listens on all interfaces. The unit file was written on the Mac and has not been run under
systemd yet (no board access from this session).
