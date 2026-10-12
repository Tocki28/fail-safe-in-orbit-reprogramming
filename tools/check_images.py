#!/usr/bin/env python3
"""Host check: run the bootloader's own image checks (src/controller/bootloader/image_check.c,
with the Monocypher it links and the public keys in keys.h) on the images the build made, and
compare each result with the expected one.

Each image is placed at the start of a buffer the size of its region, the rest 0xFF as
erased flash reads, and checked with that region's chip address, as the bootloader does.
This also shows that Monocypher accepts the signatures the `cryptography` package made.

Usage: check_images.py <image directory> <host build directory>
"""
import ctypes
import subprocess
import sys
from pathlib import Path

import firmware_layout as fl

ERASED = 0xFF
VERSION_OFFSET = 8       # image_format.h, IMAGE_OFFSET_VERSION
TARGET_OFFSET = 6        # image_format.h, IMAGE_OFFSET_TARGET
LENGTH_OFFSET = 12       # image_format.h, IMAGE_OFFSET_LENGTH
assert (VERSION_OFFSET, TARGET_OFFSET, LENGTH_OFFSET) == (
    fl.D["IMAGE_OFFSET_VERSION"], fl.D["IMAGE_OFFSET_TARGET"], fl.D["IMAGE_OFFSET_LENGTH"])


def build(out_dir: Path) -> ctypes.CDLL:
    out_dir.mkdir(parents=True, exist_ok=True)
    lib = out_dir / "libimagecheck.so"
    mono = fl.REPO / "third_party" / "monocypher"
    boot = fl.REPO / "src" / "controller" / "bootloader"
    subprocess.run(["cc", "-O2", "-shared", "-fPIC", f"-I{fl.COMMON}", f"-I{boot}", f"-I{mono}",
                    str(boot / "image_check.c"), str(fl.COMMON / "crc32.c"),
                    str(mono / "monocypher.c"), str(mono / "monocypher-ed25519.c"),
                    "-o", str(lib)], check=True)
    dll = ctypes.CDLL(str(lib))
    dll.image_check_contents.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32,
                                         ctypes.c_uint8, ctypes.POINTER(ctypes.c_uint32)]
    dll.image_check_contents.restype = ctypes.c_int
    dll.image_check_signatures.argtypes = [ctypes.c_char_p, ctypes.c_uint32]
    dll.image_check_signatures.restype = ctypes.c_int
    dll.image_result_text.argtypes = [ctypes.c_int]
    dll.image_result_text.restype = ctypes.c_char_p
    return dll


def check(dll: ctypes.CDLL, image: bytes, region: str) -> str:
    start, size, kind = fl.REGIONS[region]
    flash = image + bytes([ERASED]) * (size - len(image))
    signed = ctypes.c_uint32(0)
    result = dll.image_check_contents(flash, start, size, fl.KINDS[kind], ctypes.byref(signed))
    if result == 0:
        result = dll.image_check_signatures(flash, signed.value)
    return dll.image_result_text(result).decode()


def changed(image: bytes, offset: int, new: bytes) -> bytes:
    return image[:offset] + new + image[offset + len(new):]


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    images = Path(sys.argv[1])
    dll = build(Path(sys.argv[2]))
    a = (images / "slotA-v1.img").read_bytes()
    cases = [
        # (what, image bytes, region, expected result)
        ("good image, slot A", a, "A", "passed"),
        ("good image, slot B (confirms)", (images / "slotB-v2.img").read_bytes(), "B", "passed"),
        ("good image, slot B (hangs)", (images / "slotB-v2-hang.img").read_bytes(), "B", "passed"),
        ("good last-resort image", (images / "last-resort-v1.img").read_bytes(), "last-resort",
         "passed"),
        ("one body byte flipped", (images / "slotA-v1-flipped.img").read_bytes(), "A",
         "body check value"),
        ("Signature A by an unknown key", (images / "slotA-v1-wrong-key-a.img").read_bytes(), "A",
         "signature A"),
        ("Signature B by an unknown key", (images / "slotB-v2-wrong-key-b.img").read_bytes(), "B",
         "signature B"),
        ("version changed after signing", changed(a, VERSION_OFFSET, b"\x09"), "A", "signature A"),
        ("built for another controller", changed(a, TARGET_OFFSET, b"\x32\x04"), "A", "target"),
        ("length larger than the slot", changed(a, LENGTH_OFFSET, b"\xff\xff\x00\x00"), "A",
         "length"),
        ("proprietary string damaged", changed(a, 0, b"X"), "A", "proprietary string"),
        ("slot A image placed in slot B", a, "B", "start address outside the region"),
        ("main firmware in last-resort region", a, "last-resort", "kind"),
        ("erased region", b"", "A", "empty"),
    ]
    failures = 0
    for what, image, region, expected in cases:
        got = check(dll, image, region)
        ok = got == expected
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {what:38s} region {region:11s} -> {got}"
              + ("" if ok else f"   (expected {expected})"))
    print("image host check:", "all passed" if failures == 0 else f"{failures} FAILED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
