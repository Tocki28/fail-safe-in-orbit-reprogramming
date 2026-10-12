# Makefile — the controller's main firmware, linked for slot A or slot B.
#   make SLOT=A   -> build/A/firmware.bin
#   make SLOT=B   -> build/B/firmware.bin
#   make clean
# The .bin becomes a signed image in ../Makefile (tools/make_image.py).

SLOT ?= A

NAME  = firmware
BUILD = build/$(SLOT)
TOP  := $(abspath ../../..)
MONO  = $(TOP)/third_party/monocypher

# common/: the image checks the bootloader also runs (UPLOAD END), boot state, flash, CRC-32.
SRCS  = main.c core_clock.c data-link/uart_link.c data-link/frame.c \
        update-receiver/update_receiver.c update-receiver/update_port.c \
        health-monitor/health_monitor.c \
        ../common/image_check.c ../common/boot_state.c ../common/flash.c ../common/crc32.c
SRCS_EXT = $(MONO)/monocypher.c $(MONO)/monocypher-ed25519.c
HEADERS  = $(wildcard *.h data-link/*.h update-receiver/*.h health-monitor/*.h)
INCLUDES = -I. -Idata-link -Iupdate-receiver -Ihealth-monitor -I$(MONO)

ifeq ($(SLOT),A)
  CODE_START = SLOT_A_CODE_START
else ifeq ($(SLOT),B)
  CODE_START = SLOT_B_CODE_START
else
  $(error SLOT must be A or B)
endif
CODE_BYTES = SLOT_CODE_BYTES
DEFS = -DCODE_START=$(CODE_START)

# -Os: nothing here is time-critical; the UPLOAD END check (Ed25519) is slower than at the
# bootloader's -O2 but runs once per update. Smaller code is easier to read in the map file.
OPT = -Os

include ../common/build.mk
