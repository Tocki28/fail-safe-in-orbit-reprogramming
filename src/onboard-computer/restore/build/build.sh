#!/bin/sh
# Build the static aarch64 OpenOCD on the Mac (Docker, emulated arm64 - slow, ~20-40 min).
# Output: ../dist/openocd (binary), ../dist/tcl/ (scripts), ../dist/VERSIONS.txt
set -eu
here=$(cd "$(dirname "$0")" && pwd)
dist="$here/../dist"
mkdir -p "$dist"
docker build --platform linux/arm64 -t fsir-openocd-build "$here"
docker run --rm --platform linux/arm64 \
    -v fsir-openocd-work:/work -v "$here":/build:ro -v "$dist":/out \
    fsir-openocd-build sh /build/build-in-container.sh
