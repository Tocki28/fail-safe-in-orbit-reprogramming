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
Runs the checks of an upload (both Ed25519 signatures, damage, target, kind, length) on one file
and prints one line. Exit 0: passed; 1: refused (the line says why); 2: usage; 3: keys unusable.

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
