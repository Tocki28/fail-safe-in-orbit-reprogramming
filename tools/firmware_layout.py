"""Flash layout and image format, read from the C headers the firmware is built with.

The tools take every address, size and field value from src/controller/common/layout.h,
image_format.h and boot_state.h, so the bootloader, the linker script and the tools cannot
disagree.
Only plain `#define NAME <number>` lines are read; expressions are left to the C code.
"""
import re
import struct
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
COMMON = REPO / "src" / "controller" / "common"
_DEFINE = re.compile(r"^#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|\d+)U?\b", re.MULTILINE)


def _read_defines() -> dict:
    values = {}
    for header in ("layout.h", "image_format.h", "boot_state.h"):
        for name, number in _DEFINE.findall((COMMON / header).read_text()):
            values[name] = int(number, 0)
    return values


D = _read_defines()

MAGIC = bytes([D["IMAGE_MAGIC_0"], D["IMAGE_MAGIC_1"], D["IMAGE_MAGIC_2"], D["IMAGE_MAGIC_3"]])
FORMAT = D["IMAGE_FORMAT_VERSION"]
KINDS = {
    "bootloader": D["IMAGE_KIND_BOOTLOADER"],
    "main": D["IMAGE_KIND_MAIN"],
    "last-resort": D["IMAGE_KIND_LAST_RESORT"],
}
TARGET = D["IMAGE_TARGET_CONTROLLER"]
HEADER_BYTES = D["IMAGE_HEADER_BYTES"]
SIGNATURE_BYTES = D["IMAGE_SIGNATURE_BYTES"]
BUILD_ID_BYTES = D["IMAGE_BUILD_ID_BYTES"]
RESERVED_BYTES = HEADER_BYTES - D["IMAGE_OFFSET_BUILD_ID"] - BUILD_ID_BYTES   # 12
CODE_OFFSET = D["IMAGE_CODE_OFFSET"]

# Header fields in order, little-endian (Interfaces Sec. 4.0 and Sec. 5.5, Figure 6):
# magic, format, kind, target, version, length, body check value, build identifier, reserved.
HEADER_STRUCT = struct.Struct("<4sBBHIII32s12s")
assert HEADER_STRUCT.size == HEADER_BYTES
assert RESERVED_BYTES == 12

# Region name -> (start address, size in bytes, the image kind it may hold).
REGIONS = {
    "A": (D["SLOT_A_START"], D["SLOT_BYTES"], "main"),
    "B": (D["SLOT_B_START"], D["SLOT_BYTES"], "main"),
    "last-resort": (D["LAST_RESORT_START"], D["LAST_RESORT_BYTES"], "last-resort"),
}

BOOT_STATE_START = D["BOOT_STATE_START"]
BOOT_STATE_COPY_BYTES = D["BOOT_STATE_COPY_BYTES"]
BOOT_SLOTS = {"none": D["BOOT_SLOT_NONE"], "A": D["BOOT_SLOT_A"], "B": D["BOOT_SLOT_B"]}


if __name__ == "__main__":
    # `firmware_layout.py NAME` prints one value in hex, for the Makefile.
    import sys
    print(f"0x{D[sys.argv[1]]:08X}")
