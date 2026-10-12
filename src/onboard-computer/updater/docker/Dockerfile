# Build environment for fsir-updater on the onboard computer (STM32MP257F board: Cortex-A35,
# 64-bit ARM, Linux). An arm64 Debian container, emulated on the Intel Mac, so g++ builds for
# the board directly. The same base and compiler as the owner's other board builds (Debian
# bookworm, g++ 12.2), plus libssl-dev for libcrypto.a: the updater links it statically, so
# the board needs no OpenSSL of its own.
FROM --platform=linux/arm64 debian:bookworm
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ make libssl-dev file \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
