#!/usr/bin/env python3
"""Write one boot-state record (src/controller/common/boot_state.h) as a 16-byte file, for
bench tests that start from a chosen state, e.g. "confirmed A, trial B". Written through SWD
at BOOT_STATE_COPY_0 after the boot-state pages are erased.

  bytes 0-3   sequence (little-endian)
  byte  4     confirmed slot     byte 5  trial slot     byte 6  failed starts    byte 7  flags
  bytes 8-11  given-up check     bytes 12-15  CRC-32 of bytes 0-11 (Interfaces Sec. 4.0)

Flags: bit 0 FALLBACK, no other bit (owner's decision 30, 15 Oct 2026). Given-up check: the body
check value of the image given up last, 0 for none. Both default to 0, which is the record
written before decision 30 (bytes 7-11 were "unused" and "reserved", 0): no FALLBACK, no given-up
image.

Usage: make_boot_state.py --confirmed A --trial B [--failed-starts 0] [--sequence 1]
       [--fallback] [--given-up-check 0x1234ABCD] --out f.bin
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

import firmware_layout as fl

RECORD = struct.Struct("<IBBBBI")   # the 12 checked bytes, in boot_state_t field order
FLAG_FALLBACK = 0x01                # BOOT_STATE_FLAG_FALLBACK (common/boot_state.h)


def main() -> int:
    """Builds the record from the command line and writes it.

    Why: a bench test that needs a given boot state (a trial set, or FALLBACK with a given-up
    image) writes this file at BOOT_STATE_COPY_0 instead of driving the controller there.

    Args (command line):
        --confirmed: confirmed slot, A or B.
        --trial: trial slot, none, A or B; must not be the confirmed slot.
        --failed-starts: trial starts so far, 0 or more (the controller refuses a value above
            BOOT_STATE_MAX_STARTS as damage).
        --sequence: the record's sequence number.
        --fallback: set the FALLBACK flag (byte 7, bit 0).
        --given-up-check: body check value of the given-up image, 0 to 0xFFFFFFFF; 0 for none.
        --out: path of the 16-byte file.

    Returns:
        0 when the file was written; exits with a message on a trial equal to the confirmed slot
        or a given-up check outside 32 bits.
    """
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--confirmed", required=True, choices=("A", "B"))
    p.add_argument("--trial", default="none", choices=("none", "A", "B"))
    p.add_argument("--failed-starts", type=int, default=0)
    p.add_argument("--sequence", type=int, default=1)
    p.add_argument("--fallback", action="store_true")
    p.add_argument("--given-up-check", type=lambda s: int(s, 0), default=0)
    p.add_argument("--out", required=True, type=Path)
    a = p.parse_args()
    if a.trial == a.confirmed:
        sys.exit("the trial slot must be the slot that is not confirmed")
    if not 0 <= a.given_up_check <= 0xFFFFFFFF:
        sys.exit("the given-up check is a 32-bit value")

    flags = FLAG_FALLBACK if a.fallback else 0
    checked = RECORD.pack(a.sequence, fl.BOOT_SLOTS[a.confirmed], fl.BOOT_SLOTS[a.trial],
                          a.failed_starts, flags, a.given_up_check)
    record = checked + struct.pack("<I", zlib.crc32(checked))
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(record)
    print(f"{a.out}: confirmed {a.confirmed}, trial {a.trial}, failed starts {a.failed_starts}, "
          f"flags {flags}, given-up check 0x{a.given_up_check:08x}, sequence {a.sequence} "
          f"-> write at 0x{fl.BOOT_STATE_START:08x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
