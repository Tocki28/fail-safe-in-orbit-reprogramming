# Makefile — the operating software of the controller: the bootloader and the main firmware
# (more programs as they are written). Bench-only test tools are in test/controller/ (REQ-101).
#
#   make              build the operating programs and sign the firmware images:
#                     build/images/firmware-A.img, firmware-B.img (kind main), and
#                     state-confirmed-A.bin (boot state: confirmed A, no trial)
#   make check        forwards to test/controller: host checks (CRC-32 against zlib; every
#                     test image through the bootloader's own check code)
#   make keys         once: development key pairs (outside the repository) + common/keys.h
#   make flash        bootloader only; everything else erased (bench scenarios: test/controller)
#   make flash-firmware  bootloader + firmware in slot A + boot state "confirmed A"; the
#                     firmware does not confirm itself yet (step 2d), so it runs as confirmed
#   make reset        restart the chip
#   make monitor      print the board's USART2 text (115200 8N1); PORT=/dev/cu.usbmodem...

include bench.mk

OUT = build/images
FW  = firmware/build
# Version written into the firmware image header.
FW_VERSION ?= 1

all: images

programs:
	$(MAKE) -C bootloader
	$(MAKE) -C firmware SLOT=A
	$(MAKE) -C firmware SLOT=B

images: programs
	mkdir -p $(OUT)
	$(SIGN) --bin $(FW)/A/firmware.bin --kind main --slot A --version $(FW_VERSION) --build-id "$(COMMIT) firmware A" --out $(OUT)/firmware-A.img
	$(SIGN) --bin $(FW)/B/firmware.bin --kind main --slot B --version $(FW_VERSION) --build-id "$(COMMIT) firmware B" --out $(OUT)/firmware-B.img
	$(PY) $(TOOLS)/make_boot_state.py --confirmed A --out $(OUT)/state-confirmed-A.bin

check:
	$(MAKE) -C $(REPO)/test/controller check

keys:
	$(PY) $(TOOLS)/make_dev_keys.py $(KEYS)
	$(PY) $(TOOLS)/make_keys_header.py $(KEYS)/key-a.pem $(KEYS)/key-b.pem common/keys.h

# ---- Writing the board through the ST-LINK (SWD) -------------------------------------
# -e all erases everything first, the boot state included.
flash: programs
	$(CLI) -e all $(W_BL) -rst

flash-firmware: images
	$(CLI) -e all $(W_BL) -w $(OUT)/firmware-A.img $(ADDR_A) -v -w $(OUT)/state-confirmed-A.bin $(ADDR_STATE) -v -rst

# Restart only.
reset:
	$(CLI) -rst

# stty on the port's own file descriptor, then cat it: start before the reset to see it all.
monitor:
	@test -n "$(PORT)" || (echo "no /dev/cu.usbmodem* port: is the board plugged in?"; false)
	(stty 115200 cs8 -cstopb -parenb raw -echo; cat) < $(PORT)

clean:
	$(MAKE) -C bootloader clean
	rm -rf firmware/build build

.PHONY: all programs images check keys flash flash-firmware reset monitor clean
