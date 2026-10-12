# src/ — operating software

One folder per component of the architecture (Architecture Sec. 6.0). `src/` holds operating
software only: it contains no fault-injection function (REQ-101). Bench-only test tools are in
`test/`; development tools are in `tools/`.

## Controller

| Folder | Component (Architecture Sec. 6.0) | Requirements | Verified by (Verification Plan) | Build step | Status |
|---|---|---|---|---|---|
| `controller/bootloader` | Bootloader | REQ-004 to 006, REQ-013 to 019, REQ-023, REQ-113, REQ-119, REQ-125, REQ-130, REQ-136 | Test-3 to Test-7 | 1 | BUILT; bench scenarios 1-7 PASS on 12 Oct (`test/controller/results/2026-10-12`) |
| `controller/firmware/data-link` | Data-link drivers | REQ-042, REQ-099, REQ-138 | Test-26, Inspection-1, Inspection-7 | 2a | NEXT |
| `controller/firmware/command-checker` | Command checker | REQ-027 to 036, REQ-100, REQ-133, REQ-134, REQ-136, REQ-137 | Test-9, Test-10 | 2b | LATER |
| `controller/firmware/update-receiver` | Update receiver | REQ-007, REQ-008 | Test-6 | 2c | LATER |
| `controller/firmware/mode-manager` | Mode manager | REQ-001 to 003, REQ-011 to 013, REQ-020, REQ-021, REQ-024 to 026, REQ-113, REQ-117, REQ-125, REQ-136 | Test-1, Test-4, Test-8 | 2d | LATER |
| `controller/firmware/health-monitor` | Health monitor | REQ-022, REQ-037 to 041, REQ-135 | Test-7, Test-11 | 2e | LATER |
| `controller/firmware/clock` | Controller clock | REQ-030, REQ-131, REQ-132 | Test-12 | after 2e | LATER |
| `controller/last-resort` | Last-resort firmware | REQ-004, REQ-009, REQ-010, REQ-125 | Test-6 | after 2e | LATER |
| `controller/common` | Shared by the bootloader and the firmware: layout, image format, boot state, flash, CRC-32. Bench limit: REQ-119 asks the bootloader to share no code | — | — | 1 | BUILT |

## Onboard computer and ground

| Folder | Software item | Build step | Status |
|---|---|---|---|
| `onboard-computer` | Onboard computer software (Architecture Sec. 6.0) | 3 | LATER |
| `ground` | Ground software (Architecture Sec. 6.0) | 4 | LATER |

## Outside `src/`

| Folder | What | Status |
|---|---|---|
| `test/controller/test-firmware` | Test tool "Test firmware": built to fail on purpose; bench only (REQ-101) | BUILT |
| `test/controller/Makefile` | Bench scenarios 1 to 7 and the host checks | BUILT |
| `tools/` | Development tools: dev keys, bench image signing, host checks | BUILT |
| `third_party/monocypher` | Ed25519 verification, Monocypher 4.0.2 | VENDORED |

## How to build and test

STM32CubeIDE's `arm-none-eabi-gcc` must be on `PATH` (see `src/controller/common/build.mk`).

    make -C src/controller               # build the operating programs
    make -C src/controller check         # host checks (same as: make -C test/controller check)
    make -C test/controller              # test firmware + signed test images
    make -C test/controller scenario-N   # write bench test N (1-7) to the board
    make -C src/controller flash         # bootloader only;  reset / monitor also there
    make -C src/controller clean && make -C test/controller clean
