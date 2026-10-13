#!/usr/bin/env python3
"""Ground console: drives a firmware update through the onboard computer over UDP.

Frames as in the Interfaces document, Sec. 4.0; answers as in Sec. 4.2; commands as in
Sec. 5.1; reports as in Sec. 5.4. Standard library only.

BENCH LIMIT (accepted for the demo): no envelope encryption and no command signatures
(Interfaces Sec. 4.1 is left out). Frames travel as plain UDP datagrams, one frame each.

Every command is stop-and-wait: the next one is sent only after the previous one's DONE or
REFUSED (Sec. 4.2). Commands whose answer does not come within the timeout are sent again
with the same sequence number, so the onboard computer answers ALREADY DONE and does not run
them twice (Sec. 4.2, REQ-031).

Subcommands:  status | upload IMAGE --number N | deliver --number N | switch | update [--yes] | confirm

Sequence numbers are kept across runs in a state file (default ~/.fsir/console-seq, option
--seq-file), because the onboard computer refuses an ARM or a dangerous command whose ground
sequence number it has already seen since it started (REFUSED 0x04, Sec. 4.0, REQ-065). See
load_first_seq() for the numbering rule.

`update` ends with the operator's decision: after the controller reports "trial tests passed"
the console shows the trial report and asks "Confirm? [y/N]"; only y sends CONFIRM (Sec. 5.1, REQ-012).
"""
import argparse
import os
import random
import socket
import struct
import sys
import time
import zlib

# ---- Frame, Sec. 4.0 -----------------------------------------------------------------
SYNC = b"\xa5\x5a"                      # Sec. 4.0, field table: start of a frame
VERSION = 0x01                          # Sec. 4.0: frame format version
TYPE_COMMAND, TYPE_ANSWER, TYPE_REPORT = 0x01, 0x02, 0x03   # Sec. 4.0: Type field
DEST_ONBOARD, DEST_CONTROLLER, DEST_GROUND = 0x01, 0x02, 0x03   # Sec. 4.0: Destination field
HEADER = struct.Struct("<2sBBBBHII")    # Sec. 4.0: Sync, Version, Type, Dest, Flags, Length, Seq, Expiry
CHECK_BYTES = 4                         # Sec. 4.0: CRC-32 at the end
PAYLOAD_MAX = 1024                      # Sec. 4.0: Length is 0 to 1024
SEQ_MAX = 2**32 - 1                     # Sec. 4.0: sequence numbers run 1 to 2^32-1

# ---- Answers, Sec. 4.2 ---------------------------------------------------------------
ANSWER = struct.Struct("<IBB")          # Sec. 4.2: sequence answered (4), kind (1), code (1)
RECEIVED, DONE, REFUSED, ALREADY_DONE = 0x01, 0x02, 0x03, 0x04   # Sec. 4.2: kinds
KINDS = {RECEIVED: "RECEIVED", DONE: "DONE", REFUSED: "REFUSED", ALREADY_DONE: "ALREADY DONE"}
REFUSAL_TEXT = {                        # Sec. 4.2: table of REFUSED codes
    0x01: "Dangerous command without a valid arming command",
    0x02: "Arming command expired",
    0x03: "Command past its expiry",
    0x04: "Old command sent again",
    0x05: "Not allowed in the current mode",
    0x06: "Impossible values, or an unknown command",
    0x07: "No new firmware during a trial",
    0x08: "Image damaged",
    0x09: "Image not from the operator",
    0x0A: "Image older than the minimum version",
    0x0B: "Image built for another controller",
    0x0C: "The controller's clock disagrees with the scheduled time",
    0x0D: "A dangerous command cannot be scheduled",
    0x0E: "Image built for the other slot",
    0x0F: "Trial tests not passed",
    0x10: "Image of the wrong kind for its place",
    0x11: "Boot state could not be stored",
    # BENCH ONLY, not in the Sec. 4.2 table: the onboard computer's own reasons, protocol.hpp
    # kNoAnswerBench, kStorageBench, kCannotSendBench (open item: add them to Sec. 4.2).
    0xFD: "bench: the onboard computer could not send on data link A",
    0xFE: "bench: the onboard computer's own storage failed",
    0xFF: "bench: the controller did not answer within its timeout",
}

# ---- Commands, Sec. 5.1 --------------------------------------------------------------
UPLOAD_BEGIN, UPLOAD_CHUNK, UPLOAD_END, UPLOAD_CANCEL, DELIVER = 0x01, 0x02, 0x03, 0x04, 0x05
ARM, SWITCH, GET_STATUS, CONFIRM = 0x0D, 0x20, 0x25, 0x26
CHUNK_MAX = 256                         # Sec. 5.1: UPLOAD CHUNK carries up to 256 data bytes

# ---- Reports, Sec. 5.4 ---------------------------------------------------------------
REPORT_HEALTH, REPORT_EVENT = 0x01, 0x02            # Sec. 5.4: report kind
MODES = {0x01: "OPERATE", 0x02: "TRIAL", 0x03: "FALLBACK"}          # Sec. 5.4: HEALTH mode
SLOTS = {0x01: "A", 0x02: "B", 0x03: "last-resort"}                  # Sec. 5.4: running slot
SELF_TEST = {0x00: "PASSED", 0x01: "FAILED"}                         # Sec. 5.4: last self-test
EVENTS = {                                                           # Sec. 5.4: event codes
    0x0001: "fault detected", 0x0002: "fall-back to the previous copy",
    0x0003: "fall-back to the last-resort firmware", 0x0004: "self-test failed",
    0x0005: "change in memory detected", 0x0006: "trial started",
    0x0007: "firmware confirmed", 0x0008: "firmware given up", 0x0009: "restart",
    0x000A: "trial tests passed", 0x000B: "trial tests failed",
    0x0101: "data link lost", 0x0102: "data link working again",
    0x0103: "restore done", 0x0104: "health reports dropped",
    0x0105: "onboard computer clock differs from ground time",
    0x0106: "controller restarted by the onboard computer",
}
EVENT_FALLBACK_PREVIOUS, EVENT_FALLBACK_LAST_RESORT = 0x0002, 0x0003   # Sec. 5.4: event codes
EVENT_RESTARTED_BY_ONBOARD = 0x0106                                      # Sec. 5.4: event code
EVENT_FAULT_DETECTED, EVENT_MEMORY_CHANGE = 0x0001, 0x0005              # Sec. 5.4: event codes
# Check result in the detail of EVENTs 0x0002 and 0x0003 (Sec. 5.4, "Check result" table): the
# first check the refused copy failed, or why a copy was passed over without a check. COPIED from
# src/controller/common/backup_layout.h (CHECK_*), where the bootloader that writes the codes
# defines them; keep the two in step. Each value is (the header's name without CHECK_, the words).
# The console's test parses the header and fails when a code or a name differs.
CHECK_RESULT = {
    0x00: ("PASSED", "passed"), 0x01: ("EMPTY", "empty"), 0x02: ("MAGIC", "proprietary string"),
    0x03: ("FORMAT", "format"), 0x04: ("KIND", "kind"), 0x05: ("TARGET", "target"),
    0x06: ("LENGTH", "length"), 0x07: ("START", "start address outside the region"),
    0x08: ("BODY", "body check value"), 0x09: ("SIGNATURE_A", "signature A"),
    0x0A: ("SIGNATURE_B", "signature B"),
    # Added 13 Oct 2026 (owner's decision 4): a refusal or skip that is not an image check.
    0x0B: ("FLASH_READ", "flash read error"), 0x0C: ("TRIAL_STARTS", "trial started too often"),
    0x0D: ("RESTARTS", "restarted too often"),
    # Added 13 Oct 2026, second round: a trial or copy passed over without a check of its image.
    0x0E: ("WATCHDOG_NOT_RUNNING", "not started: watchdog not confirmed running"),
    0x0F: ("START_NOT_COUNTED", "not started: start count could not be saved"),
    0x10: ("STATE_DAMAGED", "boot state damaged, only the last-resort tried"),
}
# Bit 7 of the first detail byte of EVENT 0x0005 (Sec. 5.4): set when the bootloader found the
# fault at the last start. COPIED from FAULT_FOUND_BY_BOOTLOADER in fault_codes.h; the console's
# test compares the two.
FOUND_BY_BOOTLOADER = 0x80
# The later-faults byte of EVENT 0x0005 stops counting here. COPIED from BACKUP_FAULT_LATER_MAX in
# src/controller/common/backup_layout.h ("the count stops at 255"): a byte of 255 may mean more.
FAULT_LATER_MAX = 0xFF
# Fault kind, the 1-byte detail of EVENT 0x0001 (owner's decision 2, 13 Oct 2026). COPIED from
# src/controller/common/fault_codes.h (FAULT_KIND_*), where the controller defines the codes; keep
# the two in step. Each value is (the header's name without FAULT_KIND_, the words). The console's
# test parses the header and fails when a code or a name differs.
FAULT_KIND = {
    0x01: ("CORE_CLOCK", "core clock failed, running at 16 MHz"),
}
# Fault location, the 1-byte detail of EVENT 0x0005 (owner's decision 3, 13 Oct 2026). COPIED from
# src/controller/common/fault_codes.h (FAULT_AT_*): one code per function that holds a fault site;
# 0x01-0x0F bootloader, 0x10-0x1F common (both programs), 0x20-0x4F firmware. Each value is
# (the header's name without FAULT_AT_, the words: the program and function, then what is
# impossible there). Kept in step with the header by the console's test, as FAULT_KIND is.
FAULT_LOCATION = {
    0x01: ("VALUE_INDEX", "bootloader backup: pair number past the last pair"),
    0x02: ("SLOT_LETTER", "bootloader: slot code is not A, B or none"),
    0x03: ("RESET_CAUSE_TEXT", "bootloader: reset cause outside its list"),
    0x04: ("LOAD_STATE", "bootloader: boot state load result outside its list"),
    0x05: ("SLOT_REGION", "bootloader: slot code is not A or B"),
    0x06: ("ROLE_TEXT", "bootloader: start role is not confirmed, trial or last resort"),
    0x07: ("CHECK_RESULT_CODE", "bootloader: image check result outside its list"),
    0x10: ("IMAGE_RESULT_TEXT", "image check (both programs): image check result outside its list"),
    0x11: ("BOOT_STATE_OTHER_SLOT", "boot state (both programs): slot code is not A or B"),
    0x20: ("TO_SLOT", "boot slot: stored slot code is not none, A or B"),
    0x21: ("OTHER_SLOT", "boot slot: other slot of none, or of a value outside the slots"),
    0x22: ("STATE_CODE", "boot slot: state code of a value outside the slots"),
    0x23: ("CORE_CLOCK_START_TICK", "core clock: clock is not 170 MHz or 16 MHz (start tick)"),
    0x24: ("UART_LINK_INIT", "UART link: clock is not 170 MHz or 16 MHz (init)"),
    0x25: ("SLOT_CODE", "health monitor: slot code outside the slots"),
    0x26: ("UPDATE_RECEIVER_SLOT_START",
          "update receiver: slot start of a value outside the slots"),
    0x27: ("UPDATE_RECEIVER_REFUSAL_FOR", "update receiver: image check result outside its list"),
    0x28: ("UPDATE_RECEIVER_VALUES_FIT",
          "update receiver: upload identifier outside its list (values fit)"),
    0x29: ("UPDATE_RECEIVER_RUN", "update receiver: upload identifier outside its list (run)"),
    0x2A: ("ARM_STATE_CHECK_INTACT",
          "arm state: a word differs from its complement, or is no code word"),
    0x2B: ("TEST_IMAGE_ACCEPT", "test image: upload identifier outside its list (accept)"),
    0x2C: ("TEST_IMAGE_RUN", "test image: upload identifier outside its list (run)"),
    0x2D: ("COPY_CODE", "mode manager: slot code outside the slots (copy code)"),
    0x2E: ("MODE_MANAGER_POLL",
          "mode manager: fall-back mode (never set) or a mode outside its list (poll)"),
    0x2F: ("MODE_MANAGER_RUN_DANGEROUS",
          "mode manager: dangerous command that is neither switch nor confirm"),
}
# Reason in the detail of EVENT 0x0106 (Sec. 5.4, event table).
RESTART_REASON = {0x01: "trial past its time"}
EVENT_TRIAL_STARTED, EVENT_CONFIRMED, EVENT_GIVEN_UP = 0x0006, 0x0007, 0x0008   # Sec. 5.4: event codes
EVENT_TESTS_PASSED, EVENT_TESTS_FAILED = 0x000A, 0x000B                         # Sec. 5.4: event codes
# Detail byte of EVENT 0x000B, which test failed (Sec. 5.4: "detail: which test").
# TBD-tests: these three numbers are the owner's proposal, awaiting acceptance.
TRIAL_TESTS = {0x01: "self-test", 0x02: "data-link answer", 0x03: "test image"}
ADDED = struct.Struct("<II")            # Sec. 5.4: report number (4), onboard clock time (4)
HEALTH_BODY = struct.Struct("<IBBBIBBHB")   # Sec. 5.4 order: controller clock (4), kind (1),
#   mode (1), firmware kind (1), version (4), slot (1), failed starts (1), trial left (2), self-test (1)
HEALTH_LEN = HEALTH_BODY.size           # 16 bytes as the controller sends it
HEALTH_LEN_ADDED = ADDED.size + HEALTH_LEN   # 24 bytes once the onboard computer has added its two fields

# ---- Bench values --------------------------------------------------------------------
DEFAULT_HOST, DEFAULT_PORT = "192.168.7.1", 47001   # brief: onboard computer bench address and port
EXPIRY_S = 60               # bench value: a command is good for 60 s; ground time is Unix time on the bench
TIMEOUT_S = 3.0             # brief: a command normally answers within a few ms; 3 s counts as lost
# DELIVER moves the whole image to the controller over data link A (115200 baud 8N1 =
# 11520 byte/s, Sec. 6.2): a 64 KiB image is about 6 s of line time, plus flash writes and
# the framing overhead, so 30 s leaves a margin of several times.
DELIVER_TIMEOUT_S = 30.0
RETRIES = 5                 # resends of one command before giving up; keeps a dead link from hanging the demo
HEALTH_WAIT_S = 10.0        # how long `update` waits for the first HEALTH report
# TBD-trial: the trial time is the bench value 120 s (owner's decision for the bench); the flight
# value is not set. The controller reports "trial tests passed" or "failed" inside this time.
TRIAL_TIME_S = 120.0
TRIAL_MARGIN_S = 30.0       # margin for the restart, report delays and the radio loss simulator
UPDATE_WATCH_S = TRIAL_TIME_S + TRIAL_MARGIN_S   # how long `update` watches reports after SWITCH
CONFIRM_WAIT_S = 10.0       # how long to wait for EVENT 0x0007 and HEALTH OPERATE after CONFIRM is DONE
STATUS_SECONDS = 10.0       # default listening time of `status`
RECV_BYTES = 2048           # larger than the biggest frame (16 + 1024 + 4)
DEFAULT_SEQ_FILE = os.path.join("~", ".fsir", "console-seq")   # bench choice: per-user state file


class Refused(Exception):
    """A command was REFUSED or never answered."""


def build(ftype, dest, seq, payload, expiry=0):
    """One frame of bytes: header, payload and CRC-32 (Sec. 4.0).

    The receiver drops any frame whose check value does not match, so every frame the console
    sends is closed with the CRC-32 (zlib) over all bytes before it (Sec. 4.0).

    Args:
        ftype: Type field, TYPE_COMMAND, TYPE_ANSWER or TYPE_REPORT.
        dest: Destination field, DEST_ONBOARD, DEST_CONTROLLER or DEST_GROUND.
        seq: sequence number, 1 to 2^32-1.
        payload: payload bytes, at most PAYLOAD_MAX (1024).
        expiry: expiry time, seconds of Unix time (bench ground time); 0 for none.

    Returns:
        The complete frame as bytes: 16-byte header, payload, 4-byte CRC-32.
    """
    head = HEADER.pack(SYNC, VERSION, ftype, dest, 0, len(payload), seq, expiry) + payload
    return head + struct.pack("<I", zlib.crc32(head))   # Sec. 4.0: CRC-32 like zlib over all bytes before it


def parse(data):
    """One datagram -> frame dict, or None if it is not a good frame (Sec. 4.0: receiver drops it)."""
    if len(data) < HEADER.size + CHECK_BYTES:
        return None
    sync, version, ftype, dest, flags, length, seq, expiry = HEADER.unpack_from(data)
    if sync != SYNC or version != VERSION or length > PAYLOAD_MAX:
        return None
    if len(data) != HEADER.size + length + CHECK_BYTES:
        return None
    if struct.unpack_from("<I", data, len(data) - CHECK_BYTES)[0] != zlib.crc32(data[:-CHECK_BYTES]):
        return None
    return dict(type=ftype, dest=dest, seq=seq, payload=data[HEADER.size:HEADER.size + length])


def unknown(value, digits=2):
    """Text for a number that is in no table, so the operator never sees a bare code.

    Every number the ground receives is shown as words (Sec. 4.2, Sec. 5.4); one that the
    Interfaces tables do not list is shown as "unknown code" with the number, never dropped.

    Args:
        value: the number received.
        digits: hexadecimal digits to print, 2 for a byte, 4 for an event code.

    Returns:
        The text "unknown code 0x.." with the number.
    """
    return f"unknown code 0x{value:0{digits}x}"


def name_of(table, value, digits=2):
    """Look a received number up in a table of the Interfaces document.

    Args:
        table: dict from number to text, one of the tables of Sec. 4.2 or Sec. 5.4.
        value: the number received.
        digits: hexadecimal digits for the unknown text, see unknown().

    Returns:
        The table's text, or unknown(value, digits) when the table has no such number.
    """
    return table[value] if value in table else unknown(value, digits)


def refusal_text(code):
    """Words for the code of a REFUSED answer (Sec. 4.2, table of reasons).

    The operator has to read why a command was refused (Sec. 4.2), so the code is turned into
    the reason of the table; the bench values 0xFD to 0xFF are marked "bench".

    Args:
        code: the code byte of the REFUSED answer.

    Returns:
        The reason text, or "unknown code 0x.." when the code is in no table.
    """
    return name_of(REFUSAL_TEXT, code)


def check_result_text(result):
    """Words for a check result, the second detail byte of EVENTs 0x0002 and 0x0003 (Sec. 5.4).

    The operator reads the first check the refused copy failed, or why a copy was passed over,
    so the byte is turned into the CHECK_RESULT words. A byte outside the table is shown as
    "unknown code" with the number, never dropped.

    Args:
        result: the check-result byte (0x00 to 0x10 are listed).

    Returns:
        The table's words, or "unknown code 0x.." for a byte the table does not list.
    """
    return CHECK_RESULT[result][1] if result in CHECK_RESULT else unknown(result)


def later_faults_text(count):
    """Words for the number of faults found after the first, the second byte of EVENT 0x0005.

    The bootloader's count stops at 255 (BACKUP_FAULT_LATER_MAX), so a byte of 255 only says "at
    least 255". The text then reads "255 or more", so the operator does not read a saturated count
    as an exact one. Every other value is printed as it is.

    Args:
        count: the later-faults byte, 0 to 255.

    Returns:
        "N more after it", or "255 or more after it" when count is FAULT_LATER_MAX.
    """
    if count == FAULT_LATER_MAX:
        return f"{count} or more after it"
    return f"{count} more after it"


def memory_change_text(detail):
    """Words for the detail of EVENT 0x0005 "change in memory detected" (Sec. 5.4).

    The detail is where (1) and later faults (1). Bit 7 of where (FAULT_FOUND_BY_BOOTLOADER in
    fault_codes.h) is set when the bootloader found the fault at the last start, so the operator
    can tell a fault from before the restart from one the running firmware found. The location is
    the lower seven bits, looked up in FAULT_LOCATION; one in no table prints as "unknown code".
    With both bytes the text ends "found by the bootloader, N more after it" (N is the second
    byte, always shown) or "found by the firmware" (N is added only when not 0, never dropped).
    A second byte of 255 is the saturated count and reads "255 or more after it"
    (later_faults_text()).
    With one byte only the text is as before the second byte existed: the byte is read as a
    location and nothing is said about who found it.

    Args:
        detail: the bytes after the event code, at least one.

    Returns:
        The text after the event name, e.g. " at 0x10 (image check result outside its list),
        found by the bootloader, 3 more after it".
    """
    both = len(detail) >= 2
    bootloader = both and bool(detail[0] & FOUND_BY_BOOTLOADER)
    where = detail[0] & ~FOUND_BY_BOOTLOADER if both else detail[0]
    if where in FAULT_LOCATION:
        text = f" at 0x{where:02x} ({FAULT_LOCATION[where][1]})"
    else:
        text = f" at {unknown(where)}"
    if not both:
        return text
    if bootloader:
        return text + f", found by the bootloader, {later_faults_text(detail[1])}"
    return text + ", found by the firmware" + (f", {later_faults_text(detail[1])}" if detail[1] else "")


def decode_event(code, detail):
    """Words for an EVENT report: its name and, where the table gives one, its detail (Sec. 5.4).

    Sec. 5.4 gives a detail to some events: the refused copy and check result of 0x0002 and
    0x0003 (so the operator sees which copy was refused and which check failed), the failed test
    of 0x000B, and the reason of 0x0106. The fault kind of 0x0001 and the fault location of
    0x0005 are decoded with FAULT_KIND and FAULT_LOCATION (fault_codes.h; owner's decisions 2
    and 3, 13 Oct 2026); 0x0005 has a second detail byte and a bit 7, see memory_change_text().
    Each detail byte is turned into the table's words; a value outside the table, or a detail of
    the wrong length, is shown as such, never left out.

    Args:
        code: 16-bit event code.
        detail: the bytes after the event code.

    Returns:
        The text, e.g. "fall-back to the previous copy: slot B refused (signature A)" or "change
        in memory detected at 0x27 (update receiver: image check result outside its list)". An
        event code outside the table gives "unknown code 0x....", a fault kind or location outside
        its table "unknown code 0x..".
    """
    text = name_of(EVENTS, code, 4)
    if code in (EVENT_FALLBACK_PREVIOUS, EVENT_FALLBACK_LAST_RESORT):
        if len(detail) != 2:
            return text + f": detail of {len(detail)} bytes, expected 2"
        copy = detail[0]
        copy_text = f"slot {SLOTS[copy]}" if copy in (0x01, 0x02) else name_of(SLOTS, copy)
        return text + f": {copy_text} refused ({check_result_text(detail[1])})"
    if code == EVENT_FAULT_DETECTED:
        if not detail:
            return text + ": fault kind not given"
        return text + ": " + (FAULT_KIND[detail[0]][1] if detail[0] in FAULT_KIND else unknown(detail[0]))
    if code == EVENT_MEMORY_CHANGE:
        if not detail:
            return text + ": location not given"
        return text + memory_change_text(detail)
    if code == EVENT_TESTS_FAILED:
        return text + ": " + ("test not named" if not detail else name_of(TRIAL_TESTS, detail[0]))
    if code == EVENT_RESTARTED_BY_ONBOARD:
        return text + ": " + ("reason not given" if not detail else name_of(RESTART_REASON, detail[0]))
    return text


def decode_report(payload):
    """Decode a Sec. 5.4 report payload into a dict, or None if it is not a report.

    The onboard computer puts its two added fields (report number, onboard time; 8 bytes) in
    front of every report it forwards (Sec. 5.4, REQ-070, REQ-071). A HEALTH report is a fixed
    length, so with or without the fields is told by length. An EVENT report is of variable
    length, so it is told by the kind byte: at offset 12 with the fields, offset 4 without.
    The event's detail is decoded into the text by decode_event().

    Args:
        payload: the report frame's payload bytes.

    Returns:
        dict with kind "HEALTH" or "EVENT" and the decoded fields, or None.
    """
    out = {}
    offset = ADDED.size
    if len(payload) >= offset + 7 and payload[offset + 4] == REPORT_EVENT:
        out["report_number"], out["onboard_time"] = ADDED.unpack_from(payload)
        payload = payload[offset:]
    elif len(payload) == HEALTH_LEN_ADDED:
        out["report_number"], out["onboard_time"] = ADDED.unpack_from(payload)
        payload = payload[offset:]
    if len(payload) >= 7 and payload[4] == REPORT_EVENT:
        code = struct.unpack_from("<H", payload, 5)[0]
        detail = payload[7:]
        return dict(out, kind="EVENT", code=code, text=decode_event(code, detail), detail=detail)
    if len(payload) != HEALTH_LEN or payload[4] != REPORT_HEALTH:
        return None
    clock, _, mode, fwkind, version, slot, failed, trial_left, selftest = HEALTH_BODY.unpack(payload)
    return dict(out, kind="HEALTH", controller_time=clock, mode=name_of(MODES, mode),
                fw_kind=fwkind, version=version, slot=name_of(SLOTS, slot),
                slot_code=slot, failed_starts=failed, trial_left=trial_left,
                self_test=name_of(SELF_TEST, selftest))


def format_report(r):
    """One decoded report as a single line of text for the operator (Sec. 5.4).

    Args:
        r: report dict from decode_report(), kind "HEALTH" or "EVENT".

    Returns:
        The line: for an EVENT the code and text; for a HEALTH the mode, firmware kind and version,
        slot, failed starts, trial time left in seconds and last self-test. The onboard computer's
        report number and clock time lead the line when the report carries them.
    """
    extra = ""
    if "report_number" in r:
        extra = f"#{r['report_number']} onboard t={r['onboard_time']} "
    if r["kind"] == "EVENT":
        return f"EVENT   {extra}0x{r['code']:04x} {r['text']}"
    return (f"HEALTH  {extra}mode {r['mode']}, firmware kind {r['fw_kind']} version {r['version']}, "
            f"slot {r['slot']}, failed starts {r['failed_starts']}, trial left {r['trial_left']} s, "
            f"self-test {r['self_test']}")


def load_first_seq(path, now=None):
    """Choose the first sequence number of this run (Sec. 4.0, REQ-065).

    The onboard computer refuses an ARM or a dangerous command whose ground sequence number it
    has seen since it started, so a run must never reuse a number of an earlier run. The state
    file holds the last number the previous run used; this run starts one above it.

    Numbering rule when the file is missing or unreadable (absent, empty, not a decimal
    integer, or outside 1 to 2^32-1): start from the clock, seconds since the Unix epoch.
    Why: the clock only grows, so a run that starts later starts higher than the runs
    before it, with no memory needed. The rule is sound only while an earlier run sent fewer
    frames than the seconds that have passed since it started (a frame needs at least a
    round trip, so at the bench rates a run is far below one frame per second on average;
    ASSUMPTION, owner to accept); it also stays inside 2^32-1 until the year 2106.

    Args:
        path: state file path.
        now: current time, seconds since the Unix epoch; the clock if None.

    Returns:
        (first, note): first is the first sequence number to use, 1 to 2^32-1; note is a text
        for the operator when the file could not be used, else None.
    """
    now = int(time.time()) if now is None else int(now)
    try:
        with open(path, "r", encoding="ascii") as f:
            text = f.read().strip()
        last = int(text, 10)
        if not 1 <= last < SEQ_MAX:
            raise ValueError(f"value {last} is outside 1 to {SEQ_MAX - 1}")
        return last + 1, None
    except FileNotFoundError:
        return now, f"no sequence state file {path}: starting from clock time {now}"
    except (OSError, ValueError, UnicodeDecodeError) as e:
        return now, f"sequence state file {path} unreadable ({e}): starting from clock time {now}"


def save_seq(path, seq):
    """Write the last-used sequence number to the state file, atomically (Sec. 4.0, REQ-065).

    A crash must never lead to a reused number, and a half-written file would be read as
    damaged. So the number goes to a temporary file in the same directory, is forced to disk
    (fsync), and then replaces the state file in one rename; the directory is synced as well so
    the rename survives a power loss.

    Args:
        path: state file path; its directory is created if missing.
        seq: the sequence number just taken, 1 to 2^32-1.

    Raises:
        Refused: the file could not be written (a number that is not saved could be reused).
    """
    folder = os.path.dirname(path) or "."
    tmp = f"{path}.tmp{os.getpid()}"
    try:
        os.makedirs(folder, exist_ok=True)
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        try:
            os.write(fd, f"{seq}\n".encode("ascii"))
            os.fsync(fd)
        finally:
            os.close(fd)
        os.replace(tmp, path)
        try:
            dfd = os.open(folder, os.O_RDONLY)
            try:
                os.fsync(dfd)
            finally:
                os.close(dfd)
        except OSError:
            pass                        # directory sync is not supported everywhere; the file is already synced
    except OSError as e:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise Refused(f"cannot save the sequence number to {path}: {e}") from None


class Console:
    """One UDP socket to the onboard computer; stop-and-wait commands; collects reports."""

    def __init__(self, host, port, loss=0.0, timeout=TIMEOUT_S, first_seq=1, local_port=0,
                 quiet=False, rng=None, state_file=None):
        """Open the UDP socket to the onboard computer and set the sequence counter.

        The socket is bound to a local port because the onboard computer reports to the source
        address of the latest datagram it received (Sec. 5.4).

        Args:
            host: onboard computer address.
            port: onboard computer UDP port.
            loss: probability 0..1 that an outgoing datagram is dropped (radio simulator).
            timeout: seconds to wait for an answer before sending a command again.
            first_seq: first sequence number to use, 1 or more (Sec. 4.0, REQ-065).
            local_port: local UDP port to bind; 0 for any.
            quiet: True to print nothing from say().
            rng: random.Random for the radio simulator; a fresh one if None.
            state_file: path where every number taken is saved (save_seq), so the next run
                continues above it; None to keep no state.
        """
        self.state_file = state_file
        self.addr = (host, port)
        self.loss = loss
        self.timeout = timeout
        self.seq = first_seq - 1            # Sec. 4.0: first frame is 1 [REQ-065]
        self.quiet = quiet
        self.rng = rng or random.Random()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("", local_port))    # the onboard computer reports to our source address
        self.dropped = 0
        self.sent = 0
        self.health = None                  # latest HEALTH report
        self.reports = []                   # reports received and not yet consumed

    def say(self, text, end="\n"):
        """Print a progress line unless the console is quiet.

        Args:
            text: the text to print.
            end: string printed after the text; "\\r" keeps a progress line on one row.
        """
        if not self.quiet:
            print(text, end=end, flush=True)

    def next_seq(self):
        """Take the next sequence number (Sec. 4.0: numbers run 1 to 2^32-1, never reused).

        The number is saved to the state file before it is used, so a crash after the frame has
        gone out can never lead to the same number again (the onboard computer would refuse it,
        0x04, REQ-065). A resend of the same command reuses its number and needs no new save.

        Returns:
            The new sequence number.

        Raises:
            Refused: the numbers are used up, or the state file could not be written.
        """
        if self.seq + 1 > SEQ_MAX:
            raise Refused("sequence numbers used up")
        if self.state_file:
            save_seq(self.state_file, self.seq + 1)
        self.seq += 1
        return self.seq

    def transmit(self, frame):
        """Radio simulator, simplest form: drop with probability `loss` before sending."""
        if self.loss > 0 and self.rng.random() < self.loss:
            self.dropped += 1
            return
        self.sock.sendto(frame, self.addr)
        self.sent += 1

    def pump(self, wait):
        """Wait up to `wait` s for one datagram; returns a frame or None. Reports are stored."""
        self.sock.settimeout(max(wait, 0.001))
        try:
            data, _ = self.sock.recvfrom(RECV_BYTES)
        except (socket.timeout, BlockingIOError):
            return None
        frame = parse(data)
        if frame and frame["type"] == TYPE_REPORT:
            report = decode_report(frame["payload"])
            if report:
                self.reports.append(report)
                if report["kind"] == "HEALTH":
                    self.health = report
                self.say("report  " + format_report(report))
            return None
        return frame

    def command(self, dest, payload, timeout=None, what="command"):
        """Send one command and wait for its DONE (or REFUSED). Returns the DONE kind.
        Resends the identical frame (same sequence number) after each timeout."""
        timeout = timeout or self.timeout
        seq = self.next_seq()
        frame = build(TYPE_COMMAND, dest, seq, payload, int(time.time()) + EXPIRY_S)
        for attempt in range(RETRIES + 1):
            self.transmit(frame)
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                answer = self.pump(end - time.monotonic())
                if not answer or answer["type"] != TYPE_ANSWER or len(answer["payload"]) != ANSWER.size:
                    continue
                answered, kind, code = ANSWER.unpack(answer["payload"])
                if answered != seq:
                    continue                # a late duplicate answer to an earlier command
                if kind == RECEIVED:
                    continue
                if kind == REFUSED:
                    text = refusal_text(code)
                    raise Refused(f"{what} (seq {seq}) REFUSED 0x{code:02x}: {text}")
                if kind in (DONE, ALREADY_DONE):
                    return kind
            if attempt < RETRIES:
                self.say(f"no answer to {what} (seq {seq}) within {timeout:g} s, sending again")
        raise Refused(f"{what} (seq {seq}): no answer after {RETRIES + 1} tries")


# ---- Commands ------------------------------------------------------------------------
def upload(con, data, number, start=0):
    """UPLOAD BEGIN, CHUNK..., END (Sec. 5.1). `start` > 0 resumes: BEGIN is skipped.

    When a command is refused or goes unanswered, the error says which stage failed and how to
    go on, because the right restart differs: a failed BEGIN stored nothing; a failed CHUNK
    leaves the chunks before its offset stored but not that chunk itself (an unanswered chunk
    may or may not have been stored); a failed END leaves every byte stored.

    Args:
        con: the Console.
        data: image bytes.
        number: image number, 0 to 65535.
        start: byte offset to resume from, 0 to len(data); 0 sends BEGIN too.

    Raises:
        Refused: a command was refused or never answered; the text carries the resume advice.
    """
    t0 = time.monotonic()
    total = len(data)
    stage = "UPLOAD BEGIN"
    offset = start
    try:
        if start == 0:
            con.command(DEST_ONBOARD, struct.pack("<BHI", UPLOAD_BEGIN, number, total), what="UPLOAD BEGIN")
        while offset < total:
            stage = f"UPLOAD CHUNK offset {offset}"
            chunk = data[offset:offset + CHUNK_MAX]
            con.command(DEST_ONBOARD, struct.pack("<BHI", UPLOAD_CHUNK, number, offset) + chunk, what=stage)
            offset += len(chunk)
            con.say(f"upload  {offset}/{total} bytes ({100 * offset // max(total, 1)} %)   ", end="\r")
        stage = "UPLOAD END"
        con.command(DEST_ONBOARD, struct.pack("<BH", UPLOAD_END, number), what="UPLOAD END")
    except Refused as e:
        again = f"upload IMAGE --number {number}"
        if stage == "UPLOAD BEGIN":
            advice = f"nothing is stored; run `{again}` again from the start (no --offset)"
        elif stage == "UPLOAD END":
            advice = (f"all {total} bytes were accepted; send only the END with `{again} --offset {total}`")
        else:
            advice = (f"bytes before offset {offset} were accepted; the chunk at {offset} is not "
                      f"confirmed (an unanswered chunk may be stored): `{again} --offset {offset}`; "
                      f"if that is refused 0x06, run `{again}` from the start (no --offset)")
        raise Refused(f"{e}; {advice}") from None
    con.say(f"\nupload  complete, {total} bytes in {time.monotonic() - t0:.2f} s")


def deliver(con, number):
    """DELIVER an uploaded image from the onboard computer to the controller (Sec. 5.1, REQ-048).

    The transfer runs over data link A and takes seconds, so the command waits
    DELIVER_TIMEOUT_S, not the normal command timeout. Prints the elapsed time.

    Args:
        con: the Console.
        number: image number, 0 to 65535, as given to UPLOAD BEGIN.

    Raises:
        Refused: DELIVER was refused or never answered.
    """
    t0 = time.monotonic()
    con.command(DEST_ONBOARD, struct.pack("<BH", DELIVER, number), timeout=DELIVER_TIMEOUT_S, what="DELIVER")
    con.say(f"deliver complete in {time.monotonic() - t0:.2f} s")


def arm_and_send(con, command_id, name):
    """Send ARM naming a dangerous controller command and its sequence number, then that command.

    A dangerous command needs an arming command first, and the ARM names the sequence number the
    command will use (Sec. 5.1, 5.2, REQ-029, REQ-112). Both go to the controller (Destination
    0x02); the onboard computer holds the ARM and passes the pair on (Sec. 5.2).

    Args:
        con: the Console.
        command_id: identifier of the dangerous command (SWITCH or CONFIRM).
        name: the command's name, for messages.

    Returns:
        DONE or ALREADY_DONE, the answer kind of the dangerous command.

    Raises:
        Refused: either command was refused or never answered.
    """
    command_seq = con.seq + 2           # ARM takes the next number, the command the one after
    con.command(DEST_CONTROLLER, struct.pack("<BBI", ARM, command_id, command_seq), what="ARM")
    kind = con.command(DEST_CONTROLLER, bytes([command_id]), what=name)
    con.say(f"{name.lower()}  sent, answered {KINDS[kind]}")
    return kind


def arm_and_switch(con):
    """ARM naming SWITCH and the sequence number SWITCH will use, then SWITCH (Sec. 5.1, 5.2).

    Args:
        con: the Console.

    Returns:
        DONE or ALREADY_DONE, the answer kind of SWITCH.
    """
    return arm_and_send(con, SWITCH, "SWITCH")


def arm_and_confirm(con):
    """ARM naming CONFIRM and the sequence number CONFIRM will use, then CONFIRM (Sec. 5.1, 5.2).

    CONFIRM makes the controller keep the firmware it runs on trial; it is dangerous, so it
    needs its ARM like SWITCH does (Sec. 5.1, REQ-012, REQ-020).

    Args:
        con: the Console.

    Returns:
        DONE or ALREADY_DONE, the answer kind of CONFIRM.
    """
    return arm_and_send(con, CONFIRM, "CONFIRM")


def wait_health(con, seconds):
    """Latest HEALTH report within `seconds`, sending GET STATUS first so the onboard computer
    knows where to forward reports (it reports to the source of the latest datagram)."""
    con.health = None
    con.command(DEST_CONTROLLER, bytes([GET_STATUS]), what="GET STATUS")
    end = time.monotonic() + seconds
    while con.health is None and time.monotonic() < end:
        con.pump(end - time.monotonic())
    return con.health


def watch_trial(con, old, free_code, seconds):
    """Watch reports after SWITCH until the trial tests have a result.

    Waits for EVENT 0x0006 trial started (or HEALTH TRIAL on the new slot), then for EVENT
    0x000A trial tests passed or 0x000B trial tests failed (Sec. 5.4, REQ-139, REQ-140). A
    HEALTH report back in OPERATE on the old slot, after the controller left it, means the
    controller gave the new firmware up without a test result. OPERATE on the new slot means
    someone else confirmed it.

    Args:
        con: the Console; its stored reports are consumed.
        old: the HEALTH report taken before SWITCH (names the old slot).
        free_code: slot code of the new firmware's slot.
        seconds: how long to watch, seconds.

    Returns:
        (result, report) with result "passed", "failed", "confirmed", "rolled back" or
        "timeout"; report is the EVENT or HEALTH report that decided it, or the latest HEALTH.
    """
    left = False                        # has the controller left the old state since SWITCH?
    started = False
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        con.pump(min(0.5, end - time.monotonic()))
        while con.reports:
            r = con.reports.pop(0)
            if r["kind"] == "EVENT":
                if r["code"] == EVENT_TRIAL_STARTED:
                    started = True
                    con.say("update  trial started, waiting for the trial tests")
                elif r["code"] == EVENT_TESTS_PASSED:
                    return "passed", r
                elif r["code"] == EVENT_TESTS_FAILED:
                    return "failed", r
                continue
            if r["mode"] == "TRIAL" and r["slot_code"] == free_code and not started:
                started = True
                con.say("update  trial running on the new slot, waiting for the trial tests")
            if r["slot_code"] == free_code and r["mode"] == "OPERATE":
                return "confirmed", r
            if r["mode"] != "OPERATE" or r["slot_code"] != old["slot_code"]:
                left = True
            elif left:
                return "rolled back", r
    return "timeout", con.health


def watch_return(con, old, seconds):
    """Wait for HEALTH OPERATE on the old slot after the controller gave the new firmware up.

    After a failed trial test the controller gives the firmware up at once (EVENT 0x0008) and
    restarts on the previous copy (Sec. 5.4).

    Args:
        con: the Console; its stored reports are consumed.
        old: the HEALTH report taken before SWITCH (names the old slot).
        seconds: how long to wait, seconds.

    Returns:
        The HEALTH report showing OPERATE on the old slot, or None if none came in time.
    """
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        con.pump(min(0.5, end - time.monotonic()))
        while con.reports:
            r = con.reports.pop(0)
            if r["kind"] == "HEALTH" and r["mode"] == "OPERATE" and r["slot_code"] == old["slot_code"]:
                return r
    return None


def read_answer(prompt):
    """Ask the operator a question on the terminal.

    The decision to confirm is the operator's (Sec. 5.1, CONFIRM; REQ-012), so the answer is
    read only from a real terminal. With input piped in, or at end of input, there is no
    operator and no answer.

    Args:
        prompt: the question text.

    Returns:
        The typed line, or None when stdin is not a terminal or input ended.
    """
    if not sys.stdin.isatty():
        return None
    try:
        return input(prompt)
    except EOFError:
        return None


def show_trial_report(con, old):
    """Print the HEALTH report fields the operator judges: mode, slot, version, self-test, trial time left.

    Asks for a fresh HEALTH report first (GET STATUS, Sec. 5.1), so the figures are current.

    Args:
        con: the Console.
        old: the HEALTH report taken before SWITCH, used if no fresh one arrives.

    Returns:
        The HEALTH report shown.
    """
    last = con.health
    h = wait_health(con, HEALTH_WAIT_S) or last or old
    print("TRIAL REPORT")
    print(f"  mode               {h['mode']}")
    print(f"  running slot       {h['slot']}")
    print(f"  firmware version   {h['version']}")
    print(f"  self-test          {h['self_test']}")
    print(f"  trial time left    {h['trial_left']} s")
    return h


def wait_confirmed(con, expect_slot, seconds):
    """Wait for EVENT 0x0007 firmware confirmed, then HEALTH OPERATE (on `expect_slot` if given).

    Args:
        con: the Console; its stored reports are consumed.
        expect_slot: slot code the HEALTH report must show, or None for any slot.
        seconds: how long to wait, seconds.

    Returns:
        (event_seen, health): whether EVENT 0x0007 came, and the HEALTH OPERATE report that
        followed it (None if none came in time).
    """
    seen = False
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        con.pump(min(0.5, end - time.monotonic()))
        while con.reports:
            r = con.reports.pop(0)
            if r["kind"] == "EVENT" and r["code"] == EVENT_CONFIRMED:
                seen = True
            elif (seen and r["kind"] == "HEALTH" and r["mode"] == "OPERATE"
                  and (expect_slot is None or r["slot_code"] == expect_slot)):
                return True, r
    return seen, None


def image_for_free_slot(running_code, image_dir):
    """Pick the image built for the slot the controller does not run (Sec. 5.1, REFUSED 0x0E).

    An image is built for one slot, and the controller writes only the slot it does not run
    (REQ-007), so the free slot decides which file is sent.

    Args:
        running_code: slot code from HEALTH, 0x01 A or 0x02 B.
        image_dir: directory that holds firmware-A.img and firmware-B.img.

    Returns:
        (slot name, image path, slot code) of the free slot.

    Raises:
        Refused: the controller runs the last-resort firmware, so no slot is free.
    """
    if running_code == 0x01:
        return "B", f"{image_dir}/firmware-B.img", 0x02
    if running_code == 0x02:
        return "A", f"{image_dir}/firmware-A.img", 0x01
    raise Refused("controller runs the last-resort firmware: no free slot to update")


def send_confirm(con, expect_slot):
    """Send ARM and CONFIRM, then wait for the controller to say it kept the firmware.

    Success is CONFIRM answered DONE 0x00, EVENT 0x0007, and HEALTH OPERATE (Sec. 5.1, 5.4).
    A refusal such as 0x0F "Trial tests not passed" is raised by the command layer.

    Args:
        con: the Console.
        expect_slot: slot code HEALTH must show afterwards, or None for any slot.

    Returns:
        (event_seen, health) as wait_confirmed() gives them.

    Raises:
        Refused: ARM or CONFIRM was refused or never answered.
    """
    con.reports.clear()                 # only reports from now on count
    arm_and_confirm(con)
    return wait_confirmed(con, expect_slot, CONFIRM_WAIT_S)


def cmd_update(con, args):
    """Whole update: status, upload, deliver, arm, switch, trial, then the operator's decision.

    After SWITCH the controller runs the new firmware on trial and tests it (Sec. 5.4). When the
    tests pass, the console shows the trial report and asks "Confirm? [y/N]"; y sends ARM and
    CONFIRM, anything else sends nothing and the controller returns to the previous copy when
    the trial time ends (REQ-012, REQ-020). A failed test makes the controller give the firmware
    up at once; the console reports which test and waits for the old slot to run again.

    Args:
        con: the Console.
        args: parsed options: image_dir, number, yes (answer y without asking).

    Returns:
        Exit code: 0 confirmed, 1 rolled back or tests failed, 2 unresolved, 3 not confirmed
        by the operator.
    """
    t0 = time.monotonic()
    con.say("update  reading a HEALTH report")
    old = wait_health(con, HEALTH_WAIT_S)
    if old is None:
        raise Refused("no HEALTH report received")
    if old["mode"] == "TRIAL":
        raise Refused("controller is in a trial: no new firmware during a trial (REFUSED 0x07 would follow)")
    name, path, free_code = image_for_free_slot(old["slot_code"], args.image_dir)
    con.say(f"update  running slot {old['slot']} version {old['version']}; free slot {name}, image {path}")
    with open(path, "rb") as f:
        data = f.read()
    upload(con, data, args.number)
    deliver(con, args.number)
    con.reports.clear()                 # only reports from now on count
    arm_and_switch(con)
    result, r = watch_trial(con, old, free_code, UPDATE_WATCH_S)
    elapsed = lambda: time.monotonic() - t0     # noqa: E731
    if result == "confirmed":
        print(f"UPDATE CONFIRMED: slot {r['slot']} version {r['version']}  ({elapsed():.1f} s)")
        return 0
    if result == "rolled back":
        print(f"UPDATE ROLLED BACK: running slot {r['slot']} version {r['version']}  ({elapsed():.1f} s)")
        return 1
    if result == "failed":
        print(f"TRIAL TESTS FAILED: {r['text']}")
        print("update  the controller gives the new firmware up and returns to the previous copy")
        back = watch_return(con, old, TRIAL_MARGIN_S)
        if back:
            print(f"UPDATE REJECTED: running slot {back['slot']} version {back['version']} in "
                  f"{back['mode']}  ({elapsed():.1f} s)")
        else:
            print(f"UPDATE REJECTED: no OPERATE report from the old slot within {TRIAL_MARGIN_S:g} s")
        return 1
    if result == "timeout":
        print(f"UPDATE UNRESOLVED: no trial result or rollback within {UPDATE_WATCH_S:g} s  ({elapsed():.1f} s)")
        return 2
    show_trial_report(con, old)
    if args.yes:
        answer = "y"
        print("Confirm? [y/N] y (--yes)")
    else:
        answer = read_answer("Confirm? [y/N] ")
    if (answer or "").strip().lower() not in ("y", "yes"):
        why = "no answer" if answer is None else "answered N"
        print(f"UPDATE NOT CONFIRMED ({why}): nothing sent. The controller returns to the "
              f"previous copy when its trial time ends.")
        return 3
    seen, health = send_confirm(con, free_code)
    if seen and health:
        print(f"UPDATE CONFIRMED: slot {health['slot']} version {health['version']}  ({elapsed():.1f} s)")
        return 0
    print(f"UPDATE UNRESOLVED: CONFIRM answered DONE but "
          f"{'no HEALTH OPERATE on the new slot' if seen else 'no EVENT 0x0007'} within {CONFIRM_WAIT_S:g} s")
    return 2


def cmd_confirm(con, args):
    """Send ARM and CONFIRM for a controller in a trial, and report the answers and EVENT 0x0007.

    For an operator who answered N to `update`, or confirms with another tool (Sec. 5.1,
    REQ-012). A controller whose trial tests have not passed refuses with 0x0F.

    Args:
        con: the Console.
        args: parsed options (none used).

    Returns:
        Exit code: 0 confirmed, 2 CONFIRM done but the event or HEALTH OPERATE did not follow.
    """
    seen, health = send_confirm(con, None)
    if seen and health:
        print(f"CONFIRMED: EVENT 0x0007 seen; slot {health['slot']} version {health['version']} in OPERATE")
        return 0
    print(f"CONFIRM UNRESOLVED: CONFIRM answered DONE but "
          f"{'no HEALTH OPERATE' if seen else 'no EVENT 0x0007'} within {CONFIRM_WAIT_S:g} s")
    return 2


def cmd_status(con, args):
    """Send GET STATUS and print the reports that arrive for a while (Sec. 5.1, 5.4).

    GET STATUS also tells the onboard computer where to send reports, since it reports to the
    source of the latest datagram.

    Args:
        con: the Console.
        args: parsed options; args.seconds is the listening time, seconds.

    Returns:
        Exit code 0.

    Raises:
        Refused: GET STATUS was refused or never answered.
    """
    con.command(DEST_CONTROLLER, bytes([GET_STATUS]), what="GET STATUS")
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        con.pump(end - time.monotonic())
    return 0


def main(argv=None):
    """Parse the command line, run one subcommand, print FAILED on a refusal.

    The first sequence number is --first-seq if given, else the one after the last number of
    the earlier run, kept in --seq-file (load_first_seq), because the onboard computer refuses
    a ground sequence number it has seen since it started (Sec. 4.0, REQ-065).

    Args:
        argv: argument list, or None for sys.argv.

    Returns:
        The subcommand's exit code (1 after a refusal).
    """
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=DEFAULT_HOST, help="onboard computer address (default %(default)s)")
    p.add_argument("--port", type=int, default=DEFAULT_PORT, help="onboard computer UDP port")
    p.add_argument("--local-port", type=int, default=0, help="local UDP port to bind (0: any)")
    p.add_argument("--radio-loss", type=float, default=0.0, metavar="P",
                   help="drop each outgoing datagram with probability P (0..1), resend after the timeout")
    p.add_argument("--timeout", type=float, default=TIMEOUT_S, help="seconds before a command is sent again")
    p.add_argument("--first-seq", type=int, default=None,
                   help="first sequence number, 1 or more; wins over the state file when given "
                        "(default: continue from the state file, REQ-065)")
    p.add_argument("--seq-file", default=DEFAULT_SEQ_FILE,
                   help="file that keeps the last sequence number between runs (default %(default)s); "
                        "the onboard computer refuses a number it has seen since it started")
    p.add_argument("--seed", type=int, default=None, help="seed of the radio-loss random numbers")
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("status", help="print HEALTH reports for N seconds")
    s.add_argument("--seconds", type=float, default=STATUS_SECONDS)
    s = sub.add_parser("upload", help="UPLOAD BEGIN / CHUNK / END")
    s.add_argument("image")
    s.add_argument("--number", type=int, required=True)
    s.add_argument("--offset", type=int, default=0, help="resume: first byte not yet on the onboard computer")
    s = sub.add_parser("deliver", help="DELIVER an uploaded image to the controller")
    s.add_argument("--number", type=int, required=True)
    sub.add_parser("switch", help="ARM, then SWITCH, both to the controller")
    s = sub.add_parser("update", help="whole update: status, upload, deliver, arm, switch, trial, confirm?")
    s.add_argument("--yes", action="store_true",
                   help="answer y to 'Confirm?' without asking (bench test); otherwise the answer is read from the terminal")
    s.add_argument("--number", type=int, default=1, help="image number (default 1)")
    s.add_argument("--image-dir", default="src/controller/build/images")
    sub.add_parser("confirm", help="ARM, then CONFIRM, both to the controller (new firmware in its trial)")
    a = p.parse_args(argv)
    if not 0.0 <= a.radio_loss <= 1.0:
        p.error("--radio-loss must be between 0 and 1")

    if a.first_seq is not None and not 1 <= a.first_seq <= SEQ_MAX:
        p.error(f"--first-seq must be between 1 and {SEQ_MAX}")
    seq_file = os.path.expanduser(a.seq_file)
    if a.first_seq is not None:
        first = a.first_seq
    else:
        first, note = load_first_seq(seq_file)
        if note:
            print(f"seq     {note}")
    con = Console(a.host, a.port, a.radio_loss, a.timeout, first, a.local_port,
                  rng=random.Random(a.seed), state_file=seq_file)
    try:
        if a.cmd == "status":
            code = cmd_status(con, a)
        elif a.cmd == "upload":
            with open(a.image, "rb") as f:
                upload(con, f.read(), a.number, a.offset)
            code = 0
        elif a.cmd == "deliver":
            deliver(con, a.number)
            code = 0
        elif a.cmd == "switch":
            arm_and_switch(con)
            code = 0
        elif a.cmd == "confirm":
            code = cmd_confirm(con, a)
        else:
            code = cmd_update(con, a)
    except Refused as e:
        print(f"FAILED: {e}")
        code = 1
    finally:
        con.sock.close()
    if a.radio_loss > 0:
        print(f"radio simulator: dropped {con.dropped} of {con.dropped + con.sent} outgoing datagrams")
    return code


if __name__ == "__main__":
    sys.exit(main())
