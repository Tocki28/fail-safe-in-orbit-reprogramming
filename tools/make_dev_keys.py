#!/usr/bin/env python3
"""Generate the development Ed25519 key pairs used to sign bench images.

Writes key-a.pem and key-b.pem (the two signing keys, Interfaces Sec. 5.5) and
key-wrong.pem (a key the controller does not know, for the refusal test) into a
directory OUTSIDE the repository. Private keys never enter the repository; only
the public keys are copied in, by make_keys_header.py.

Development keys only: unencrypted PEM files, no flight use.

Usage: make_dev_keys.py <key directory>
"""
import os
import sys
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

KEY_NAMES = ("key-a.pem", "key-b.pem", "key-wrong.pem")
# Owner read/write only: the files hold private keys.
PRIVATE_FILE_MODE = 0o600
PRIVATE_DIR_MODE = 0o700


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    key_dir = Path(sys.argv[1]).expanduser().resolve()
    repo = Path(__file__).resolve().parent.parent
    if key_dir == repo or repo in key_dir.parents:
        print(f"refused: {key_dir} is inside the repository", file=sys.stderr)
        return 1
    key_dir.mkdir(mode=PRIVATE_DIR_MODE, parents=True, exist_ok=True)
    for name in KEY_NAMES:
        path = key_dir / name
        if path.exists():
            print(f"kept existing {path}")
            continue
        pem = Ed25519PrivateKey.generate().private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.PKCS8,
            encryption_algorithm=serialization.NoEncryption(),
        )
        # O_EXCL: never overwrite a key that appeared in the meantime.
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, PRIVATE_FILE_MODE)
        with os.fdopen(fd, "wb") as f:
            f.write(pem)
        print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
