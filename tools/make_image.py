#!/usr/bin/env python3
"""Make a signed firmware image (Interfaces Sec. 5.5, Figures 5 and 6) from a program .bin.

  image = header (64 bytes) | body | Signature A (64) | Signature B (64)
  body  = 448 zero bytes | program .bin

The zero bytes put the program's vector table at region start + 0x200 when the image is
written at the start of its region (layout.h, IMAGE_CODE_OFFSET), so header and body stay in
one piece in flash. Both signatures are pure Ed25519 [RFC 8032, Sec. 5.1.6] over
header | body, Signature A with key A and Signature B with key B.

Signing uses the `cryptography` package (OpenSSL underneath); the bootloader checks with
Monocypher. Two implementations written by different people, on purpose: a defect in one is
unlikely to be repeated in the other, and tools/check_images.py shows on the host that they
agree. REQ-130 asks the same of the controller and the onboard computer.

Run with the Python that has `cryptography` (../.venv next to the repository).

Usage:
  make_image.py --bin app.bin --kind main --slot A --version 1 \\
                --key-a key-a.pem --key-b key-b.pem --build-id "abc1234 test A" --out a.img
"""
import argparse
import sys
import zlib
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

import firmware_layout as fl

RESET_VECTOR_OFFSET = 4   # vector table word 1 = reset handler [Arm DUI 0553A, Sec. 2.3.4, p. 2-24]
WORD_BYTES = 4
MAX_U32 = 0xFFFFFFFF


def load_key(path: Path) -> Ed25519PrivateKey:
    key = serialization.load_pem_private_key(path.read_bytes(), password=None)
    if not isinstance(key, Ed25519PrivateKey):
        sys.exit(f"{path}: not an Ed25519 private key")
    return key


def check_linked_for_region(program: bytes, region_start: int, slot: str) -> None:
    """The reset handler must lie inside the program as placed in this region; otherwise the
    .bin was linked for another region (the bootloader would refuse it: start address)."""
    code_start = region_start + fl.CODE_OFFSET
    reset = int.from_bytes(program[RESET_VECTOR_OFFSET:RESET_VECTOR_OFFSET + WORD_BYTES], "little")
    if not code_start <= reset < code_start + len(program):
        sys.exit(f"reset handler 0x{reset:08x} is outside slot {slot} "
                 f"(0x{code_start:08x}..): wrong build for this slot")


def make_header(kind: int, version: int, body: bytes, build_id: bytes) -> bytes:
    return fl.HEADER_STRUCT.pack(
        fl.MAGIC, fl.FORMAT, kind, fl.TARGET, version, len(body),
        zlib.crc32(body),   # same CRC-32 as Interfaces Sec. 4.0 (tools/check_crc32.py)
        build_id.ljust(fl.BUILD_ID_BYTES, b"\0"),
        bytes(fl.RESERVED_BYTES),           # reserved: 12 bytes of 0x00
    )


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--bin", required=True, type=Path, help="program binary (objcopy -O binary)")
    p.add_argument("--kind", required=True, choices=fl.KINDS)
    p.add_argument("--slot", required=True, choices=fl.REGIONS)
    p.add_argument("--version", required=True, type=int)
    p.add_argument("--key-a", required=True, type=Path)
    p.add_argument("--key-b", required=True, type=Path)
    p.add_argument("--build-id", required=True, help=f"ASCII, at most {fl.BUILD_ID_BYTES} bytes")
    p.add_argument("--out", required=True, type=Path)
    a = p.parse_args()

    region_start, region_bytes, region_kind = fl.REGIONS[a.slot]
    if a.kind != region_kind:
        sys.exit(f"slot {a.slot} holds {region_kind} images, not {a.kind}")
    if not 0 <= a.version <= MAX_U32:
        sys.exit("version must fit 32 bits")
    build_id = a.build_id.encode("ascii")
    if len(build_id) > fl.BUILD_ID_BYTES:
        sys.exit(f"build id longer than {fl.BUILD_ID_BYTES} bytes")

    program = a.bin.read_bytes()
    check_linked_for_region(program, region_start, a.slot)
    body = bytes(fl.CODE_OFFSET - fl.HEADER_BYTES) + program
    header = make_header(fl.KINDS[a.kind], a.version, body, build_id)
    signed = header + body
    image = signed + load_key(a.key_a).sign(signed) + load_key(a.key_b).sign(signed)
    if len(image) > region_bytes:
        sys.exit(f"image is {len(image)} bytes; slot {a.slot} holds {region_bytes}")

    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(image)
    print(f"{a.out}: {a.kind} v{a.version} for slot {a.slot} at 0x{region_start:08x}, "
          f"{len(image)} of {region_bytes} bytes, body CRC 0x{zlib.crc32(body):08x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
