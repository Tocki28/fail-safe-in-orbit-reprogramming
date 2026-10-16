# Makefile — stress audit Part A.1: fuzz the parsers of the onboard computer's updater.
# Bench only; nothing here is part of the operating software (REQ-101).
#
#   make run            build, then run every target (seeded, reproducible) and write the logs,
#                       then give the verdict over them (exits non-zero on any finding)
#   make run N=100000   the same with 100,000 inputs per target instead of 1,000,000
#   make verdict        only the verdict over the logs already in OUT
#   make run-frame      one target: frame parser records reports updater image imagefile
#   make repro T=frame I=1234     rebuild and rerun the single input with that index (seed SEED)
#   make clean
#
# Needs the signed images, built beforehand: make -C src/controller and make -C test/controller.
# The fuzzer is mutation-based and has no coverage feedback: clang 17 on this Mac has no libFuzzer
# runtime and AFL and honggfuzz are not installed. Everything builds into build/ here, apart from
# the logs, which go to test/onboard-computer/results/<date>/stress-host/fuzz/.

REPO        := $(abspath ../../../..)
UPD          = $(REPO)/src/onboard-computer/updater
CTRL         = $(REPO)/src/controller
IMAGES       = $(CTRL)/build/images
TEST_IMAGES  = $(REPO)/test/controller/build/images
OPENSSL     ?= $(shell brew --prefix openssl@3 2>/dev/null)
N           ?= 1000000
SEED        ?= 20261016
JOBS        ?= 4
DATE        := $(shell date +%F)
OUT         ?= $(REPO)/test/onboard-computer/results/$(DATE)/stress-host/fuzz
TARGETS      = frame parser records reports image imagefile updater
T           ?= frame
I           ?= 0

# The pinned C reference of the frame layer, as test/onboard-computer/Makefile takes it.
REF_COMMIT   = 5c0b5f8
REF          = build/ref

WARNINGS = -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wformat=2 \
           -Wswitch-enum -Werror
# Host tests are built with the undefined-behaviour sanitizer and no recovery (code rules E5);
# the address sanitizer does not run on this Mac.
SANITIZE = -fsanitize=undefined -fno-sanitize-recover=undefined
INCLUDES = -I$(UPD) -I$(REF) -I. -I$(CTRL)/common -isystem $(OPENSSL)/include
CXXFLAGS = -std=c++17 -O2 -g -fno-omit-frame-pointer -fno-exceptions -fno-rtti $(WARNINGS) \
           $(SANITIZE) $(INCLUDES)

UPD_SOURCES = $(filter-out $(UPD)/main.cpp,$(wildcard $(UPD)/*.cpp))
UPD_OBJECTS = $(patsubst $(UPD)/%.cpp,build/obj/upd_%.o,$(UPD_SOURCES))
UPD_HEADERS = $(wildcard $(UPD)/*.hpp)
FUZZ_SOURCES = fuzz_common.cpp fuzz_frames.cpp fuzz_images.cpp fuzz_records.cpp fuzz_updater.cpp \
               fuzz_main.cpp
FUZZ_OBJECTS = $(patsubst %.cpp,build/obj/%.o,$(FUZZ_SOURCES))
FUZZ_HEADERS = $(wildcard *.hpp)
BIN          = build/fuzz

run: images-present
	$(MAKE) -j$(JOBS) $(addprefix run-,$(TARGETS))
	@echo "logs: $(OUT)"
	@$(MAKE) --no-print-directory verdict OUT=$(OUT)

# The verdict over every target's log in OUT, given once all logs are written. run-% exits 0
# whatever its target did, so that under -j one target's failure does not stop the others.
# This step exits 1 when a log shows findings or a non-zero exit status of its target, so that
# a finding cannot hide behind a passing exit status of make run. A log without its SUMMARY
# line or its exit status line counts as failed: missing evidence is not a pass. OUT is passed
# down by run because DATE is computed again in each make and may have changed by the end.
# Each $$(...) stays on one line: GNU make 3.81, the make on this Mac, takes a $$( that runs
# over a backslash-newline for a reference, joins the lines and corrupts the end of the command
# (seen 16 Oct: "every exit" came out as "everytaxit").
verdict:
	@bad=0; why=""; \
	for t in $(TARGETS); do \
	  log=$(OUT)/$$t.log; \
	  f=$$(sed -n 's/^SUMMARY target .* inputs, \([0-9]*\) findings,.*/\1/p' $$log 2>/dev/null); \
	  s=$$(sed -n 's/^exit status \([0-9]*\);.*/\1/p' $$log 2>/dev/null); \
	  if [ "$$f" != 0 ] || [ "$$s" != 0 ]; then bad=$$((bad + 1)); \
	    why="$$why $$t (findings $${f:-none}, exit status $${s:-none});"; fi; \
	done; \
	if [ $$bad -ne 0 ]; then \
	  echo "verdict: FAIL, $$bad of $(words $(TARGETS)) targets have findings or a non-zero" \
	    "exit status:$$why"; exit 1; fi; \
	echo "verdict: PASS, $(words $(TARGETS)) targets, 0 findings, every exit status 0"

images-present:
	@for f in $(IMAGES)/firmware-A.img $(IMAGES)/firmware-B.img $(TEST_IMAGES)/slotA-v1.img; do \
	  test -f $$f || { echo "missing $$f: run make -C src/controller and make -C test/controller" \
	    "first"; exit 1; }; done

$(REF)/frame.c $(REF)/frame.h:
	mkdir -p $(REF)
	git -C $(REPO) show $(REF_COMMIT):src/controller/firmware/data-link/$(notdir $@) > $@

$(REF)/crc32.c $(REF)/crc32.h:
	mkdir -p $(REF)
	git -C $(REPO) show $(REF_COMMIT):src/controller/common/$(notdir $@) > $@

build/ref_frame.o: $(REF)/frame.c $(REF)/frame.h $(REF)/crc32.h
	cc -std=c11 -O2 -g -Wall -Wextra -Werror $(SANITIZE) -I$(REF) -c $(REF)/frame.c -o $@

build/ref_crc32.o: $(REF)/crc32.c $(REF)/crc32.h
	cc -std=c11 -O2 -g -Wall -Wextra -Werror $(SANITIZE) -I$(REF) -c $(REF)/crc32.c -o $@

build/obj/upd_%.o: $(UPD)/%.cpp $(UPD_HEADERS) | build/obj
	@echo CXX $<
	@clang++ $(CXXFLAGS) -c $< -o $@

build/obj/%.o: %.cpp $(FUZZ_HEADERS) $(REF)/frame.h $(UPD_HEADERS) | build/obj
	@echo CXX $<
	@clang++ $(CXXFLAGS) -c $< -o $@

build/obj:
	mkdir -p build/obj

$(BIN): $(UPD_OBJECTS) $(FUZZ_OBJECTS) build/ref_frame.o build/ref_crc32.o
	@test -n "$(OPENSSL)" || (echo "OpenSSL 3 not found: brew install openssl@3"; false)
	clang++ $(SANITIZE) -o $@ $^ -L$(OPENSSL)/lib -lcrypto

# One target: its own work directory, its stderr in an appended file that the fuzzer empties now
# and then (the updater logs a line for most inputs); a failing run keeps the last lines of it.
run-%: $(BIN)
	@mkdir -p $(OUT) build/work/$*
	@rm -f build/work/$*.stderr
	@start=$$(date +%s); \
	FUZZ_START="$$(date '+%F %T')" FUZZ_COMMIT=$$(git -C $(REPO) rev-parse --short HEAD) \
	FUZZ_UPTIME="$$(uptime)" \
	$(BIN) $* --seed $(SEED) --n $(N) --out $(OUT) --work build/work/$* \
	  --ctrl-images $(IMAGES) --test-images $(TEST_IMAGES) 2>>build/work/$*.stderr; \
	status=$$?; end=$$(date +%s); \
	echo "exit status $$status; wall time $$((end - start)) s;" \
	  "uptime at end: $$(uptime)" >> $(OUT)/$*.log; \
	if [ $$status -ne 0 ]; then \
	  tail -n 30 build/work/$*.stderr > $(OUT)/$*.stderr-tail 2>/dev/null; fi; \
	exit 0

repro: $(BIN)
	@mkdir -p build/work/repro-$(T) build/repro
	$(BIN) $(T) --seed $(SEED) --n $(N) --only $(I) --out build/repro --work build/work/repro-$(T) \
	  --ctrl-images $(IMAGES) --test-images $(TEST_IMAGES); echo "exit $$?"; cat build/repro/$(T).log

# Proves the oracles catch deliberate breaks (copies of single updater files; src/ is untouched).
mutants: $(BIN)
	@echo '$(CXXFLAGS)' > build/cxxflags.txt
	UPD=$(UPD) CXXFLAGS_FILE=build/cxxflags.txt OUT=$(OUT) IMAGES=$(IMAGES) \
	TEST_IMAGES=$(TEST_IMAGES) OPENSSL_LIB=$(OPENSSL)/lib sh mutants.sh

clean:
	rm -rf build

.PHONY: mutants run verdict images-present repro clean
