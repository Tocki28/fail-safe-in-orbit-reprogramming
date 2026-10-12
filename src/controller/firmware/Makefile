# Makefile — the controller's main firmware, linked for slot A or slot B.
#   make SLOT=A   -> build/A/firmware.bin
#   make SLOT=B   -> build/B/firmware.bin
#   make clean
# The .bin becomes a signed image in ../Makefile (tools/make_image.py).

SLOT ?= A

NAME  = firmware
BUILD = build/$(SLOT)
SRCS  = main.c core_clock.c data-link/uart_link.c data-link/frame.c ../common/crc32.c
HEADERS  = $(wildcard *.h data-link/*.h)
INCLUDES = -I. -Idata-link

ifeq ($(SLOT),A)
  CODE_START = SLOT_A_CODE_START
else ifeq ($(SLOT),B)
  CODE_START = SLOT_B_CODE_START
else
  $(error SLOT must be A or B)
endif
CODE_BYTES = SLOT_CODE_BYTES
DEFS = -DCODE_START=$(CODE_START)

# -Os: nothing in this step is time-critical (the frame check value of a 1044-byte frame takes
# well under a millisecond); smaller code is easier to read in the map file.
OPT = -Os

include ../common/build.mk
