#!/usr/bin/env python3
"""Write compile_commands.json for clangd from the flags `make` passes in.

Usage (from the Makefile):
  python3 scripts/gen_compile_commands.py ROOT CC CPPFLAGS CFLAGS WARN INCLUDES
"""

import json
import shlex
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 7:
        print(
            "usage: gen_compile_commands.py ROOT CC CPPFLAGS CFLAGS WARN INCLUDES",
            file=sys.stderr,
        )
        return 2
    root = Path(sys.argv[1]).resolve()
    prefix = [sys.argv[2]]
    for group in sys.argv[3:]:
        prefix.extend(shlex.split(group))
    sources = sorted(root.glob("src/**/*.c")) + sorted(root.glob("tests/*.c"))
    entries = []
    for path in sources:
        rel = path.relative_to(root).as_posix()
        entries.append(
            {
                "directory": str(root),
                "file": rel,
                "arguments": prefix + ["-c", rel],
            }
        )
    out = root / "compile_commands.json"
    out.write_text(json.dumps(entries, indent=2) + "\n")
    print(f"wrote {out} ({len(entries)} files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
