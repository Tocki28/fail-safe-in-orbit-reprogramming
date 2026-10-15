# Makefile — the bootloader, linked at the start of flash (layout.h: BOOTLOADER_START).
#   make         build/bootloader.bin
#   make clean

NAME  = bootloader
BUILD = build
TOP  := $(abspath ../../..)
MONO  = $(TOP)/third_party/monocypher

# No common/*.c: the bootloader shares no code with the firmware (REQ-119, owner's decision 35,
# 15 Oct 2026). Its CRC-32, flash access, boot state and image checks are its own bl_*.c files,
# written apart from common/crc32.c, flash.c, boot_state.c and image_check.c. From common/ it
# takes headers of definitions only, plus build.mk and program.ld below. Monocypher and ST's
# startup and system files are still shared: an exception the owner has not decided yet.
SRCS = main.c clock.c uart.c handover.c watchdog.c backup.c fault.c \
       bl_crc32.c bl_flash.c bl_boot_state.c bl_image_check.c
SRCS_EXT = $(MONO)/monocypher.c $(MONO)/monocypher-ed25519.c
HEADERS  = $(wildcard *.h)
INCLUDES = -I. -I$(MONO)

CODE_START = BOOTLOADER_START
CODE_BYTES = BOOTLOADER_BYTES

# -O2: the signature check is most of the start-up time; -Os would save flash but slow it.
OPT = -O2

include ../common/build.mk
