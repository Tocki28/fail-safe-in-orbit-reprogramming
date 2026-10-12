# Makefile — bench-only test tools for the controller: test firmware, signed test images
# (good, hanging, flipped, wrong key) and the bench scenarios. Never part of the operating
# software (REQ-101). The operating programs are built by src/controller/Makefile.
#
#   make              build the bootloader and the test firmware, sign the test images
#   make check        host checks: CRC-32 against zlib; every image through the bootloader's
#                     own check code (tools/check_images.py); the frame unit tests
#   make unit         frame unit tests only (unit/test_frame.c, built with the Mac's cc)
#   make link-probe   talk to the running firmware over data link A (link_probe.py)
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

check: images unit
	$(PY) $(TOOLS)/check_crc32.py build/host
	$(PY) $(TOOLS)/check_images.py $(OUT) build/host

# The frame layer compiled for this computer. The undefined-behaviour sanitizer stops the run
# on any array index out of bounds (the address sanitizer does not run on this Mac).
LINK_SRC  = $(SRC)/firmware/data-link
UNIT_SRCS = unit/test_frame.c $(LINK_SRC)/frame.c $(SRC)/common/crc32.c
build/host/test_frame: $(UNIT_SRCS) $(LINK_SRC)/frame.h $(SRC)/common/crc32.h
	mkdir -p build/host
	cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=undefined \
	   -I$(LINK_SRC) -I$(SRC)/common $(UNIT_SRCS) -o $@

unit: build/host/test_frame
	build/host/test_frame

# Needs the firmware running from slot A: make -C ../../src/controller flash-firmware.
# PROBE_ARGS="--listen 4": listen first, and reset the board meanwhile to see its BL: lines.
PROBE_ARGS ?=
link-probe:
	@test -n "$(PORT)" || (echo "no /dev/cu.usbmodem* port: is the board plugged in?"; false)
	$(PY) link_probe.py $(PORT) $(PROBE_ARGS)

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

.PHONY: all programs images check unit link-probe flash clean \
        scenario-1 scenario-2 scenario-3 scenario-4 scenario-5 scenario-6 scenario-7
