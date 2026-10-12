#!/usr/bin/env python3
"""Write one boot-state record (src/controller/common/boot_state.h) as a 16-byte file, for
bench tests that start from a chosen state, e.g. "confirmed A, trial B". Written through SWD
at BOOT_STATE_COPY_0 after the boot-state pages are erased.

  bytes 0-3   sequence (little-endian)
  byte  4     confirmed slot     byte 5  trial slot     byte 6  failed starts    byte 7  0
  bytes 8-11  reserved, 0        bytes 12-15  CRC-32 of bytes 0-11 (Interfaces Sec. 4.0)

Usage: make_boot_state.py --confirmed A --trial B [--failed-starts 0] [--sequence 1] --out f.bin
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

import firmware_layout as fl

RECORD = struct.Struct("<IBBBBI")   # the 12 checked bytes, in boot_state_t field order


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--confirmed", required=True, choices=("A", "B"))
    p.add_argument("--trial", default="none", choices=("none", "A", "B"))
    p.add_argument("--failed-starts", type=int, default=0)
    p.add_argument("--sequence", type=int, default=1)
    p.add_argument("--out", required=True, type=Path)
    a = p.parse_args()
    if a.trial == a.confirmed:
        sys.exit("the trial slot must be the slot that is not confirmed")

    checked = RECORD.pack(a.sequence, fl.BOOT_SLOTS[a.confirmed], fl.BOOT_SLOTS[a.trial],
                          a.failed_starts, 0, 0)
    record = checked + struct.pack("<I", zlib.crc32(checked))
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(record)
    print(f"{a.out}: confirmed {a.confirmed}, trial {a.trial}, failed starts {a.failed_starts}, "
          f"sequence {a.sequence} -> write at 0x{fl.BOOT_STATE_START:08x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
