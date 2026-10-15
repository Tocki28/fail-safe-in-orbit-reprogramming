# Makefile — the controller's main firmware (C++17), linked for slot A or slot B.
#   make SLOT=A   -> build/A/firmware.bin
#   make SLOT=B   -> build/B/firmware.bin
#   make clean
# The .bin becomes a signed image in ../Makefile (tools/make_image.py).
# The C version of these files is in the git history (before the C++ port, commit 6fabb28).

SLOT ?= A

NAME  = firmware
BUILD = build/$(SLOT)
TOP  := $(abspath ../../..)
MONO  = $(TOP)/third_party/monocypher

# Our C++: one class per component (common/build.mk has the C++ flags). Object files are named
# after the source without its folder, so every .cpp name must be unique.
SRCS_CXX = main.cpp core_clock.cpp data-link/uart_link.cpp data-link/frame.cpp \
           data-link/frame_sender.cpp \
           update-receiver/update_receiver.cpp update-receiver/update_port.cpp \
           health-monitor/health_monitor.cpp \
           mode-manager/mode_manager.cpp mode-manager/test_image.cpp mode-manager/mode_port.cpp \
           command-checker/arm_state.cpp
# common/ stays C, the firmware's own copy (the bootloader has its own bl_ files since decision
# 35): the image checks run at UPLOAD END, boot state, flash, CRC-32. Its headers declare
# extern "C" for C++ callers.
SRCS     = ../common/image_check.c ../common/boot_state.c ../common/flash.c ../common/crc32.c
SRCS_EXT = $(MONO)/monocypher.c $(MONO)/monocypher-ed25519.c
HEADERS  = $(wildcard *.hpp data-link/*.hpp update-receiver/*.hpp health-monitor/*.hpp \
                      mode-manager/*.hpp command-checker/*.hpp)
INCLUDES = -I. -Idata-link -Iupdate-receiver -Ihealth-monitor -Imode-manager -Icommand-checker \
           -I$(MONO)

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
