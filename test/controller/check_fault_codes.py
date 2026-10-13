"""check_fault_codes.py — every fault site reports its own fault-location code (decision 3).

Owner's decision 3, 13 Oct 2026: EVENT 0x0005 carries a 1-byte fault location, one code per
function that contains a fault site, from the table in src/controller/common/fault_codes.h. This
check reads the controller's flight sources and fails unless:
  1. every call of the fault reaction (fault_impossible_value, faultImpossibleValue) passes a
     FAULT_AT_ code of the table as its first argument, except the one forwarder in
     firmware/main.cpp, which passes its own parameter on;
  2. the code's name is FAULT_AT_ and the enclosing function's name in capitals, so each site
     names its own function (two sites in one function share its code; two functions never do);
  3. the code lies in the range of the directory the function is in: bootloader/ 0x01-0x0F,
     common/ 0x10-0x1F, firmware/ 0x20-0x4F;
  4. no two codes of the table have the same value, and every code is used by a site.
Why a source check: most fault sites are reached only by a corrupted value, so no run of the
programs reaches them all; the host unit tests drive many of them and check their codes too.

Usage: check_fault_codes.py <src/controller directory>. Prints PASS or FAIL per check; exit
status 1 on a failure.
"""
import pathlib
import re
import sys

CALL = re.compile(r"\b(fault_impossible_value|faultImpossibleValue)\s*\(")
DEFINE = re.compile(r"#define\s+(FAULT_AT_\w+)\s+(0x[0-9A-Fa-f]+)U\b")
HEADER = re.compile(r"^[A-Za-z_][^;=]*?\b([A-Za-z_~][\w:~]*)\s*\(")
NOT_HEADER = ("#", "static_assert", "typedef", "struct", "class", "enum", "namespace", "template",
              "return", "using")
REACTIONS = ("fault_impossible_value", "faultImpossibleValue")
RANGES = {"bootloader": (0x01, 0x0F), "common": (0x10, 0x1F), "firmware": (0x20, 0x4F)}
SUFFIXES = (".c", ".h", ".cpp", ".hpp")


def strip_code(text):
    """Blanks comments and the contents of string and character literals, keeping line breaks.

    Why: a comment that names the fault reaction ("calls fault_impossible_value (common/fault.h)")
    is not a call, and a string may hold any text.

    Args:
        text: C or C++ source text.

    Returns:
        The text, same length and lines, with comments and literal contents replaced by spaces.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text[i] in "\"'":
            quote = text[i]
            j = i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            out.append(quote + " " * (min(j, n) - i - 1) + (quote if j < n else ""))
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def first_argument(code, open_paren):
    """Returns the first argument of the call whose "(" is at open_paren.

    Args:
        code: stripped source text.
        open_paren: index of the call's opening parenthesis.

    Returns:
        The first argument, without surrounding white space.
    """
    depth = 0
    for k in range(open_paren, len(code)):
        if code[k] == "(":
            depth += 1
        elif code[k] == ")":
            depth -= 1
            if depth == 0:
                return code[open_paren + 1:k].strip()
        elif code[k] == "," and depth == 1:
            return code[open_paren + 1:k].strip()
    return ""


def enclosing_function(lines, line_index):
    """Returns the name of the function definition that contains a line.

    Function definitions in the controller sources start at column 0 and their bodies are
    indented (the code-rules hook finds them the same way), so the nearest line above that starts
    at column 0 with a name and "(" is the enclosing function's head.

    Args:
        lines: the stripped source, split into lines.
        line_index: index of the line with the call.

    Returns:
        The function's name as written, with its class ("ArmState::checkIntact"), or "" if none.
    """
    for k in range(line_index, -1, -1):
        line = lines[k]
        if not line or not (line[0].isalpha() or line[0] == "_"):
            continue
        if line.startswith(NOT_HEADER):
            continue
        match = HEADER.match(line)
        if match:
            return match.group(1)
    return ""


def code_name(function):
    """Returns the FAULT_AT_ name a function's code must have.

    Args:
        function: the function's name, e.g. "UpdateReceiver::slotStart" or "slot_letter".

    Returns:
        "FAULT_AT_" and the name in capitals, words split at "::" and at each capital letter,
        e.g. "FAULT_AT_UPDATE_RECEIVER_SLOT_START".
    """
    words = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", function.replace("::", "_"))
    return "FAULT_AT_" + words.upper()


def read_table(header):
    """Reads the FAULT_AT_ codes of fault_codes.h.

    Args:
        header: path of src/controller/common/fault_codes.h.

    Returns:
        A dict from each FAULT_AT_ name to its value.
    """
    return {name: int(value, 16) for name, value in DEFINE.findall(header.read_text())}


def find_sites(src):
    """Finds every call of the fault reaction in the flight sources.

    Declarations and definitions of the reaction itself (a "void" before the name on its line)
    are not calls.

    Args:
        src: the src/controller directory.

    Returns:
        A list of (file path relative to src, line number, enclosing function, first argument).
    """
    sites = []
    for path in sorted(src.rglob("*")):
        if path.suffix not in SUFFIXES or "build" in path.parts or not path.is_file():
            continue
        code = strip_code(path.read_text())
        lines = code.split("\n")
        for match in CALL.finditer(code):
            line_index = code.count("\n", 0, match.start())
            line_start = code.rfind("\n", 0, match.start()) + 1
            if re.search(r"\bvoid\s*$", code[line_start:match.start()]):
                continue
            argument = first_argument(code, match.end() - 1)
            function = enclosing_function(lines, line_index)
            sites.append((path.relative_to(src), line_index + 1, function, argument))
    return sites


def check(src):
    """Runs the four checks of the module docstring and prints a PASS or FAIL line for each.

    Args:
        src: the src/controller directory.

    Returns:
        The number of failed checks.
    """
    table = read_table(src / "common" / "fault_codes.h")
    sites = find_sites(src)
    problems = {1: [], 2: [], 3: [], 4: []}
    used = set()
    for path, line, function, argument in sites:
        where = f"{path}:{line} in {function or '?'}"
        if function in REACTIONS and argument == "location":
            continue   # the forwarder in firmware/main.cpp passes its own parameter on
        if argument not in table:
            problems[1].append(f"{where}: first argument '{argument}' is no code of the table")
            continue
        used.add(argument)
        if argument != code_name(function):
            problems[2].append(f"{where}: passes {argument}, its function's code is "
                               f"{code_name(function)}")
        low, high = RANGES.get(path.parts[0], (1, 0))
        if not low <= table[argument] <= high:
            problems[3].append(f"{where}: {argument} = 0x{table[argument]:02X} is outside "
                               f"{path.parts[0]}/'s range")
    values = {}
    for name, value in table.items():
        if value in values:
            problems[4].append(f"{name} and {values[value]} share 0x{value:02X}")
        values[value] = name
        if name not in used:
            problems[4].append(f"{name} (0x{value:02X}) is used by no fault site")
    names = {1: "every fault site passes a FAULT_AT_ code of common/fault_codes.h",
             2: "each site passes its own function's code (one code per function)",
             3: "each code lies in its directory's range (bootloader 0x01-0x0F, common "
                "0x10-0x1F, firmware 0x20-0x4F)",
             4: "no two codes share a value, and every code is used"}
    for number, name in names.items():
        print(f"{'PASS' if not problems[number] else 'FAIL'}  {name}")
        for problem in problems[number]:
            print(f"      {problem}")
    functions = {function for _, _, function, _ in sites if function not in REACTIONS}
    print(f"      ({len(sites)} calls found: {len(functions)} functions, {len(table)} codes)")
    return sum(1 for p in problems.values() if p)


def main():
    """Runs the check on the directory given on the command line.

    Returns:
        0 if every check passed, 1 if one failed, 2 on a usage error.
    """
    if len(sys.argv) != 2:
        print("usage: check_fault_codes.py <src/controller directory>")
        return 2
    failed = check(pathlib.Path(sys.argv[1]))
    if failed == 0:
        print("fault-location codes: all 4 checks passed")
        return 0
    print(f"fault-location codes: {failed} of 4 checks FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
