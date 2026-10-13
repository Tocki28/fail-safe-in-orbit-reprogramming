# Makefile — fsir-updater, the onboard computer's part of an update (bench build step 3).
#
#   make            build for this Mac: build/host/fsir-updater (for the tests)
#   make test       the host tests: unit tests and the fake-controller bench test
#                   (same as: make -C test/onboard-computer check)
#   make board      build for the board: build/board/fsir-updater, static, 64-bit ARM Linux,
#                   in Docker (the build image is made the first time)
#   make image      make the Docker build image from docker/Dockerfile
#   make clean      delete everything built
#
# Deploy and run on the board (STM32MP257F, reached over USB at 192.168.7.1):
#   scp build/board/fsir-updater root@192.168.7.1:/usr/local/bin/
#   ssh root@192.168.7.1 fsir-updater --device /dev/ttyACM0 --beat-s 2
#
# Compiler settings as in the owner's other C++ bench programs: no exceptions, no run-time
# type information, every warning on and warnings are errors.

REPO   := $(abspath ../../..)
# The controller's image format, flash layout and public keys (src/controller/common): one
# definition for the bootloader, the image tool and this program. Relative, so the same flags
# work inside the container, where the repository is mounted at /src.
COMMON  = ../../controller/common

SOURCES = $(wildcard *.cpp)
HEADERS = $(wildcard *.hpp) $(COMMON)/image_format.h $(COMMON)/layout.h $(COMMON)/keys.h

WARNINGS = -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow \
           -Wold-style-cast -Wcast-align -Wnull-dereference -Wformat=2 -Wswitch-enum -Werror
FLAGS    = -std=c++17 -O2 -fno-exceptions -fno-rtti $(WARNINGS)
INCLUDES = -I. -I$(COMMON)

# ---- This Mac --------------------------------------------------------------------------
# OpenSSL 3 from Homebrew (brew install openssl@3). -isystem: its headers are not ours to
# warn about.
OPENSSL ?= $(shell brew --prefix openssl@3 2>/dev/null)
HOST_CXX = clang++
HOST_SSL = -isystem $(OPENSSL)/include
HOST_LIB = -L$(OPENSSL)/lib -lcrypto

# ---- The board -------------------------------------------------------------------------
# Cortex-A35 (aarch64). -static: the program does not depend on the board's libraries; the
# linker warns that glibc's name lookups (getaddrinfo, dlopen) would need the board's shared
# libraries at run time: this program calls neither, it binds 0.0.0.0 by number.
BOARD_PLATFORM  = linux/arm64
BOARD_CPU       = cortex-a35
BOARD_FILE_TYPE = ARM aarch64
BUILD_IMAGE     = fsir-arm64-build
DOCKER_RUN = docker run --rm --platform $(BOARD_PLATFORM) -v "$(REPO)":/src \
             -w /src/src/onboard-computer/updater $(BUILD_IMAGE)

all: build/host/fsir-updater

build/host/fsir-updater: $(SOURCES) $(HEADERS)
	@test -n "$(OPENSSL)" || (echo "OpenSSL 3 not found: brew install openssl@3"; false)
	mkdir -p build/host
	$(HOST_CXX) $(FLAGS) $(INCLUDES) $(HOST_SSL) -o $@ $(SOURCES) $(HOST_LIB)

test:
	$(MAKE) -C $(REPO)/test/onboard-computer check

image:
	docker build --platform $(BOARD_PLATFORM) -t $(BUILD_IMAGE) docker

build/board/fsir-updater: $(SOURCES) $(HEADERS)
	docker image inspect $(BUILD_IMAGE) > /dev/null 2>&1 || $(MAKE) image
	mkdir -p build/board
	$(DOCKER_RUN) g++ $(FLAGS) -mcpu=$(BOARD_CPU) $(INCLUDES) -static -s -o $@ $(SOURCES) -lcrypto
	file $@ | grep -q "$(BOARD_FILE_TYPE)" || { echo "not a $(BOARD_FILE_TYPE) program: $@ deleted" >&2; rm -f $@; exit 1; }
	file $@
	ls -l $@

board: build/board/fsir-updater

clean:
	rm -rf build

.PHONY: all test image board clean
