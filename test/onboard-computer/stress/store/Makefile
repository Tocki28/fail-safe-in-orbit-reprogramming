# Makefile — store stress test (Parts A.2, A.3, A.4 report numbers) of the onboard computer.
# Bench only; nothing here is part of the operating software (REQ-101).
#
#   make run     build into build/ and run everything; logs go to
#                ../../results/<date>/stress-host/store/
#   make clean
#
# Builds only into build/ of this directory (A10b: nothing in src/ is changed). The updater
# objects the parts need are compiled here with the unit tests' warnings and the undefined
# behaviour sanitizer (D1, E5). The shim defines open, write, fsync, rename ... itself, so the
# calls made by the updater objects reach it (shim.hpp).

REPO     := $(abspath ../../../..)
UPD       = $(REPO)/src/onboard-computer/updater
CTRL      = $(REPO)/src/controller
OPENSSL  ?= $(shell brew --prefix openssl@3 2>/dev/null)
DATE     := $(shell date +%F)
LOGDIR   ?= $(abspath ../../results/$(DATE)/stress-host/store)
COMMIT   := $(shell git -C $(REPO) rev-parse --short HEAD)
WARNINGS  = -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wformat=2 \
            -Wswitch-enum -Werror
SANITIZE  = -fsanitize=undefined -fno-sanitize-recover=undefined
CXXFLAGS  = -std=c++17 -O1 -g -fno-rtti $(WARNINGS) $(SANITIZE) -I$(UPD) -I$(CTRL)/common \
            -isystem $(OPENSSL)/include

# The updater files under test and what they include (image_checker.cpp: limits_record.cpp needs
# defaultImageLimits()).
UPD_FILES = report_store durable_record clock_record limits_record log clock crc32 image_checker
OWN_FILES = shim util a2 a3 a4 main
OBJS      = $(addprefix build/upd_,$(addsuffix .o,$(UPD_FILES))) \
            $(addprefix build/own_,$(addsuffix .o,$(OWN_FILES)))
HEADERS   = $(wildcard *.hpp) $(wildcard $(UPD)/*.hpp)

build/upd_%.o: $(UPD)/%.cpp $(HEADERS)
	@mkdir -p build
	clang++ $(CXXFLAGS) -c $< -o $@

build/own_%.o: %.cpp $(HEADERS)
	@mkdir -p build
	clang++ $(CXXFLAGS) -c $< -o $@

build/store_stress: $(OBJS)
	clang++ $(SANITIZE) -o $@ $(OBJS) -L$(OPENSSL)/lib -lcrypto

run: build/store_stress
	mkdir -p $(LOGDIR)
	build/store_stress build/work $(LOGDIR) $(COMMIT) "make -C test/onboard-computer/stress/store run"

clean:
	rm -rf build

.PHONY: run clean
