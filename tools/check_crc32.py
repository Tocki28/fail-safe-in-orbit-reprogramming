#!/usr/bin/env python3
"""Host check: crc32_compute() in src/controller/common/crc32.c gives the same value as
Python's zlib.crc32, whose CRC-32 (polynomial 0x4C11DB7, start 0xFFFFFFFF, bits taken least
significant first, result inverted) is the one Interfaces Sec. 4.0 names.

Compiles crc32.c for this computer with `cc`, calls it through ctypes, compares.
Usage: check_crc32.py <host build directory>
"""
import ctypes
import random
import subprocess
import sys
import zlib
from pathlib import Path

import firmware_layout as fl

SEED = 2026          # fixed, so a failure can be repeated
RANDOM_BYTES = 40 * 1024   # one slot's worth


def build(out_dir: Path) -> ctypes.CDLL:
    out_dir.mkdir(parents=True, exist_ok=True)
    lib = out_dir / "libcrc32.so"
    subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                    str(fl.COMMON / "crc32.c"), "-o", str(lib)], check=True)
    dll = ctypes.CDLL(str(lib))
    dll.crc32_compute.argtypes = [ctypes.c_char_p, ctypes.c_uint32]
    dll.crc32_compute.restype = ctypes.c_uint32
    return dll


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    dll = build(Path(sys.argv[1]))
    rng = random.Random(SEED)
    vectors = {
        "empty": b"",
        "one byte 'a'": b"a",
        "'123456789'": b"123456789",
        "bytes 0..255": bytes(range(256)),
        "64 x 0xFF (erased flash)": b"\xff" * 64,
        f"{RANDOM_BYTES} random bytes": bytes(rng.getrandbits(8) for _ in range(RANDOM_BYTES)),
    }
    failures = 0
    for name, data in vectors.items():
        ours, ref = dll.crc32_compute(data, len(data)), zlib.crc32(data)
        ok = ours == ref
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name:28s} crc32.c 0x{ours:08x}  zlib 0x{ref:08x}")
    print("CRC-32 host check:", "all passed" if failures == 0 else f"{failures} FAILED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
