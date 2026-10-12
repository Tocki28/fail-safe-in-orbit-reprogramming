#!/usr/bin/env python3
"""Copy a file with one byte inverted, to make a damaged image for the bench tests.

Usage: flip_byte.py <in> <offset> <out>      (offset may be hex, e.g. 0x600)
"""
import sys
from pathlib import Path

ALL_BITS = 0xFF


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    data = bytearray(Path(sys.argv[1]).read_bytes())
    offset = int(sys.argv[2], 0)
    data[offset] ^= ALL_BITS
    Path(sys.argv[3]).write_bytes(data)
    print(f"{sys.argv[3]}: byte 0x{offset:x} inverted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
