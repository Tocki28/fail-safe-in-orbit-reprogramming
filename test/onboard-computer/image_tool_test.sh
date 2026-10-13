#!/bin/sh
# image_tool_test.sh - regression test of tools/make_image.py (bench only, REQ-101).
#
# Question answered: does make_image.py still make byte-identical images for the kinds main
# and last-resort, and does it make a correct image of kind bootloader (Interfaces Sec. 5.5:
# header | body | Signature A | Signature B, body = the raw bootloader.bin as linked)?
#
# Checks (each prints PASS or FAIL; the exit status is non-zero if any FAIL):
#  1 bootloader image: size = 64 + len(bootloader.bin) + 128
#  2 bootloader image: body (bytes 64 .. 64+len) is byte-identical to bootloader.bin
#  3 bootloader image: kind byte is 1, length field = len(bootloader.bin)
#  4 fsir-updater --check-image --kind bootloader accepts it (public keys of the test keys)
#  5 the same image is refused when the other key is not the one the updater knows
#  6 slot A, slot B and last-resort images rebuilt from the committed images (version,
#    build id and program taken from the committed image's own header and body) equal the
#    committed ones in header and body; with the development keys also in every byte.
#
# Usage: image_tool_test.sh      env: PY (python with `cryptography`), KEYS (development keys),
#                                     FSIR_UPDATER, BOOTLOADER_BIN
here=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$here/../.." && pwd)
PY=${PY:-$REPO/../.venv/bin/python}
KEYS=${KEYS:-$REPO/../keys-dev}
FSIR_UPDATER=${FSIR_UPDATER:-$REPO/src/onboard-computer/updater/build/host/fsir-updater}
BOOTLOADER_BIN=${BOOTLOADER_BIN:-$REPO/src/controller/bootloader/build/bootloader.bin}
MAKE_IMAGE=${MAKE_IMAGE:-$REPO/tools/make_image.py}
HDR=64; SIGS=128

T=$(mktemp -d "${TMPDIR:-/tmp}/image_tool_test.XXXXXX") || { echo "FAIL setup: mktemp"; exit 1; }
trap 'rm -rf "$T"' EXIT
trap 'exit 1' INT TERM HUP
npass=0; nfail=0

# @brief  Print one PASS or FAIL line and count it.
#
# @param $1  1 for pass, anything else for fail
# @param $2  name of the check
# @return 0
verdict() {
    if [ "$1" = 1 ]; then npass=$((npass + 1)); echo "PASS $2"; else nfail=$((nfail + 1)); echo "FAIL $2"; fi
    return 0
}

[ -x "$PY" ] || { echo "FAIL setup: python not found: $PY"; exit 1; }
[ -x "$FSIR_UPDATER" ] || { echo "FAIL setup: fsir-updater not built: $FSIR_UPDATER"; exit 1; }
[ -f "$BOOTLOADER_BIN" ] || { echo "FAIL setup: no bootloader.bin: $BOOTLOADER_BIN"; exit 1; }

# Test keys (outside the repository, as make_dev_keys.py requires): independent of the
# development keys, so checks 1-5 need nothing else.
"$PY" "$REPO/tools/make_dev_keys.py" "$T/keys" > /dev/null || { echo "FAIL setup: make_dev_keys.py"; exit 1; }
"$PY" - "$T/keys" <<'EOT' || { echo "FAIL setup: public keys"; exit 1; }
import sys
from pathlib import Path
from cryptography.hazmat.primitives import serialization
d = Path(sys.argv[1])
for n in ("key-a", "key-b", "key-wrong"):
    k = serialization.load_pem_private_key((d / f"{n}.pem").read_bytes(), password=None)
    (d / f"{n}.pub").write_bytes(k.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw))
EOT

len=$(( $(wc -c < "$BOOTLOADER_BIN") ))
"$PY" "$MAKE_IMAGE" --bin "$BOOTLOADER_BIN" --kind bootloader --slot bootloader --version 1 \
    --key-a "$T/keys/key-a.pem" --key-b "$T/keys/key-b.pem" --build-id "image_tool_test" \
    --out "$T/bl.img" > /dev/null || { echo "FAIL setup: make_image.py bootloader"; exit 1; }

size=$(( $(wc -c < "$T/bl.img") ))
[ "$size" -eq $(( HDR + len + SIGS )) ] && verdict 1 "1 bootloader image size is 64 + $len + 128" \
    || verdict 0 "1 bootloader image size $size, expected $(( HDR + len + SIGS ))"

tail -c +$(( HDR + 1 )) "$T/bl.img" | head -c "$len" > "$T/body.bin"
cmp -s "$T/body.bin" "$BOOTLOADER_BIN" && verdict 1 "2 bootloader image body equals bootloader.bin" \
    || verdict 0 "2 bootloader image body differs from bootloader.bin"

kind=$(od -An -tu1 -j 5 -N 1 "$T/bl.img" | tr -d ' ')
hl=$(od -An -tu1 -j 12 -N 4 "$T/bl.img" | { read -r a b c d; echo $(( a + b * 256 + c * 65536 + d * 16777216 )); })
[ "$kind" = 1 ] && [ "$hl" -eq "$len" ] && verdict 1 "3 header kind 1, length field $len" \
    || verdict 0 "3 header kind $kind, length field $hl (expected 1, $len)"

"$FSIR_UPDATER" --check-image "$T/bl.img" --kind bootloader --key-a "$T/keys/key-a.pub" \
    --key-b "$T/keys/key-b.pub" > "$T/check.out" 2>&1
rc=$?
[ "$rc" -eq 0 ] && verdict 1 "4 fsir-updater --check-image --kind bootloader accepts the image" \
    || verdict 0 "4 fsir-updater status $rc: $(head -n 1 "$T/check.out")"

"$FSIR_UPDATER" --check-image "$T/bl.img" --kind bootloader --key-a "$T/keys/key-wrong.pub" \
    --key-b "$T/keys/key-b.pub" > "$T/check.out" 2>&1
rc=$?
[ "$rc" -eq 1 ] && verdict 1 "5 the image is refused under another key A (status 1)" \
    || verdict 0 "5 wrong key A: status $rc, expected 1"

# Check 6: take version, build id and program out of a committed image's own header and body,
# make the image again, compare. The committed images are the output of the original script.
for item in "A:main:A:test/controller/build/images/slotA-v1.img" \
            "B:main:B:src/controller/build/images/firmware-B.img" \
            "last-resort:last-resort:last-resort:test/controller/build/images/last-resort-v1.img"; do
    name=${item%%:*}; rest=${item#*:}
    kindname=${rest%%:*}; rest=${rest#*:}
    slot=${rest%%:*}; orig=$REPO/${rest#*:}
    if [ ! -f "$orig" ]; then verdict 0 "6 slot $name: committed image missing: $orig"; continue; fi
    "$PY" - "$orig" "$T/prog-$name.bin" "$T/args-$name" <<'EOT' || { verdict 0 "6 slot $name: cannot read $orig"; continue; }
import sys, struct
img = open(sys.argv[1], "rb").read()
magic, fmt, kind, target, version, length, crc, build_id, reserved = struct.unpack("<4sBBHIII32s12s", img[:64])
body = img[64:64 + length]
open(sys.argv[2], "wb").write(body[448:])          # IMAGE_CODE_OFFSET - HEADER_BYTES zero bytes first
open(sys.argv[3], "w").write(f"{version}\n{build_id.rstrip(chr(0).encode()).decode('ascii')}\n")
EOT
    version=$(sed -n 1p "$T/args-$name"); build_id=$(sed -n 2p "$T/args-$name")
    "$PY" "$MAKE_IMAGE" --bin "$T/prog-$name.bin" --kind "$kindname" --slot "$slot" \
        --version "$version" --key-a "$T/keys/key-a.pem" --key-b "$T/keys/key-b.pem" \
        --build-id "$build_id" --out "$T/new-$name.img" > /dev/null \
        || { verdict 0 "6 slot $name: make_image.py failed"; continue; }
    n=$(( $(wc -c < "$orig") ))
    if [ "$(( $(wc -c < "$T/new-$name.img") ))" -eq "$n" ] \
        && cmp -s -n $(( n - SIGS )) "$T/new-$name.img" "$orig"; then
        verdict 1 "6 slot $name: header and body identical to the committed image"
    else
        verdict 0 "6 slot $name: header or body differ from the committed image"
    fi
    if [ -f "$KEYS/key-a.pem" ] && [ -f "$KEYS/key-b.pem" ]; then
        "$PY" "$MAKE_IMAGE" --bin "$T/prog-$name.bin" --kind "$kindname" --slot "$slot" \
            --version "$version" --key-a "$KEYS/key-a.pem" --key-b "$KEYS/key-b.pem" \
            --build-id "$build_id" --out "$T/newdev-$name.img" > /dev/null
        cmp -s "$T/newdev-$name.img" "$orig" && verdict 1 "6 slot $name: every byte identical (development keys)" \
            || verdict 0 "6 slot $name: bytes differ with the development keys"
    else
        echo "NOTE slot $name: development keys not found in $KEYS: signature bytes not compared"
    fi
done

echo "image_tool_test: $npass PASS, $nfail FAIL"
[ "$nfail" -eq 0 ]
