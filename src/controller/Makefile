# Makefile — the operating software of the controller: the bootloader (more programs as they
# are written). Bench-only test tools are in test/controller/ (REQ-101).
#
#   make              build the operating programs
#   make check        forwards to test/controller: host checks (CRC-32 against zlib; every
#                     test image through the bootloader's own check code)
#   make keys         once: development key pairs (outside the repository) + common/keys.h
#   make flash        bootloader only; everything else erased (bench scenarios: test/controller)
#   make reset        restart the chip
#   make monitor      print the board's USART2 text (115200 8N1); PORT=/dev/cu.usbmodem...

include bench.mk

all: programs

programs:
	$(MAKE) -C bootloader

check:
	$(MAKE) -C $(REPO)/test/controller check

keys:
	$(PY) $(TOOLS)/make_dev_keys.py $(KEYS)
	$(PY) $(TOOLS)/make_keys_header.py $(KEYS)/key-a.pem $(KEYS)/key-b.pem common/keys.h

# ---- Writing the board through the ST-LINK (SWD) -------------------------------------
# -e all erases everything first, the boot state included.
flash: programs
	$(CLI) -e all $(W_BL) -rst

# Restart only.
reset:
	$(CLI) -rst

# stty on the port's own file descriptor, then cat it: start before the reset to see it all.
monitor:
	@test -n "$(PORT)" || (echo "no /dev/cu.usbmodem* port: is the board plugged in?"; false)
	(stty 115200 cs8 -cstopb -parenb raw -echo; cat) < $(PORT)

clean:
	$(MAKE) -C bootloader clean

.PHONY: all programs check keys flash reset monitor clean
