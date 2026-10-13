# Makefile — bench-only test tools for the controller: test firmware, signed test images
# (good, hanging, flipped, wrong key) and the bench scenarios. Never part of the operating
# software (REQ-101). The operating programs are built by src/controller/Makefile.
#
#   make              build the bootloader and the test firmware, sign the test images
#   make check        host checks: CRC-32 against zlib; every image through the bootloader's
#                     own check code (tools/check_images.py); the unit tests
#   make unit         unit tests, built with the Mac's clang++ (C++17, as the firmware): frame
#                     layer (unit/test_frame.cpp), HEALTH and EVENT payloads
#                     (unit/test_health.cpp), update receiver with a fake flash and the build's
#                     signed images (unit/test_update.cpp), arm state (unit/test_arm.cpp), mode
#                     manager, trial tests, SWITCH and CONFIRM (unit/test_mode.cpp), the
#                     bootloader's slot handling with the chip replaced (unit/test_boot.c), the
#                     order of its steps: watchdog, start count, park loop (unit/test_flow.c),
#                     its watchdog module (unit/test_watchdog.c) and the flash address barrier
#                     (unit/test_flash.c)
#   make link-probe   talk to the running firmware over data link A (link_probe.py)
#   make upload-probe upload firmware into the slot that is not running, plus refused
#                     uploads (upload_probe.py)
#   make trial-probe  SCENARIO=confirm|bad-image|timeout: switch to the other slot, run its
#                     trial, then confirm it or watch it given up (trial_probe.py)
#   make set-trial    boot state "confirmed $(CONFIRMED), trial $(TRIAL)" (default A, B), only
#                     the boot-state pages rewritten, then restart: the bootloader checks and
#                     starts the slot upload-probe wrote
#   make flash        same as scenario-1
#   make scenario-N   write the flash contents of bench test N (1-7), then restart the chip
#
# Bench tests (expected "BL:"/"APP" lines in parentheses):
#   1  good image in slot A, no boot state   (slot A accepted; starting slot A (confirmed))
#   2  hanging trial in slot B, trial = B    (3 trial starts ended by the watchdog; trial given
#                                             up; starting slot A (confirmed))
#   3  slot A with one body byte flipped     (slot A refused: body check value; last-resort runs)
#   4  B (trial) and A signed with an        (slot B refused: signature B; trial given up;
#      unknown key                            slot A refused: signature A; last-resort runs)
#   5  both slots erased                     (slot A refused: empty; last-resort runs)
#   6  only the bootloader                   (no firmware can run)
#   7  good trial in slot B, trial = B       (B runs, confirms itself; after `make reset`:
#                                             starting slot B (confirmed))
# Use `make -C ../../src/controller reset` / `monitor` to restart and to read the board.

include ../../src/controller/bench.mk

SRC   = $(CTRL)
OUT   = build/images
APPS  = test-firmware/build
SIGN_BAD_A = $(PY) $(TOOLS)/make_image.py --key-a $(KEYS)/key-wrong.pem --key-b $(KEYS)/key-b.pem
SIGN_BAD_B = $(PY) $(TOOLS)/make_image.py --key-a $(KEYS)/key-a.pem --key-b $(KEYS)/key-wrong.pem

# A code byte well inside the program (past the 0x200 offset and the 472-byte vector table),
# so the damage is in code, not in padding.
FLIP_OFFSET = 0x600

all: images

programs:
	$(MAKE) -C $(SRC) programs
	$(MAKE) -C test-firmware SLOT=A  VERSION=1 BEHAVIOUR=confirm
	$(MAKE) -C test-firmware SLOT=B  VERSION=2 BEHAVIOUR=confirm
	$(MAKE) -C test-firmware SLOT=B  VERSION=2 BEHAVIOUR=hang
	$(MAKE) -C test-firmware SLOT=LR VERSION=1 BEHAVIOUR=confirm

images: programs
	mkdir -p $(OUT)
	$(SIGN) --bin $(APPS)/A-v1-confirm/app.bin  --kind main --slot A --version 1 --build-id "$(COMMIT) test A" --out $(OUT)/slotA-v1.img
	$(SIGN) --bin $(APPS)/B-v2-confirm/app.bin  --kind main --slot B --version 2 --build-id "$(COMMIT) test B" --out $(OUT)/slotB-v2.img
	$(SIGN) --bin $(APPS)/B-v2-hang/app.bin     --kind main --slot B --version 2 --build-id "$(COMMIT) test B hang" --out $(OUT)/slotB-v2-hang.img
	$(SIGN) --bin $(APPS)/LR-v1-confirm/app.bin --kind last-resort --slot last-resort --version 1 --build-id "$(COMMIT) test LR" --out $(OUT)/last-resort-v1.img
	$(SIGN_BAD_A) --bin $(APPS)/A-v1-confirm/app.bin --kind main --slot A --version 1 --build-id "$(COMMIT) bad key A" --out $(OUT)/slotA-v1-wrong-key-a.img
	$(SIGN_BAD_B) --bin $(APPS)/B-v2-confirm/app.bin --kind main --slot B --version 2 --build-id "$(COMMIT) bad key B" --out $(OUT)/slotB-v2-wrong-key-b.img
	$(PY) $(TOOLS)/flip_byte.py $(OUT)/slotA-v1.img $(FLIP_OFFSET) $(OUT)/slotA-v1-flipped.img
	$(PY) $(TOOLS)/make_boot_state.py --confirmed A --trial B --out $(OUT)/state-trial-B.bin
	# The operating firmware signed with an unknown key A: UPLOAD END must refuse it 0x09.
	$(SIGN_BAD_A) --bin $(SRC)/firmware/build/A/firmware.bin --kind main --slot A --version 1 --build-id "$(COMMIT) fw A bad key" --out $(OUT)/firmware-A-wrong-key-a.img
	$(SIGN_BAD_A) --bin $(SRC)/firmware/build/B/firmware.bin --kind main --slot B --version 1 --build-id "$(COMMIT) fw B bad key" --out $(OUT)/firmware-B-wrong-key-a.img

check: images unit
	$(PY) $(TOOLS)/check_crc32.py build/host
	$(PY) $(TOOLS)/check_images.py $(OUT) build/host

# Firmware parts compiled for this computer. The undefined-behaviour sanitizer stops the run
# on any index outside a plain C array (the address sanitizer does not run on this Mac).
# Monocypher is compiled apart, as shipped: no -Werror, no sanitizer.
# The shared C files (common/) are compiled as C, with HOST_CC; the firmware's C++ files and
# the tests with HOST_CXX: the firmware's language and its flags (C++17, no exceptions, no
# RTTI, the same warnings), the same sanitizer. That sanitizer does not see an index into a
# std::array; libc++'s debug hardening checks those, and stops the run with the failed bound
# printed.
# -Wswitch-enum on every host build, as on the board (common/build.mk): a switch over an enum
# must name every enumerator, even with a default.
HOST_CC    = cc -std=c11 -O1 -g -Wall -Wextra -Wswitch-enum -Werror -fsanitize=undefined -fno-sanitize-recover=undefined
HOST_CXX   = clang++ -std=c++17 -O1 -g -fno-exceptions -fno-rtti \
             -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast \
             -Wcast-align -Wnull-dereference -Wformat=2 -Wswitch-enum -Werror \
             -fsanitize=undefined -fno-sanitize-recover=undefined \
             -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG
MONO       = $(REPO)/third_party/monocypher
COMMON     = $(SRC)/common
FW_SRC     = $(SRC)/firmware
LINK_SRC   = $(FW_SRC)/data-link
UPD_SRC    = $(FW_SRC)/update-receiver
HEALTH_SRC = $(FW_SRC)/health-monitor
MODE_SRC   = $(FW_SRC)/mode-manager
CMD_SRC    = $(FW_SRC)/command-checker
CXX_INCLUDES = -Iunit -I$(FW_SRC) -I$(LINK_SRC) -I$(UPD_SRC) -I$(HEALTH_SRC) -I$(MODE_SRC) \
               -I$(CMD_SRC) -I$(COMMON)
# Every firmware and common header: a test is rebuilt when any of them changes.
FW_HEADERS = $(wildcard $(FW_SRC)/*.hpp $(LINK_SRC)/*.hpp $(UPD_SRC)/*.hpp $(HEALTH_SRC)/*.hpp \
                        $(MODE_SRC)/*.hpp $(CMD_SRC)/*.hpp $(COMMON)/*.h unit/*.hpp)
FRAME_SRCS  = unit/test_frame.cpp $(LINK_SRC)/frame.cpp
UPDATE_SRCS = unit/test_update.cpp unit/fake_port.cpp $(UPD_SRC)/update_receiver.cpp
HEALTH_SRCS = unit/test_health.cpp $(HEALTH_SRC)/health_monitor.cpp
ARM_SRCS    = unit/test_arm.cpp $(CMD_SRC)/arm_state.cpp
# The mode manager with everything it is wired to in main.cpp; the chip and the UART replaced
# by fakes (fake_port.cpp, fake_mode_port.cpp, fake_uart_link.cpp).
MODE_SRCS   = unit/test_mode.cpp unit/fake_port.cpp unit/fake_mode_port.cpp \
              unit/fake_uart_link.cpp $(MODE_SRC)/mode_manager.cpp $(MODE_SRC)/test_image.cpp \
              $(CMD_SRC)/arm_state.cpp $(LINK_SRC)/frame_sender.cpp $(LINK_SRC)/frame.cpp \
              $(HEALTH_SRC)/health_monitor.cpp $(UPD_SRC)/update_receiver.cpp
COMMON_OBJS = build/host/crc32.o build/host/image_check.o
# The bootloader's own main.c with the chip replaced (unit/boot_stubs), and boot_state.c as built.
BOOT_SRCS   = unit/test_boot.c unit/boot_stubs/boot_stubs.c $(COMMON)/boot_state.c
BOOT_HDRS   = $(wildcard $(SRC)/bootloader/*.h unit/boot_stubs/*.h)
# The bootloader's whole main() with the chip, the boot state and the image checks replaced.
FLOW_SRCS   = unit/test_flow.c unit/boot_stubs/boot_stubs.c
# watchdog.c and flash.c as built, each against its own register stand-in (unit/*_stubs).
# flash.c turns a 32-bit address into a pointer, which the chip's 32-bit cores allow and this
# computer's 64-bit pointers warn about: -Wno-int-to-pointer-cast for that one file (that code is
# never run here: only the refusals and a zero-length program are).
WDOG_SRCS   = unit/test_watchdog.c $(SRC)/bootloader/watchdog.c
FLASH_SRCS  = unit/test_flash.c $(COMMON)/flash.c
MONO_OBJS   = build/host/monocypher.o build/host/monocypher-ed25519.o

build/host/%.o: $(MONO)/%.c $(MONO)/%.h
	mkdir -p build/host
	cc -std=c11 -O2 -c $< -o $@

$(COMMON_OBJS): build/host/%.o: $(COMMON)/%.c $(wildcard $(COMMON)/*.h)
	mkdir -p build/host
	$(HOST_CC) -I$(COMMON) -I$(MONO) -c $< -o $@

build/host/test_frame: $(FRAME_SRCS) build/host/crc32.o $(FW_HEADERS)
	$(HOST_CXX) $(CXX_INCLUDES) $(FRAME_SRCS) build/host/crc32.o -o $@

build/host/test_update: $(UPDATE_SRCS) $(COMMON_OBJS) $(MONO_OBJS) $(FW_HEADERS)
	$(HOST_CXX) $(CXX_INCLUDES) $(UPDATE_SRCS) $(COMMON_OBJS) $(MONO_OBJS) -o $@

build/host/test_health: $(HEALTH_SRCS) $(COMMON_OBJS) $(MONO_OBJS) $(FW_HEADERS)
	$(HOST_CXX) $(CXX_INCLUDES) $(HEALTH_SRCS) $(COMMON_OBJS) $(MONO_OBJS) -o $@

build/host/test_boot: $(BOOT_SRCS) $(COMMON_OBJS) $(MONO_OBJS) $(BOOT_HDRS) $(FW_HEADERS) \
                      $(SRC)/bootloader/main.c
	mkdir -p build/host
	$(HOST_CC) -Iunit/boot_stubs -I$(SRC)/bootloader -I$(COMMON) -I$(MONO) $(BOOT_SRCS) \
	    $(COMMON_OBJS) $(MONO_OBJS) -o $@

build/host/test_flow: $(FLOW_SRCS) $(BOOT_HDRS) $(FW_HEADERS) $(SRC)/bootloader/main.c
	mkdir -p build/host
	$(HOST_CC) -Iunit/boot_stubs -I$(SRC)/bootloader -I$(COMMON) $(FLOW_SRCS) -o $@

build/host/test_watchdog: $(WDOG_SRCS) $(wildcard $(SRC)/bootloader/*.h unit/watchdog_stubs/*.h)
	mkdir -p build/host
	$(HOST_CC) -Iunit/watchdog_stubs -I$(SRC)/bootloader -I$(COMMON) $(WDOG_SRCS) -o $@

build/host/test_flash: $(FLASH_SRCS) $(wildcard $(COMMON)/*.h unit/flash_stubs/*.h)
	mkdir -p build/host
	$(HOST_CC) -Wno-int-to-pointer-cast -Iunit/flash_stubs -I$(COMMON) $(FLASH_SRCS) -o $@

build/host/test_arm: $(ARM_SRCS) $(FW_HEADERS)
	mkdir -p build/host
	$(HOST_CXX) $(CXX_INCLUDES) $(ARM_SRCS) -o $@

build/host/test_mode: $(MODE_SRCS) $(COMMON_OBJS) $(MONO_OBJS) $(FW_HEADERS)
	$(HOST_CXX) $(CXX_INCLUDES) $(MODE_SRCS) $(COMMON_OBJS) $(MONO_OBJS) -o $@

# The operating firmware's signed images (firmware-A.img, firmware-B.img).
src-images:
	$(MAKE) -C $(SRC) images

unit: build/host/test_frame build/host/test_health build/host/test_update build/host/test_arm \
      build/host/test_mode build/host/test_boot build/host/test_flow build/host/test_watchdog \
      build/host/test_flash images src-images
	build/host/test_frame
	build/host/test_health
	build/host/test_update $(SRC)/build/images $(OUT)
	build/host/test_arm
	build/host/test_mode $(SRC)/build/images
	build/host/test_boot
	build/host/test_flow
	build/host/test_watchdog
	build/host/test_flash

# Needs the firmware running from slot A: make -C ../../src/controller flash-firmware.
# PROBE_ARGS="--listen 4": listen first, and reset the board meanwhile to see its BL: lines.
PROBE_ARGS ?=
link-probe:
	@test -n "$(PORT)" || (echo "no /dev/cu.usbmodem* port: is the board plugged in?"; false)
	$(PY) link_probe.py $(PORT) $(PROBE_ARGS)

# Needs the step-2c firmware running (make -C ../../src/controller flash-firmware). Learns the
# running slot from a HEALTH report, uploads into the other one stop-and-wait, and checks the
# refused uploads. In TRIAL mode it checks the 0x07 refusals instead. PROBE_ARGS as above.
upload-probe: images src-images
	@test -n "$(PORT)" || (echo "no /dev/cu.usbmodem* port: is the board plugged in?"; false)
	$(PY) upload_probe.py $(PORT) --images $(SRC)/build/images --test-images $(OUT) $(PROBE_ARGS)

# Needs the step-2d firmware running in OPERATE (make -C ../../src/controller flash-firmware).
# Uploads the other slot's firmware (PROBE_ARGS="--no-upload" if it is already there), then
# ARM + SWITCH, the trial, and the end SCENARIO names. timeout waits for the 120 s trial time.
SCENARIO ?= confirm
trial-probe: src-images
	@test -n "$(PORT)" || (echo "no /dev/cu.usbmodem* port: is the board plugged in?"; false)
	$(PY) trial_probe.py $(PORT) --images $(SRC)/build/images --scenario $(SCENARIO) $(PROBE_ARGS)

# Boot-state pages, as programmer page numbers: page = (address - flash start) / 2 KB.
CONFIRMED ?= A
TRIAL     ?= B
FLASH0      = $(call ADDR,FLASH_START)
PAGE        = $(call ADDR,FLASH_PAGE_BYTES)
STATE_FIRST = $(shell echo $$(( ($(ADDR_STATE) - $(FLASH0)) / $(PAGE) )))
STATE_LAST  = $(shell echo $$(( ($(ADDR_STATE) - $(FLASH0) + 2 * $(call ADDR,BOOT_STATE_COPY_BYTES)) / $(PAGE) - 1 )))
# Both copies erased, so the record written here (sequence 1) is the only valid one.
set-trial:
	$(PY) $(TOOLS)/make_boot_state.py --confirmed $(CONFIRMED) --trial $(TRIAL) --out $(OUT)/state-set-trial.bin
	$(CLI) -e [$(STATE_FIRST) $(STATE_LAST)] -w $(OUT)/state-set-trial.bin $(ADDR_STATE) -v -rst

# ---- Writing the board through the ST-LINK (SWD) -------------------------------------
# -e all erases everything first, the boot state included.
W_LR    = -w $(OUT)/last-resort-v1.img $(ADDR_LR) -v
W_TRIAL = -w $(OUT)/state-trial-B.bin $(ADDR_STATE) -v

flash scenario-1: images
	$(CLI) -e all $(W_BL) -w $(OUT)/slotA-v1.img $(ADDR_A) -v $(W_LR) -rst
scenario-2: images
	$(CLI) -e all $(W_BL) -w $(OUT)/slotA-v1.img $(ADDR_A) -v -w $(OUT)/slotB-v2-hang.img $(ADDR_B) -v $(W_LR) $(W_TRIAL) -rst
scenario-3: images
	$(CLI) -e all $(W_BL) -w $(OUT)/slotA-v1-flipped.img $(ADDR_A) -v $(W_LR) -rst
scenario-4: images
	$(CLI) -e all $(W_BL) -w $(OUT)/slotA-v1-wrong-key-a.img $(ADDR_A) -v -w $(OUT)/slotB-v2-wrong-key-b.img $(ADDR_B) -v $(W_LR) $(W_TRIAL) -rst
scenario-5: images
	$(CLI) -e all $(W_BL) $(W_LR) -rst
scenario-6: images
	$(CLI) -e all $(W_BL) -rst
scenario-7: images
	$(CLI) -e all $(W_BL) -w $(OUT)/slotA-v1.img $(ADDR_A) -v -w $(OUT)/slotB-v2.img $(ADDR_B) -v $(W_LR) $(W_TRIAL) -rst

clean:
	rm -rf test-firmware/build build

.PHONY: all programs images src-images check unit link-probe upload-probe trial-probe set-trial \
        flash clean \
        scenario-1 scenario-2 scenario-3 scenario-4 scenario-5 scenario-6 scenario-7
