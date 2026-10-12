# Makefile — TEST firmware for the bootloader bench tests (not the operating firmware; REQ-101).
#   make SLOT=A  VERSION=1 BEHAVIOUR=confirm   -> build/A-v1-confirm/app.bin
#   make SLOT=B  VERSION=2 BEHAVIOUR=hang      -> build/B-v2-hang/app.bin
#   make SLOT=LR VERSION=1                     -> build/LR-v1-confirm/app.bin (never confirms)
# The .bin is then made into a signed image by tools/make_image.py (see ../Makefile).

SLOT      ?= A
VERSION   ?= 1
BEHAVIOUR ?= confirm

NAME  = app
BUILD = build/$(SLOT)-v$(VERSION)-$(BEHAVIOUR)
CTRL  = ../../../src/controller
SRCS  = main.c $(CTRL)/common/boot_state.c $(CTRL)/common/flash.c $(CTRL)/common/crc32.c

ifeq ($(SLOT),A)
  CODE_START = SLOT_A_CODE_START
  CODE_BYTES = SLOT_CODE_BYTES
  APP_SLOT   = BOOT_SLOT_A
else ifeq ($(SLOT),B)
  CODE_START = SLOT_B_CODE_START
  CODE_BYTES = SLOT_CODE_BYTES
  APP_SLOT   = BOOT_SLOT_B
else ifeq ($(SLOT),LR)
  CODE_START = LAST_RESORT_CODE_START
  CODE_BYTES = LAST_RESORT_CODE_BYTES
  APP_SLOT   = BOOT_SLOT_NONE
else
  $(error SLOT must be A, B or LR)
endif

ifeq ($(BEHAVIOUR),confirm)
  APP_HANG = 0
else ifeq ($(BEHAVIOUR),hang)
  APP_HANG = 1
else
  $(error BEHAVIOUR must be confirm or hang)
endif

DEFS = -DAPP_SLOT=$(APP_SLOT) -DAPP_NAME='"$(SLOT)"' -DAPP_VERSION=$(VERSION)U \
       -DAPP_HANG=$(APP_HANG) -DCODE_START=$(CODE_START)
OPT  = -Os

include $(CTRL)/common/build.mk
