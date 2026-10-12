# Makefile — bench-only test tools for the controller: test firmware, signed test images
# (good, hanging, flipped, wrong key) and the bench scenarios. Never part of the operating
# software (REQ-101). The operating programs are built by src/controller/Makefile.
#
#   make              build the bootloader and the test firmware, sign the test images
#   make check        host checks: CRC-32 against zlib; every image through the bootloader's
#                     own check code (tools/check_images.py); the unit tests
#   make unit         unit tests, built with the Mac's cc: frame layer (unit/test_frame.c),
#                     HEALTH payload (unit/test_health.c), update receiver with a fake flash
#                     and the build's signed images (unit/test_update.c)
#   make link-probe   talk to the running firmware over data link A (link_probe.py)
#   make upload-probe upload firmware into the slot that is not running, plus refused
#                     uploads (upload_probe.py)
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
# on any array index out of bounds (the address sanitizer does not run on this Mac).
# Monocypher is compiled apart, as shipped: no -Werror, no sanitizer.
HOST_CC    = cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=undefined
MONO       = $(REPO)/third_party/monocypher
COMMON     = $(SRC)/common
LINK_SRC   = $(SRC)/firmware/data-link
UPD_SRC    = $(SRC)/firmware/update-receiver
HEALTH_SRC = $(SRC)/firmware/health-monitor
UNIT_SRCS  = unit/test_frame.c $(LINK_SRC)/frame.c $(COMMON)/crc32.c
UPDATE_SRCS = unit/test_update.c unit/fake_port.c $(UPD_SRC)/update_receiver.c \
              $(COMMON)/image_check.c $(COMMON)/crc32.c
HEALTH_SRCS = unit/test_health.c $(HEALTH_SRC)/health_monitor.c $(COMMON)/image_check.c \
              $(COMMON)/crc32.c
MONO_OBJS  = build/host/monocypher.o build/host/monocypher-ed25519.o

build/host/test_frame: $(UNIT_SRCS) $(LINK_SRC)/frame.h $(COMMON)/crc32.h
	mkdir -p build/host
	$(HOST_CC) -I$(LINK_SRC) -I$(COMMON) $(UNIT_SRCS) -o $@

build/host/%.o: $(MONO)/%.c $(MONO)/%.h
	mkdir -p build/host
	cc -std=c11 -O2 -c $< -o $@

build/host/test_update: $(UPDATE_SRCS) $(MONO_OBJS) unit/fake_port.h $(wildcard $(UPD_SRC)/*.h) \
                        $(wildcard $(COMMON)/*.h) $(LINK_SRC)/frame.h
	$(HOST_CC) -Iunit -I$(UPD_SRC) -I$(LINK_SRC) -I$(COMMON) -I$(MONO) $(UPDATE_SRCS) $(MONO_OBJS) -o $@

build/host/test_health: $(HEALTH_SRCS) $(MONO_OBJS) $(wildcard $(HEALTH_SRC)/*.h) $(wildcard $(COMMON)/*.h)
	$(HOST_CC) -I$(HEALTH_SRC) -I$(COMMON) -I$(MONO) $(HEALTH_SRCS) $(MONO_OBJS) -o $@

# The operating firmware's signed images (firmware-A.img, firmware-B.img).
src-images:
	$(MAKE) -C $(SRC) images

unit: build/host/test_frame build/host/test_health build/host/test_update images src-images
	build/host/test_frame
	build/host/test_health
	build/host/test_update $(SRC)/build/images $(OUT)

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

.PHONY: all programs images src-images check unit link-probe upload-probe set-trial flash clean \
        scenario-1 scenario-2 scenario-3 scenario-4 scenario-5 scenario-6 scenario-7
