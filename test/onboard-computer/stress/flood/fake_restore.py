#!@PYTHON@ -S
"""Fake restore script for the flood stress audit: records when it was called.

The updater runs it as `restore.py reset` when a trial overruns or the controller is silent
(restart_runner.hpp). The rig copies this file next to its log, replacing @PYTHON@ with the
absolute path of the interpreter (interpreter start is then as short as a Python start can be:
-S skips the site module). The call time is taken as the first action, so the line shows when
the script ran, not when it finished. The one result line printed is the one the updater counts
as "the controller was reset" (restart_runner.hpp, header comment: "RESTORE DONE: reset").
"""
import os
import sys
import time

CALLS_FILE = "restore-calls.txt"   # next to this script; the rig reads it


def main():
    """Appends "<wall time> <arguments>" to the calls file and prints the result line.

    Returns:
        Exit status 0, as the real script has after a reset that worked.
    """
    called = time.time()
    path = os.path.join(os.path.dirname(os.path.abspath(sys.argv[0])), CALLS_FILE)
    with open(path, "a") as out:
        out.write("%.4f %s\n" % (called, " ".join(sys.argv[1:])))
    print("RESTORE DONE: reset", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
