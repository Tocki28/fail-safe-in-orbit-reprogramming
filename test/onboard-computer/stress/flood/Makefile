# Makefile — stress audit A.5: floods against the host fsir-updater with a fake controller.
# Bench only; nothing here is part of the operating software (REQ-101).
#
#   make run     build the updater into build/ here, then run every flood scenario
#                (flood_run.py); logs go to test/onboard-computer/results/<date>/stress-host/flood/
#   make build   only the updater
#   make clean   delete build/
#
# The updater is built from src/onboard-computer/updater with the sources and flags of that
# directory's Makefile (host target) plus the undefined-behaviour sanitizer (CODE_RULES E5), into
# build/ here, so the coordinator's own build directories are not touched.

REPO     := $(abspath ../../../..)
UPD      := $(REPO)/src/onboard-computer/updater
COMMON   := $(REPO)/src/controller/common
IMAGE    := $(REPO)/src/controller/build/images/firmware-B.img
DATE     := $(shell date +%F)
RESULTS  := $(REPO)/test/onboard-computer/results/$(DATE)/stress-host/flood
OPENSSL  ?= $(shell brew --prefix openssl@3 2>/dev/null)
PY       ?= python3

WARNINGS = -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow \
           -Wold-style-cast -Wcast-align -Wnull-dereference -Wformat=2 -Wswitch-enum -Werror
SANITIZE = -fsanitize=undefined -fno-sanitize-recover=undefined
FLAGS    = -std=c++17 -O2 -fno-exceptions -fno-rtti $(WARNINGS) $(SANITIZE)
SOURCES  = $(wildcard $(UPD)/*.cpp)
HEADERS  = $(wildcard $(UPD)/*.hpp) $(COMMON)/image_format.h $(COMMON)/layout.h $(COMMON)/keys.h

build/fsir-updater: $(SOURCES) $(HEADERS)
	@test -n "$(OPENSSL)" || (echo "OpenSSL 3 not found: brew install openssl@3"; false)
	mkdir -p build
	cd $(UPD) && clang++ $(FLAGS) -I. -I$(COMMON) -isystem $(OPENSSL)/include \
	  -o $(CURDIR)/build/fsir-updater $(notdir $(SOURCES)) -L$(OPENSSL)/lib -lcrypto

build: build/fsir-updater

run: build/fsir-updater
	mkdir -p $(RESULTS)
	$(PY) flood_run.py --updater $(CURDIR)/build/fsir-updater --image $(IMAGE) --out $(RESULTS) \
	  --work $(CURDIR)/build/work

clean:
	rm -rf build

.PHONY: build run clean
