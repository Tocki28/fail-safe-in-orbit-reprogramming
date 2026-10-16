# Makefile — boundary and eviction stress cases for the onboard computer's updater
# (stress audit Parts A.4 and A.6). Host only; nothing here is part of the operating software
# (REQ-101). Builds into build/ in this directory and nowhere else.
#
#   make run        build, run every case, write the log under
#                   test/onboard-computer/results/<date>/stress-host/boundaries/
#   make wrap-full  the 2^32-frame wrap of data link A's counter (slow; not part of run)
#   make clean
#
# The updater sources are compiled unchanged. Two substitutions are made at build time, both
# shown in the log:
#   - fake_clock.cpp stands in for src/onboard-computer/updater/clock.cpp, so that the system
#     clock is a value the test sets;
#   - for the wrap cases only, boundaries-wrap uses a copy of data_link.cpp in build/wrap/ whose
#     one changed token is the first sequence number (next_sequence_(1) becomes
#     next_sequence_(STRESS_LINK_START)); the diff is printed into the log.

REPO     := $(abspath ../../../..)
UPD       = $(REPO)/src/onboard-computer/updater
CTRL      = $(REPO)/src/controller
OPENSSL  ?= $(shell brew --prefix openssl@3 2>/dev/null)
DATE     := $(shell date +%F)
OUT      ?= $(REPO)/test/onboard-computer/results/$(DATE)/stress-host/boundaries

WARNINGS = -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wformat=2 \
           -Wswitch-enum -Werror
SANITIZE = -fsanitize=undefined -fno-sanitize-recover=undefined
CXXFLAGS = -std=c++17 -O1 -g -fno-exceptions -fno-rtti $(WARNINGS) $(SANITIZE) \
           -I$(UPD) -I$(CTRL)/common -I. -isystem $(OPENSSL)/include

# Every updater file except main.cpp, and except the two that are substituted.
UPD_SOURCES = $(filter-out $(UPD)/main.cpp $(UPD)/clock.cpp,$(wildcard $(UPD)/*.cpp))
UPD_HEADERS = $(wildcard $(UPD)/*.hpp)
UPD_OBJS    = $(patsubst $(UPD)/%.cpp,build/obj/%.o,$(UPD_SOURCES))
SHARED_OBJS = build/obj/fake_clock.o build/obj/common.o
COMMON_OBJS = $(SHARED_OBJS) build/obj/boundaries.o
WRAP_START  = 0xFFFFFFFCU
LDLIBS      = -L$(OPENSSL)/lib -lcrypto

run: build/boundaries build/boundaries-wrap
	@test -n "$(OPENSSL)" || (echo "OpenSSL 3 not found: brew install openssl@3"; false)
	mkdir -p $(OUT) build/scratch build/logs
	( echo "$$(date '+%F %T %Z') | $$(git -C $(REPO) rev-parse --short HEAD) |" \
	       "make -C test/onboard-computer/stress/boundaries run"; \
	  echo "load: $$(uptime | sed 's/.*load averages*: //')"; \
	  echo "--- wrap build: the one changed line of data_link.cpp ---"; \
	  diff $(UPD)/data_link.cpp build/wrap/data_link.cpp || true; \
	  echo "--- cases ---"; \
	  build/boundaries build/scratch build/logs $(OUT); \
	  echo "--- wrap build (data link A counter starts at $(WRAP_START)) ---"; \
	  build/boundaries-wrap build/scratch build/logs $(OUT); \
	  echo "load at end: $$(uptime | sed 's/.*load averages*: //')" \
	) > $(OUT)/boundaries.log 2>&1
	@( echo "--- summary ---"; \
	   for v in OK FINDING NOTE DIED SETUP; do \
	     echo "$$v lines: $$(grep -c "^$$v " $(OUT)/boundaries.log)"; done ) | \
	     tee -a $(OUT)/boundaries.log
	@echo "log: $(OUT)/boundaries.log"
	@# Exit non-zero when the log holds a FINDING, DIED or SETUP line, after the log is complete.
	@# The summary lines ("FINDING lines: n") are not findings and are left out of the count.
	@n=$$(awk '/^(FINDING|DIED|SETUP) / && !/^[A-Z]+ lines: /' $(OUT)/boundaries.log \
	  | wc -l | tr -d " "); \
	  if [ "$$n" -gt 0 ]; then \
	    echo "boundaries: $$n FINDING, DIED or SETUP lines; exit status 1 because a run that" \
	      "reports findings must not look like a pass"; exit 1; fi

build/obj/%.o: $(UPD)/%.cpp $(UPD_HEADERS)
	@mkdir -p build/obj
	clang++ $(CXXFLAGS) -c $< -o $@

build/obj/fake_clock.o: fake_clock.cpp stress_clock.hpp
	@mkdir -p build/obj
	clang++ $(CXXFLAGS) -c $< -o $@

build/obj/common.o: common.cpp common.hpp rig.hpp stress_clock.hpp $(UPD_HEADERS)
	@mkdir -p build/obj
	clang++ $(CXXFLAGS) -c $< -o $@

build/obj/boundaries.o: boundaries.cpp cases_a4.hpp cases_a6.hpp common.hpp rig.hpp \
                        stress_clock.hpp $(UPD_HEADERS)
	@mkdir -p build/obj
	clang++ $(CXXFLAGS) -c $< -o $@

build/boundaries: $(UPD_OBJS) $(COMMON_OBJS)
	clang++ $(SANITIZE) -o $@ $^ $(LDLIBS)

# The wrap variant: data_link.cpp with its first sequence number set at build time.
build/wrap/data_link.cpp: $(UPD)/data_link.cpp
	@mkdir -p build/wrap
	sed 's/next_sequence_(1)/next_sequence_(STRESS_LINK_START)/' $< > $@
	@test "$$(diff $< $@ | grep -c '^>')" = "1" || \
	  (echo "wrap patch changed other than one line"; false)

build/obj/data_link_wrap.o: build/wrap/data_link.cpp $(UPD_HEADERS)
	@mkdir -p build/obj
	clang++ $(CXXFLAGS) -DSTRESS_LINK_START=$(WRAP_START) -c $< -o $@

# boundaries.cpp again, with STRESS_LINK_START defined: this build runs the wrap cases only.
build/obj/boundaries_wrap.o: boundaries.cpp cases_a4.hpp cases_a6.hpp common.hpp rig.hpp \
                             stress_clock.hpp $(UPD_HEADERS)
	@mkdir -p build/obj
	clang++ $(CXXFLAGS) -DSTRESS_LINK_START=$(WRAP_START) -c $< -o $@

build/boundaries-wrap: $(filter-out build/obj/data_link.o,$(UPD_OBJS)) build/obj/data_link_wrap.o \
                       $(SHARED_OBJS) build/obj/boundaries_wrap.o
	clang++ $(SANITIZE) -o $@ $^ $(LDLIBS)

# The full 2^32 wrap of the counter in DataLink itself (no updater around it).
build/wrap_full: wrap_full.cpp $(UPD)/data_link.cpp $(UPD)/frame.cpp $(UPD)/crc32.cpp \
                 $(UPD)/log.cpp $(UPD)/clock.cpp $(UPD_HEADERS)
	clang++ -std=c++17 -O2 -g -fno-exceptions -fno-rtti $(WARNINGS) $(SANITIZE) \
	  -I$(UPD) -I$(CTRL)/common -o $@ \
	  wrap_full.cpp $(UPD)/data_link.cpp $(UPD)/frame.cpp $(UPD)/crc32.cpp \
	  $(UPD)/log.cpp $(UPD)/clock.cpp

wrap-full: build/wrap_full
	mkdir -p $(OUT)
	( echo "$$(date '+%F %T %Z') | $$(git -C $(REPO) rev-parse --short HEAD) |" \
	       "make -C test/onboard-computer/stress/boundaries wrap-full"; \
	  echo "load: $$(uptime | sed 's/.*load averages*: //')"; \
	  build/wrap_full; \
	  echo "load at end: $$(uptime | sed 's/.*load averages*: //')" ) > $(OUT)/wrap-full.log 2>&1
	@cat $(OUT)/wrap-full.log

clean:
	rm -rf build

.PHONY: run wrap-full clean
