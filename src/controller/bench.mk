# bench.mk — settings shared by src/controller/Makefile and test/controller/Makefile:
# repository paths, development keys, image signing, Python, the programmer, and the flash
# region addresses.

CTRL  := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO  := $(abspath $(CTRL)/../..)
TOOLS  = $(REPO)/tools
# Development keys and the Python that has `cryptography` sit next to the repository,
# outside it, so private keys never enter it.
KEYS  ?= $(abspath $(REPO)/../keys-dev)
PY    ?= $(abspath $(REPO)/../.venv/bin/python)
PROG  ?= /Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.macos64_2.2.200.202503041107/tools/bin/STM32_Programmer_CLI
PORT  ?= $(firstword $(wildcard /dev/cu.usbmodem*))

BL    = $(CTRL)/bootloader/build/bootloader.bin

# Build identifier written into each image header: commit, "-dirty" if the tree has changes.
COMMIT := $(shell git -C $(REPO) rev-parse --short HEAD 2>/dev/null || echo none)$(shell git -C $(REPO) diff --quiet HEAD 2>/dev/null || echo -dirty)
SIGN   = $(PY) $(TOOLS)/make_image.py --key-a $(KEYS)/key-a.pem --key-b $(KEYS)/key-b.pem

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

# mode=UR: connect while the chip is held in reset, so a running watchdog cannot restart it
# during programming. The bootloader starts the independent watchdog as the first line of its
# main() (bootloader/main.c, decision A), so it runs within microseconds of any start; once
# running it cannot be stopped [RM0440 Rev 9, Sec. 35.3.1, p. 1557], and the firmware keeps it
# with a time-out of about 2 s (bootloader/watchdog.c). Connected under reset, the core is
# halted before that first line, so no code starts the watchdog while the programmer works.
CLI     = "$(PROG)" -c port=SWD mode=UR
W_BL    = -w $(BL) $(ADDR_BL) -v
