#!/usr/bin/env python3
"""Apply the SemVer line Copilot printed to SNOVAC_VERSION.

Copilot's stdout must contain one line `VERSION: MAJOR.MINOR.PATCH`.
The number must be greater than the core of SNOVAC_VERSION in driver_utils.h.
Release prose is written later by Gemini, not by this script.
"""

import re
import sys
from pathlib import Path

HEADER = Path("src/driver/driver_utils.h")
VERSION_LINE = re.compile(r"^VERSION:\s*(\d+\.\d+\.\d+)\s*$", re.MULTILINE)
MACRO = re.compile(r'(#define\s+SNOVAC_VERSION\s+")([^"]+)(")')


def core(text):
    match = re.match(r"(\d+)\.(\d+)\.(\d+)", text.strip())
    if not match:
        raise SystemExit(f"current version is not SemVer: {text!r}")
    return tuple(int(part) for part in match.groups())


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: copilot_release_semver.py COPILOT_STDOUT")
    reply = Path(sys.argv[1]).read_text(encoding="utf-8")
    found = VERSION_LINE.search(reply)
    if not found:
        raise SystemExit(
            "Copilot did not print a line `VERSION: MAJOR.MINOR.PATCH`.\n" + reply
        )
    version = found.group(1)
    header = HEADER.read_text(encoding="utf-8")
    macro = MACRO.search(header)
    if not macro:
        raise SystemExit("SNOVAC_VERSION macro not found in src/driver/driver_utils.h")
    current = macro.group(2)
    if core(version) <= core(current):
        raise SystemExit(
            f"Copilot proposed {version}, which is not newer than {current}"
        )
    HEADER.write_text(MACRO.sub(rf"\g<1>{version}\g<3>", header, count=1), encoding="utf-8")
    print(version)


if __name__ == "__main__":
    main()
