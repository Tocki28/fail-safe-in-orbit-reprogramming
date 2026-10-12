# Makefile — the bootloader, linked at the start of flash (layout.h: BOOTLOADER_START).
#   make         build/bootloader.bin
#   make clean

NAME  = bootloader
BUILD = build
TOP  := $(abspath ../../..)
MONO  = $(TOP)/third_party/monocypher

SRCS = main.c clock.c uart.c handover.c image_check.c \
       ../common/crc32.c ../common/flash.c ../common/boot_state.c
SRCS_EXT = $(MONO)/monocypher.c $(MONO)/monocypher-ed25519.c
HEADERS  = $(wildcard *.h)
INCLUDES = -I. -I$(MONO)

CODE_START = BOOTLOADER_START
CODE_BYTES = BOOTLOADER_BYTES

# -O2: the signature check is most of the start-up time; -Os would save flash but slow it.
OPT = -O2

include ../common/build.mk
