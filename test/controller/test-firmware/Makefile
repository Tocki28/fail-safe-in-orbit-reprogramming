# Makefile — TEST firmware for the bootloader bench tests (not the operating firmware; REQ-101).
#   make SLOT=A  VERSION=1 BEHAVIOUR=confirm   -> build/A-v1-confirm/app.bin
#   make SLOT=B  VERSION=2 BEHAVIOUR=hang      -> build/B-v2-hang/app.bin
#   make SLOT=LR VERSION=1                     -> build/LR-v1-confirm/app.bin (never confirms)
#   make SLOT=B  VERSION=2 BEHAVIOUR=trial-forever -> build/B-v2-trial-forever/app.bin (framed
#       HEALTH reports in TRIAL every second, no text; never confirms, never gives up: exercises
#       the onboard computer's trial watch, decision H)
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
  APP_REPORT_SLOT = 0x01U
  APP_REPORT_KIND = IMAGE_KIND_MAIN
else ifeq ($(SLOT),B)
  CODE_START = SLOT_B_CODE_START
  CODE_BYTES = SLOT_CODE_BYTES
  APP_SLOT   = BOOT_SLOT_B
  APP_REPORT_SLOT = 0x02U
  APP_REPORT_KIND = IMAGE_KIND_MAIN
else ifeq ($(SLOT),LR)
  CODE_START = LAST_RESORT_CODE_START
  CODE_BYTES = LAST_RESORT_CODE_BYTES
  APP_SLOT   = BOOT_SLOT_NONE
  APP_REPORT_SLOT = 0x03U
  APP_REPORT_KIND = IMAGE_KIND_LAST_RESORT
else
  $(error SLOT must be A, B or LR)
endif

# The running-slot codes of the HEALTH report: Interfaces Sec. 5.4, report table (0x01 A, 0x02 B,
# 0x03 last-resort; health_monitor.hpp, ReportSlot). Only trial-forever uses them.
ifeq ($(BEHAVIOUR),confirm)
  APP_HANG = 0
  APP_TRIAL_FOREVER = 0
else ifeq ($(BEHAVIOUR),hang)
  APP_HANG = 1
  APP_TRIAL_FOREVER = 0
else ifeq ($(BEHAVIOUR),trial-forever)
  APP_HANG = 0
  APP_TRIAL_FOREVER = 1
else
  $(error BEHAVIOUR must be confirm, hang or trial-forever)
endif

DEFS = -DAPP_SLOT=$(APP_SLOT) -DAPP_NAME='"$(SLOT)"' -DAPP_VERSION=$(VERSION)U \
       -DAPP_HANG=$(APP_HANG) -DAPP_TRIAL_FOREVER=$(APP_TRIAL_FOREVER) \
       -DAPP_REPORT_SLOT=$(APP_REPORT_SLOT) -DAPP_REPORT_KIND=$(APP_REPORT_KIND) \
       -DCODE_START=$(CODE_START)
OPT  = -Os

include $(CTRL)/common/build.mk
