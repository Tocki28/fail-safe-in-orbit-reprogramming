# Makefile — host tests of the onboard computer's updater (src/onboard-computer/updater).
# Bench only; nothing here is part of the operating software (REQ-101).
#
#   make check   unit tests, then the bench test (fake controller on a pseudo-terminal, fake
#                ground on UDP, the real fsir-updater between them)
#   make unit    unit tests only (unit/test_updater.cpp)
#   make bench   bench test only (bench_test.py)
#   make unit-arm  the unit tests on 64-bit ARM Linux, in Docker (not part of check: slow)
#   make clean
#
# Needs the signed images, built beforehand (the ARM toolchain and the development keys are
# not needed here):
#   make -C src/controller      firmware-A.img, firmware-B.img
#   make -C test/controller     the test images (good, flipped, signed with a wrong key)

REPO        := $(abspath ../..)
UPD          = $(REPO)/src/onboard-computer/updater
CTRL         = $(REPO)/src/controller
IMAGES       = $(CTRL)/build/images
TEST_IMAGES  = $(REPO)/test/controller/build/images
TOOLS        = $(REPO)/tools
PY          ?= python3
OPENSSL     ?= $(shell brew --prefix openssl@3 2>/dev/null)

NEEDED = $(IMAGES)/firmware-A.img $(IMAGES)/firmware-B.img \
         $(TEST_IMAGES)/slotA-v1.img $(TEST_IMAGES)/slotB-v2.img $(TEST_IMAGES)/slotB-v2-hang.img \
         $(TEST_IMAGES)/last-resort-v1.img $(TEST_IMAGES)/slotA-v1-flipped.img \
         $(TEST_IMAGES)/slotA-v1-wrong-key-a.img $(TEST_IMAGES)/slotB-v2-wrong-key-b.img \
         $(TEST_IMAGES)/firmware-B-wrong-key-a.img

# The reference the frame codec is compared with: the controller's frame layer and CRC-32 in C,
# taken from commit 5c0b5f8 (step 2c; frame.c and crc32.c unchanged since step 2a, the version
# test/controller/link_probe.py ran against the board). Pinned to a commit, so the test does not
# follow the controller's port to C++ while it is under way; move REF_COMMIT when the
# controller's frame code changes.
REF_COMMIT = 5c0b5f8
REF        = build/ref
REF_FILES  = $(REF)/frame.c $(REF)/frame.h $(REF)/crc32.c $(REF)/crc32.h

# Every updater file except main.cpp.
UPD_SOURCES = $(filter-out $(UPD)/main.cpp,$(wildcard $(UPD)/*.cpp))
UPD_HEADERS = $(wildcard $(UPD)/*.hpp)
WARNINGS = -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wformat=2 -Werror
# The undefined-behaviour sanitizer stops the run on any index out of bounds or overflow (the
# address sanitizer does not run on this Mac; see test/controller/Makefile).
SANITIZE = -fsanitize=undefined -fno-sanitize-recover=undefined
FLIPPED  = build/firmware-B-flipped.img

check: unit bench

images-present:
	@for f in $(NEEDED); do test -f $$f || { echo "missing $$f: run make -C src/controller and make -C test/controller first"; exit 1; }; done

# A damaged image the way the bench tests make one: byte 0x600, inside the program's code.
$(FLIPPED): $(IMAGES)/firmware-B.img
	mkdir -p build
	$(PY) $(TOOLS)/flip_byte.py $< 0x600 $@

$(REF)/frame.c $(REF)/frame.h:
	mkdir -p $(REF)
	git -C $(REPO) show $(REF_COMMIT):src/controller/firmware/data-link/$(notdir $@) > $@
$(REF)/crc32.c $(REF)/crc32.h:
	mkdir -p $(REF)
	git -C $(REPO) show $(REF_COMMIT):src/controller/common/$(notdir $@) > $@

build/frame.o: $(REF_FILES)
	cc -std=c11 -O1 -g -Wall -Wextra -Werror $(SANITIZE) -c $(REF)/frame.c -o $@

build/crc32.o: $(REF_FILES)
	cc -std=c11 -O1 -g -Wall -Wextra -Werror $(SANITIZE) -c $(REF)/crc32.c -o $@

build/test_updater: unit/test_updater.cpp $(UPD_SOURCES) $(UPD_HEADERS) build/frame.o build/crc32.o
	@test -n "$(OPENSSL)" || (echo "OpenSSL 3 not found: brew install openssl@3"; false)
	clang++ -std=c++17 -O1 -g -fno-exceptions -fno-rtti $(WARNINGS) $(SANITIZE) \
	  -I$(UPD) -I$(REF) -I$(CTRL)/common -isystem $(OPENSSL)/include \
	  -o $@ unit/test_updater.cpp $(UPD_SOURCES) build/frame.o build/crc32.o \
	  -L$(OPENSSL)/lib -lcrypto

# The updater's own log lines go to build/unit.log; shown if a test fails.
unit: images-present build/test_updater $(FLIPPED)
	mkdir -p build/scratch
	build/test_updater $(IMAGES) $(TEST_IMAGES) $(FLIPPED) build/scratch 2> build/unit.log \
	  || { echo "--- updater log (build/unit.log) ---"; cat build/unit.log; false; }

# The same unit tests on 64-bit ARM Linux, under QEMU in the board's build container
# (src/onboard-computer/updater/docker), with g++ and that container's libcrypto: the board's
# processor and compiler, the Mac's images. Slow (emulated): a few minutes.
ARM_IMAGE = fsir-arm64-build
ARM_RUN   = docker run --rm --platform linux/arm64 -v "$(REPO)":/src -w /src/test/onboard-computer $(ARM_IMAGE)
ARM_UPD   = ../../src/onboard-computer/updater
ARM_CTRL  = ../../src/controller
unit-arm: images-present $(FLIPPED) $(REF_FILES)
	docker image inspect $(ARM_IMAGE) > /dev/null 2>&1 || $(MAKE) -C $(UPD) image
	mkdir -p build/arm build/scratch-arm
	$(ARM_RUN) sh -c 'gcc -std=c11 -O1 -Wall -Wextra -Werror -c $(REF)/frame.c -o build/arm/frame.o && \
	  gcc -std=c11 -O1 -Wall -Wextra -Werror -c $(REF)/crc32.c -o build/arm/crc32.o && \
	  g++ -std=c++17 -O1 -fno-exceptions -fno-rtti $(WARNINGS) -mcpu=cortex-a35 \
	    -I$(ARM_UPD) -I$(REF) -I$(ARM_CTRL)/common -o build/arm/test_updater \
	    unit/test_updater.cpp $(patsubst $(UPD)/%,$(ARM_UPD)/%,$(UPD_SOURCES)) build/arm/frame.o build/arm/crc32.o -lcrypto && \
	  build/arm/test_updater $(patsubst $(REPO)/%,../../%,$(IMAGES)) $(patsubst $(REPO)/%,../../%,$(TEST_IMAGES)) $(FLIPPED) build/scratch-arm 2> build/arm/unit.log'

bench: images-present $(FLIPPED)
	$(MAKE) -C $(UPD)
	$(PY) bench_test.py $(UPD)/build/host/fsir-updater $(IMAGES)/firmware-B.img $(FLIPPED) \
	  $(TEST_IMAGES)/firmware-B-wrong-key-a.img build/bench

clean:
	rm -rf build

.PHONY: check images-present unit unit-arm bench clean
