# bench.mk — settings shared by src/controller/Makefile and test/controller/Makefile:
# repository paths, development keys, Python, the programmer, and the flash region addresses.

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
# during programming (after reset the watchdog is off until the bootloader starts it).
CLI     = "$(PROG)" -c port=SWD mode=UR
W_BL    = -w $(BL) $(ADDR_BL) -v
