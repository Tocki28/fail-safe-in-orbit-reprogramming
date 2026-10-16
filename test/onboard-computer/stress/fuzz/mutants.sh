#!/bin/sh
# mutants.sh — proves that the fuzz oracles catch real defects (code rules E1).
#
# Each mutant is a copy of ONE updater source file with one deliberate break, compiled into
# build/mutants/<name>/ and linked with the unchanged objects of build/obj. Nothing in src/ is
# changed. The matching fuzz target must report at least one finding on the mutant.
#
# Usage: sh mutants.sh        (needs `make build/fuzz` done first)
# Output: one line per mutant, "CAUGHT" or "MISSED", and the first finding line.

UPD=${UPD:?set UPD to the updater source directory}
CXXFLAGS_FILE=${CXXFLAGS_FILE:?set CXXFLAGS_FILE to a file holding the compiler flags}
OUT=${OUT:?set OUT to the log directory}
N=${N:-20000}
FLAGS=$(cat "$CXXFLAGS_FILE")
IMAGES=${IMAGES:?set IMAGES}
TEST_IMAGES=${TEST_IMAGES:?set TEST_IMAGES}
OPENSSL_LIB=${OPENSSL_LIB:?set OPENSSL_LIB}
FUZZ_SKIP_EXHAUSTIVE=1
export FUZZ_SKIP_EXHAUSTIVE

# @brief Builds one mutant binary and runs one fuzz target on it.
# @param $1 mutant name
# @param $2 updater source file that is broken (a file name in $UPD)
# @param $3 sed expression that makes the break (must change the file)
# @param $4 fuzz target to run
# @stdout "CAUGHT" or "MISSED", the mutant name, and the first finding line
# @return 0 always (the verdict is in the output)
run_mutant() {
  name=$1; file=$2; expr=$3; target=$4
  dir=build/mutants/$name
  rm -rf "$dir"; mkdir -p "$dir/work"
  sed "$expr" "$UPD/$file" > "$dir/$file"
  if cmp -s "$UPD/$file" "$dir/$file"; then
    echo "BROKEN-MUTANT $name: the sed expression did not change $file"
    return 0
  fi
  base=$(basename "$file" .cpp)
  # shellcheck disable=SC2086
  clang++ $FLAGS -c "$dir/$file" -o "$dir/mut.o" || { echo "BUILD-FAILED $name"; return 0; }
  objs=$(ls build/obj/*.o | grep -v "upd_$base.o")
  # shellcheck disable=SC2086
  clang++ -fsanitize=undefined -fno-sanitize-recover=undefined -o "$dir/fuzz" $objs "$dir/mut.o" \
    build/ref_frame.o build/ref_crc32.o -L"$OPENSSL_LIB" -lcrypto ||
    { echo "LINK-FAILED $name"; return 0; }
  mkdir -p "$dir/log"
  "$dir/fuzz" "$target" --seed 5 --n "$N" --out "$dir/log" --work "$dir/work" \
    --ctrl-images "$IMAGES" --test-images "$TEST_IMAGES" 2> "$dir/stderr" > /dev/null
  first=$(grep -m1 "^FINDING\|^CRASH\|^HANG" "$dir/log/$target.log")
  if [ -n "$first" ]; then
    echo "CAUGHT $name ($target): $first" | cut -c1-260
  else
    echo "MISSED $name ($target)"
  fi
  return 0
}

mkdir -p "$OUT"
{
run_mutant arm-check-off updater.cpp \
  's/if (!arm_.held || arm_.identifier != id || arm_.command_sequence != c.sequence) {'\
'/if (false) {/' updater
run_mutant any-length-send-reports updater.cpp \
  's/      return length == command::kSendReportsBytes;/      return true;/' updater
run_mutant expiry-off updater.cpp \
  's/if (c.expiry == 0 || static_cast<int64_t>(c.expiry) < now_s) {/if (false) {/' updater
run_mutant decode-crc-off frame.cpp \
  's/if (crc::crc32(b, covered) != bytes::readU32(&b\[covered\])) {'\
'/if (false \&\& crc::crc32(b, covered) != bytes::readU32(\&b[covered])) {/' frame
run_mutant parser-gap-off frame.cpp \
  's/if (count_ > 0 \&\& now_ms - last_ms_ > config::kFrameGapMaxMs) {/if (false) {/' parser
run_mutant signature-b-off image_checker.cpp \
  's/if (!signatureOk(key_b_, signature_b, image, signed_bytes)) {'\
'/if (false \&\& !signatureOk(key_b_, signature_b, image, signed_bytes)) {/' image
run_mutant record-crc-off durable_record.cpp \
  's/if (bytes::readU32(&file\[record_bytes_\]) != crc::crc32(file.data(), record_bytes_)) {'\
'/if (false) {/' records
run_mutant store-crc-off report_store.cpp \
  's/if (bytes::readU32(&raw\[kHeadBytes + n\]) != crc::crc32(raw.data(), kHeadBytes + n)) {'\
'/if (false) {/' reports
} | tee "$OUT/mutants.log"
