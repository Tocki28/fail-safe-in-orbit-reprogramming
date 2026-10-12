# src — operating software

One folder per component of the architecture. Bench-only test tools are in `test/`,
development tools in `tools/`. Each component's requirements are in the Architecture
(Section 6.0); how each requirement is verified is in the Verification Plan (Appendix A).

## Status

| Step | Folder | What it does | Status |
|---|---|---|---|
| 1 | `controller/bootloader` | Picks, checks and starts the firmware | Done, bench-tested |
| 2a | `controller/firmware/data-link` | UART and frames | Done, bench-tested; C++ port running |
| 2c | `controller/firmware/update-receiver` | Receives a new image | Done, bench-tested; C++ port running |
| 2d | `controller/firmware/mode-manager` | Trial and confirmation | Waiting for the trial judge |
| 2e | `controller/firmware/health-monitor` | Reports, watchdog, self-test | Reports done, bench-tested; self-test later |
| 2b | `controller/firmware/command-checker` | Sequence, mode and arming checks | Later |
| — | `controller/common` | Code shared by bootloader and firmware | Done |
| 3 | `onboard-computer/updater` | Checks images and delivers them | Building |
| 3r | `onboard-computer/restore` | Rewrites a bricked controller | Building |
| 4 | `ground` | Operator console | Done, host-tested |

Languages: bootloader in C; firmware in C++ (port running); onboard computer in C++; ground in Python.

Bench limit: the bootloader and the firmware share `controller/common`, against REQ-119.

Evidence: `test/controller/results/`.

## Build and test

STM32CubeIDE's `arm-none-eabi-gcc` must be on `PATH` (see `controller/common/build.mk`).

| Command | Does |
|---|---|
| `make -C src/controller` | Build the controller programs and sign the firmware images |
| `make -C src/controller check` | Host checks: CRC-32, images, frames |
| `make -C src/controller flash-firmware` | Write bootloader and firmware (slot A, confirmed) to the board |
| `make -C test/controller scenario-N` | Write bench test N (1 to 7) to the board |
| `make -C test/controller link-probe` | Talk to the controller over data link A |
| `make -C test/ground check` | Ground console tests |
