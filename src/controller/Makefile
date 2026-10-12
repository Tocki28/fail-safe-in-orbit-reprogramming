# Makefile — the controller bench: bootloader, test applications, signed images, tests.
#
#   make              build the bootloader and the test applications, sign the test images
#   make check        host checks: CRC-32 against zlib; every image through the bootloader's
#                     own check code (tools/check_images.py)
#   make keys         once: development key pairs (outside the repository) + common/keys.h
#   make flash        bootloader + good slot A + last-resort; boot state erased
#   make scenario-N   write the flash contents of bench test N (1-7), then restart the chip
#   make monitor      print the board's USART2 text (115200 8N1); PORT=/dev/cu.usbmodem...
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

REPO  := $(abspath ../..)
TOOLS  = $(REPO)/tools
# Development keys and the Python that has `cryptography` sit next to the repository,
# outside it, so private keys never enter it.
KEYS  ?= $(abspath $(REPO)/../keys-dev)
PY    ?= $(abspath $(REPO)/../.venv/bin/python)
PROG  ?= /Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.macos64_2.2.200.202503041107/tools/bin/STM32_Programmer_CLI
PORT  ?= $(firstword $(wildcard /dev/cu.usbmodem*))

OUT   = build/images
APPS  = test-app/build
BL    = bootloader/build/bootloader.bin
# Build identifier written into each image header: commit, "-dirty" if the tree has changes.
COMMIT := $(shell git -C $(REPO) rev-parse --short HEAD 2>/dev/null || echo none)$(shell git -C $(REPO) diff --quiet HEAD 2>/dev/null || echo -dirty)

SIGN  = $(PY) $(TOOLS)/make_image.py --key-a $(KEYS)/key-a.pem --key-b $(KEYS)/key-b.pem
SIGN_BAD_A = $(PY) $(TOOLS)/make_image.py --key-a $(KEYS)/key-wrong.pem --key-b $(KEYS)/key-b.pem
SIGN_BAD_B = $(PY) $(TOOLS)/make_image.py --key-a $(KEYS)/key-a.pem --key-b $(KEYS)/key-wrong.pem

# A code byte well inside the program (past the 0x200 offset and the 472-byte vector table),
# so the damage is in code, not in padding.
FLIP_OFFSET = 0x600

# Region addresses for the programmer, read from common/layout.h.
ADDR = $(shell $(PY) $(TOOLS)/firmware_layout.py $(1))
ADDR_BL    := $(call ADDR,BOOTLOADER_START)
ADDR_A     := $(call ADDR,SLOT_A_START)
ADDR_B     := $(call ADDR,SLOT_B_START)
ADDR_LR    := $(call ADDR,LAST_RESORT_START)
ADDR_STATE := $(call ADDR,BOOT_STATE_START)
ifeq ($(words $(ADDR_BL) $(ADDR_A) $(ADDR_B) $(ADDR_LR) $(ADDR_STATE)),5)
else
  $(error could not read the region addresses from common/layout.h with $(PY))
endif

all: images

programs:
	$(MAKE) -C bootloader
	$(MAKE) -C test-app SLOT=A  VERSION=1 BEHAVIOUR=confirm
	$(MAKE) -C test-app SLOT=B  VERSION=2 BEHAVIOUR=confirm
	$(MAKE) -C test-app SLOT=B  VERSION=2 BEHAVIOUR=hang
	$(MAKE) -C test-app SLOT=LR VERSION=1 BEHAVIOUR=confirm

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

check: images
	$(PY) $(TOOLS)/check_crc32.py build/host
	$(PY) $(TOOLS)/check_images.py $(OUT) build/host

keys:
	$(PY) $(TOOLS)/make_dev_keys.py $(KEYS)
	$(PY) $(TOOLS)/make_keys_header.py $(KEYS)/key-a.pem $(KEYS)/key-b.pem common/keys.h

# ---- Writing the board through the ST-LINK (SWD) -------------------------------------
# mode=UR: connect while the chip is held in reset, so a running watchdog cannot restart it
# during programming (after reset the watchdog is off until the bootloader starts it).
# -e all erases everything first, the boot state included.
CLI     = "$(PROG)" -c port=SWD mode=UR
W_BL    = -w $(BL) $(ADDR_BL) -v
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

# Restart only (e.g. to see scenario 7's second start).
reset:
	$(CLI) -rst

# stty on the port's own file descriptor, then cat it: start before the reset to see it all.
monitor:
	@test -n "$(PORT)" || (echo "no /dev/cu.usbmodem* port: is the board plugged in?"; false)
	(stty 115200 cs8 -cstopb -parenb raw -echo; cat) < $(PORT)

clean:
	$(MAKE) -C bootloader clean
	rm -rf test-app/build build

.PHONY: all programs images check keys flash reset monitor clean \
        scenario-1 scenario-2 scenario-3 scenario-4 scenario-5 scenario-6 scenario-7
