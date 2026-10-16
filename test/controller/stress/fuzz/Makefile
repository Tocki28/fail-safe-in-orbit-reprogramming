# Makefile — host fuzzing of every parser of the controller (stress audit, Part A.1).
#
#   make run            build all targets and run them (seeded, reproducible); logs go to
#                       test/controller/results/<date>/stress-host/fuzz/
#   make run N=100000   inputs per target (default 1000000; for fz_command, command payloads);
#                       SEED=7 changes the seed
#   make selfcheck      prove that the fuzzer catches defects: builds each target against a
#                       deliberately broken copy of the code under test (selfcheck.sh)
#   make all            build all targets into build/
#   make clean          remove build/
#
# The fuzzer is mutation-based and has no coverage feedback: Apple clang 17 on this Mac has no
# libFuzzer runtime, and AFL and honggfuzz are not installed. Every input is a pure function of
# (seed, index): `build/<target> --seed S --one INDEX --log DIR --dir ...` rebuilds one input.
#
# Flags are those of test/controller/Makefile (CODE_RULES D1, E5); the address sanitizer does
# not run on this Mac. Nothing here edits src/ or the unit tests; the unit-test fakes are
# compiled from their place. B is the build directory; a self-check builds into its own B and
# points one *_SRC variable at a broken copy.

REPO   := $(abspath ../../../..)
CTRL   := $(REPO)/src/controller
FW     := $(CTRL)/firmware
COMMON := $(CTRL)/common
BL     := $(CTRL)/bootloader
UNIT   := $(REPO)/test/controller/unit
OBC    := $(REPO)/src/onboard-computer
MONO   := $(REPO)/third_party/monocypher

N      ?= 1000000
SEED   ?= 20261016
DATE   := $(shell date +%F)
LOG    := $(REPO)/test/controller/results/$(DATE)/stress-host/fuzz
B      ?= build
SNAP   := build/images

# The code under test, one variable per file the self-check breaks.
FW_FRAME_SRC       ?= $(FW)/data-link/frame.cpp
ARM_SRC            ?= $(FW)/command-checker/arm_state.cpp
UPDATE_SRC         ?= $(FW)/update-receiver/update_receiver.cpp
MODE_SRC           ?= $(FW)/mode-manager/mode_manager.cpp
TESTIMG_SRC        ?= $(FW)/mode-manager/test_image.cpp
IMAGE_CHECK_SRC    ?= $(COMMON)/image_check.c
BOOT_STATE_SRC     ?= $(COMMON)/boot_state.c
BL_IMAGE_CHECK_SRC ?= $(BL)/bl_image_check.c
BL_BOOT_STATE_SRC  ?= $(BL)/bl_boot_state.c
BL_BACKUP_SRC      ?= $(BL)/backup.c
BL_MAIN_DIR        ?= $(BL)

EXTRA ?=
HOST_CC   = cc -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow \
            -Wswitch-enum -Werror -fsanitize=undefined -fno-sanitize-recover=undefined $(EXTRA)
HOST_CXX  = clang++ -std=c++17 -O1 -g -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic \
            -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast -Wcast-align \
            -Wnull-dereference -Wformat=2 -Wswitch-enum -Werror -fsanitize=undefined \
            -fno-sanitize-recover=undefined \
            -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG $(EXTRA)

TARGETS  := fz_frame fz_command fz_image fz_bootstate fz_backup
CXX_INC  := -Isrc -I$(FW) -I$(FW)/data-link -I$(OBC) -I$(COMMON)
CMD_INC  := -Isrc -I$(B) -I$(UNIT) -I$(FW) -I$(FW)/data-link -I$(FW)/update-receiver \
            -I$(FW)/health-monitor -I$(FW)/mode-manager -I$(FW)/command-checker -I$(OBC) \
            -I$(COMMON) -I$(BL)
C_INC    := -Isrc -I$(BL) -I$(COMMON) -I$(MONO)
MONO_OBJS := $(B)/monocypher.o $(B)/monocypher-ed25519.o
BLOBJS   := $(B)/bl_crc32.o $(B)/bl_image_check.o

all: $(addprefix $(B)/,$(TARGETS))

$(B):
	mkdir -p $(B)

# ---- Shared objects ----------------------------------------------------------------------

$(B)/fz_common.o: src/fz_common.c src/fz_common.h | $(B)
	$(HOST_CC) -c $< -o $@

$(B)/fz_fault_c.o: src/fz_fault_c.c src/fz_fault.h | $(B)
	$(HOST_CC) -Isrc -I$(COMMON) -c $< -o $@

$(B)/fz_fault_cxx.o: src/fz_fault.cpp src/fz_fault.h | $(B)
	$(HOST_CXX) $(CXX_INC) -c $< -o $@

$(B)/fz_ramflash.o: src/fz_ramflash.c src/fz_ramflash.h | $(B)
	$(HOST_CC) $(C_INC) -c $< -o $@

$(B)/crc32.o: $(COMMON)/crc32.c | $(B)
	$(HOST_CC) -I$(COMMON) -c $< -o $@

$(B)/image_check.o: $(IMAGE_CHECK_SRC) | $(B)
	$(HOST_CC) -I$(COMMON) -I$(MONO) -c $< -o $@

$(B)/boot_state.o: $(BOOT_STATE_SRC) | $(B)
	$(HOST_CC) -I$(COMMON) -c $< -o $@

$(B)/bl_crc32.o: $(BL)/bl_crc32.c | $(B)
	$(HOST_CC) -I$(BL) -I$(COMMON) -I$(MONO) -c $< -o $@

$(B)/bl_image_check.o: $(BL_IMAGE_CHECK_SRC) | $(B)
	$(HOST_CC) -I$(BL) -I$(COMMON) -I$(MONO) -c $< -o $@

$(B)/bl_boot_state.o: $(BL_BOOT_STATE_SRC) | $(B)
	$(HOST_CC) -I$(BL) -I$(COMMON) -c $< -o $@

$(B)/monocypher.o: $(MONO)/monocypher.c | $(B)
	cc -std=c11 -O2 -c $< -o $@

$(B)/monocypher-ed25519.o: $(MONO)/monocypher-ed25519.c | $(B)
	cc -std=c11 -O2 -c $< -o $@

# ---- Target 1: frame parser --------------------------------------------------------------

$(B)/fw_frame.o: $(FW_FRAME_SRC) | $(B)
	$(HOST_CXX) $(CXX_INC) -c $< -o $@

$(B)/obc_frame.o: $(OBC)/updater/frame.cpp | $(B)
	$(HOST_CXX) -I$(OBC)/updater -I$(COMMON) -c $< -o $@

$(B)/obc_crc32.o: $(OBC)/updater/crc32.cpp | $(B)
	$(HOST_CXX) -I$(OBC)/updater -I$(COMMON) -c $< -o $@

$(B)/fz_frame.o: src/fz_frame.cpp src/fz_common.h src/fz_fault.h | $(B)
	$(HOST_CXX) $(CXX_INC) -c $< -o $@

$(B)/fz_frame: $(B)/fz_frame.o $(B)/fz_common.o $(B)/crc32.o $(B)/fz_fault_cxx.o \
               $(B)/fw_frame.o $(B)/obc_frame.o $(B)/obc_crc32.o
	$(HOST_CXX) $^ -o $@

# ---- Target 2: command payloads ----------------------------------------------------------

# main.cpp's own routing functions, extracted so the target tests the flight text. Fails if one
# is missing (fail closed).
ROUTE_FUNCTIONS := handleUpload handleTestImage routeUpload handleArm handleDangerous \
                   handleLeaveFallback isGetStatus handle

$(B)/fz_route.inc: $(FW)/main.cpp | $(B)
	awk -v names="$(ROUTE_FUNCTIONS)" 'BEGIN { n = split(names, a, " ") } \
	  { if (!inside) { for (i = 1; i <= n; i++) \
	      if ($$0 ~ ("^(void|bool) " a[i] "\\(")) { inside = 1; found[a[i]] = 1 } } \
	    if (inside) print; if (inside && $$0 == "}") { inside = 0; print "" } } \
	  END { for (i = 1; i <= n; i++) \
	      if (!found[a[i]]) { print "missing " a[i] > "/dev/stderr"; exit 1 } }' \
	  $< > $@.tmp && mv $@.tmp $@

FAKE_SRCS := $(UNIT)/fake_port.cpp $(UNIT)/fake_mode_port.cpp $(UNIT)/fake_uart_link.cpp
FW_SRCS   := $(FW)/data-link/frame_sender.cpp $(FW)/health-monitor/health_monitor.cpp

$(B)/fz_command.o: src/fz_command.cpp $(B)/fz_route.inc src/fz_common.h src/fz_fault.h | $(B)
	$(HOST_CXX) $(CMD_INC) -c $< -o $@

$(B)/fz_command: $(B)/fz_command.o $(B)/fz_common.o $(B)/fz_fault_cxx.o $(B)/crc32.o \
                 $(B)/image_check.o $(MONO_OBJS) $(BLOBJS) $(B)/obc_crc32.o \
                 $(FW_FRAME_SRC) $(ARM_SRC) $(UPDATE_SRC) $(MODE_SRC) $(TESTIMG_SRC) \
                 $(FW_SRCS) $(FAKE_SRCS)
	$(HOST_CXX) $(CMD_INC) $^ -o $@

# ---- Target 3: image checks --------------------------------------------------------------

$(B)/fz_image.o: src/fz_image.c src/fz_common.h src/fz_fault.h | $(B)
	$(HOST_CC) $(C_INC) -c $< -o $@

$(B)/fz_image: $(B)/fz_image.o $(B)/fz_common.o $(B)/fz_fault_c.o $(B)/crc32.o \
               $(B)/image_check.o $(BLOBJS) $(MONO_OBJS)
	$(HOST_CC) $^ -lz -o $@

# ---- Target 4: boot state ----------------------------------------------------------------

$(B)/fz_bootstate.o: src/fz_bootstate.c src/fz_common.h src/fz_fault.h src/fz_ramflash.h | $(B)
	$(HOST_CC) $(C_INC) -c $< -o $@

$(B)/fz_bootstate: $(B)/fz_bootstate.o $(B)/fz_common.o $(B)/fz_fault_c.o $(B)/fz_ramflash.o \
                   $(B)/boot_state.o $(B)/bl_boot_state.o $(B)/bl_crc32.o $(B)/crc32.o
	$(HOST_CC) $^ -lz -o $@

# ---- Target 5: backup registers ----------------------------------------------------------

$(B)/backup.o: $(BL_BACKUP_SRC) | $(B)
	$(HOST_CC) -Istubs -I$(BL) -I$(COMMON) -c $< -o $@

$(B)/fz_backup.o: src/fz_backup.c src/fz_common.h src/fz_fault.h src/fz_ramflash.h \
                  $(BL_MAIN_DIR)/main.c | $(B)
	$(HOST_CC) -Istubs -Isrc -I$(BL_MAIN_DIR) -I$(BL) -I$(COMMON) -I$(MONO) -c $< -o $@

$(B)/fz_backup: $(B)/fz_backup.o $(B)/backup.o $(B)/fz_common.o $(B)/fz_fault_c.o \
                $(B)/fz_ramflash.o $(B)/bl_boot_state.o $(B)/bl_crc32.o $(B)/bl_image_check.o \
                $(B)/image_check.o $(B)/crc32.o $(MONO_OBJS)
	$(HOST_CC) $^ -lz -o $@

# ---- Run ---------------------------------------------------------------------------------

IMG_DIRS := --dir $(SNAP)/src --dir $(SNAP)/test

# The exact command is in the first line of every log (task rule); the target's own command line,
# with its seed and count, is on the second line.
HEADER = Mac time (EEST) $$(date '+%F %T %Z'); \
         commit $$(git -C $(REPO) rev-parse --short HEAD); \
         command: make -C test/controller/stress/fuzz run N=$(N) SEED=$(SEED)

# Snapshot of the signed images, so a rebuild by another agent cannot change the seeds mid-run.
$(SNAP)/.stamp:
	rm -rf $(SNAP)
	mkdir -p $(SNAP)/src $(SNAP)/test
	cp $(CTRL)/build/images/*.img $(SNAP)/src/
	cp $(REPO)/test/controller/build/images/*.img $(SNAP)/test/
	touch $@

run-%: build/% $(SNAP)/.stamp
	mkdir -p $(LOG)
	rm -f $(LOG)/$*-fail-* $(LOG)/$*-crash-* $(LOG)/$*-hang-*
	FZ_HEADER="$(HEADER)" build/$* --seed $(SEED) --n $(N) --log $(LOG) $(IMG_DIRS)

run: $(addprefix build/,$(TARGETS)) $(SNAP)/.stamp
	mkdir -p $(LOG)
	( echo "$(HEADER)"; echo "uptime: $$(uptime)"; \
	  echo "cc: $$(cc --version | head -1)" ) > $(LOG)/run.log
	@start=$$(date +%s); status=0; \
	for t in $(TARGETS); do $(MAKE) --no-print-directory run-$$t || \
	  { echo "target $$t exited with a failure" >> $(LOG)/run.log; status=1; }; done; \
	echo "wall time of the run: $$(( $$(date +%s) - start )) s; uptime at the end: $$(uptime)" \
	  | tee -a $(LOG)/run.log; \
	exit $$status

SELF_HEADER = Mac time (EEST) $$(date '+%F %T %Z'); \
              commit $$(git -C $(REPO) rev-parse --short HEAD); \
              command: make -C test/controller/stress/fuzz selfcheck

selfcheck: $(SNAP)/.stamp
	mkdir -p $(LOG)
	@echo "$(SELF_HEADER)" > $(LOG)/selfcheck.log; echo "uptime: $$(uptime)" >> $(LOG)/selfcheck.log
	@start=$$(date +%s); sh selfcheck.sh > $(LOG)/selfcheck.body 2>&1; status=$$?; \
	cat $(LOG)/selfcheck.body >> $(LOG)/selfcheck.log; rm -f $(LOG)/selfcheck.body; \
	echo "wall time: $$(( $$(date +%s) - start )) s" >> $(LOG)/selfcheck.log; \
	cat $(LOG)/selfcheck.log; exit $$status

clean:
	rm -rf build

.PHONY: all clean run selfcheck
