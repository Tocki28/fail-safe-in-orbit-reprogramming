# Makefile — host stress tests of the controller, Part A.3 (boot state through a power cut at
# every write step) and the controller half of Part A.4 (wrap and boundary cases).
#
#   make run     build everything into build/, run the three programs, write the logs to
#                test/controller/results/<date>/stress-host/powercut/
#   make build   build only
#   make clean   remove build/
#
# Programs (each in its own file, each links the real source files named below):
#   powercut_bootstate  common/boot_state.c and bootloader/bl_boot_state.c against a fake flash
#   boundaries_mode     firmware/command-checker/arm_state.cpp and firmware/mode-manager against
#                       the unit tests' fakes (read-only use of test/controller/unit)
#   clock_wrap          firmware/core_clock.cpp against the unit tests' register stand-ins
#
# Flags are the board's strict set (code rules D1, E5): the same as test/controller/Makefile,
# copied here because that Makefile is not to be changed or run by this audit. Nothing in this
# directory is part of the operating software (REQ-101).

SELF   := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
REPO   := $(abspath $(SELF)/../../../..)
CTRL   := $(REPO)/src/controller
COMMON := $(CTRL)/common
BL     := $(CTRL)/bootloader
FW     := $(CTRL)/firmware
UNIT   := $(REPO)/test/controller/unit
BUILD  := $(SELF)/build

HOST_CC  = cc -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow \
           -Wswitch-enum -Werror -fsanitize=undefined -fno-sanitize-recover=undefined
HOST_CXX = clang++ -std=c++17 -O1 -g -fno-exceptions -fno-rtti \
           -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast \
           -Wcast-align -Wnull-dereference -Wformat=2 -Wswitch-enum -Werror \
           -fsanitize=undefined -fno-sanitize-recover=undefined \
           -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG

DATE   := $(shell date +%F)
LOGDIR := $(REPO)/test/controller/results/$(DATE)/stress-host/powercut
COMMIT := $(shell git -C $(REPO) rev-parse --short HEAD)

FW_INC = -I$(UNIT) -I$(FW) -I$(FW)/data-link -I$(FW)/update-receiver -I$(FW)/health-monitor \
         -I$(FW)/mode-manager -I$(FW)/command-checker -I$(COMMON)

# Sources of the mode manager with everything it is wired to (as test/controller/Makefile,
# MODE_SRCS), minus the unit test itself.
MODE_DEPS = $(UNIT)/fake_port.cpp $(UNIT)/fake_mode_port.cpp $(UNIT)/fake_fault.cpp \
            $(UNIT)/fake_uart_link.cpp $(FW)/mode-manager/mode_manager.cpp \
            $(FW)/mode-manager/test_image.cpp $(FW)/command-checker/arm_state.cpp \
            $(FW)/data-link/frame_sender.cpp $(FW)/data-link/frame.cpp \
            $(FW)/health-monitor/health_monitor.cpp $(FW)/update-receiver/update_receiver.cpp
MODE_HDRS = $(wildcard $(FW)/*.hpp $(FW)/*/*.hpp $(COMMON)/*.h $(UNIT)/*.hpp)

all: build

build: $(BUILD)/powercut_bootstate $(BUILD)/boundaries_mode $(BUILD)/clock_wrap

$(BUILD)/powercut_bootstate: powercut_bootstate.c $(COMMON)/boot_state.c $(COMMON)/crc32.c \
                             $(BL)/bl_boot_state.c $(BL)/bl_crc32.c \
                             $(wildcard $(COMMON)/*.h $(BL)/*.h)
	mkdir -p $(BUILD)
	$(HOST_CC) -I$(COMMON) -I$(BL) powercut_bootstate.c $(COMMON)/boot_state.c \
	    $(COMMON)/crc32.c $(BL)/bl_boot_state.c $(BL)/bl_crc32.c -o $@

$(BUILD)/crc32.o: $(COMMON)/crc32.c $(wildcard $(COMMON)/*.h)
	mkdir -p $(BUILD)
	$(HOST_CC) -I$(COMMON) -c $< -o $@

$(BUILD)/image_check.o: $(COMMON)/image_check.c $(wildcard $(COMMON)/*.h)
	mkdir -p $(BUILD)
	$(HOST_CC) -I$(COMMON) -I$(REPO)/third_party/monocypher -c $< -o $@

# Monocypher is compiled apart, as shipped: no -Werror, no sanitizer (test/controller/Makefile).
$(BUILD)/monocypher.o $(BUILD)/monocypher-ed25519.o: \
    $(BUILD)/%.o: $(REPO)/third_party/monocypher/%.c
	mkdir -p $(BUILD)
	cc -std=c11 -O2 -c -I$(REPO)/third_party/monocypher $< -o $@

$(BUILD)/boundaries_mode: boundaries_mode.cpp $(MODE_DEPS) $(MODE_HDRS) $(BUILD)/crc32.o \
                          $(BUILD)/image_check.o $(BUILD)/monocypher.o $(BUILD)/monocypher-ed25519.o
	mkdir -p $(BUILD)
	$(HOST_CXX) $(FW_INC) boundaries_mode.cpp $(MODE_DEPS) $(BUILD)/crc32.o \
	    $(BUILD)/image_check.o $(BUILD)/monocypher.o $(BUILD)/monocypher-ed25519.o -o $@

$(BUILD)/clock_wrap: clock_wrap.cpp $(FW)/core_clock.cpp $(UNIT)/fake_fault.cpp $(MODE_HDRS) \
                     $(wildcard $(UNIT)/board_stubs/*.h)
	mkdir -p $(BUILD)
	$(HOST_CXX) -I$(UNIT)/board_stubs $(FW_INC) clock_wrap.cpp $(FW)/core_clock.cpp \
	    $(UNIT)/fake_fault.cpp -o $@

# Each program writes its own log; the first line is the time, the commit and the command.
# The exit status of make is 1 if any program found an invariant broken.
run: build
	mkdir -p $(LOGDIR)
	@rc=0; \
	for p in powercut_bootstate boundaries_mode clock_wrap; do \
	    log=$(LOGDIR)/$$p.log; \
	    echo "$$(date '+%F %T %Z') commit $(COMMIT) command:" \
	        "make -C test/controller/stress/powercut run ($$p)" > $$log; \
	    echo "load: $$(uptime | sed 's/.*load/load/')" >> $$log; \
	    s=$$(date +%s); \
	    $(BUILD)/$$p >> $$log 2>&1 || rc=1; \
	    echo "wall $$(( $$(date +%s) - s )) s" >> $$log; \
	done; \
	grep -h '^FAILCASE' $(LOGDIR)/powercut_bootstate.log > $(LOGDIR)/failing_cases.txt || true; \
	grep -h '^FAILSEQ' $(LOGDIR)/powercut_bootstate.log >> $(LOGDIR)/failing_cases.txt || true; \
	exit $$rc

# Mutation check (code rule E1: prove the test catches a defect). Copies of the two writers are
# changed so that a write goes to the CURRENT copy instead of the older one (rule B7 broken), and
# the powercut program must then fail. The sources in src/ are not touched.
mutants: $(BUILD)/powercut_bootstate
	mkdir -p $(BUILD)/mutant
	sed -e 's|target = (newest == 0U) ? 1U : 0U;|target = (newest == NO_VALID_COPY) ? 0U : newest;|' \
	    $(COMMON)/boot_state.c > $(BUILD)/mutant/boot_state.c
	sed -e 's|target = OTHER_COPY_ADDRESS\[newest\];|'\
	'target = COPY_ADDRESS[newest] + (OTHER_COPY_ADDRESS[0] \& 0U);|' \
	    $(BL)/bl_boot_state.c > $(BUILD)/mutant/bl_boot_state.c
	! cmp -s $(COMMON)/boot_state.c $(BUILD)/mutant/boot_state.c
	! cmp -s $(BL)/bl_boot_state.c $(BUILD)/mutant/bl_boot_state.c
	$(HOST_CC) -I$(COMMON) -I$(BL) powercut_bootstate.c $(BUILD)/mutant/boot_state.c \
	    $(COMMON)/crc32.c $(BL)/bl_boot_state.c $(BL)/bl_crc32.c -o $(BUILD)/mutant/mut_fw
	$(HOST_CC) -I$(COMMON) -I$(BL) powercut_bootstate.c $(COMMON)/boot_state.c \
	    $(COMMON)/crc32.c $(BUILD)/mutant/bl_boot_state.c $(BL)/bl_crc32.c -o $(BUILD)/mutant/mut_bl
	mkdir -p $(LOGDIR)
	@for m in mut_fw mut_bl; do \
	    if $(BUILD)/mutant/$$m > $(BUILD)/mutant/$$m.full 2>&1; then \
	        echo "MUTANT $$m SURVIVED (the test did not notice)"; exit 1; \
	    else \
	        echo "$$(date '+%F %T %Z') commit $(COMMIT) command:" \
	            "make -C test/controller/stress/powercut mutants ($$m)" > $(LOGDIR)/mutant-$$m.log; \
	        grep '^totals\|structure failures\|^FAILSEQ\|^FAIL ' $(BUILD)/mutant/$$m.full | head -12 \
	            >> $(LOGDIR)/mutant-$$m.log; \
	        echo "mutant $$m caught: $$(grep '^totals' $(BUILD)/mutant/$$m.full | tr -s ' ')"; \
	    fi; \
	done

clean:
	rm -rf $(BUILD)

.PHONY: all build run clean mutants
