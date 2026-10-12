# Build environment for a static OpenOCD for the onboard computer (STM32MP257F-DK, aarch64).
# Debian bookworm arm64 base, emulated on the Intel Mac.
# Run through build.sh, not by hand.
FROM --platform=linux/arm64 debian:bookworm
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential autoconf automake libtool pkg-config texinfo \
      ca-certificates curl bzip2 file \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /work
