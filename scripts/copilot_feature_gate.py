#!/usr/bin/env python3
"""Copilot hook: a compiler or test edit is unfinished until `make test` passed."""

import json
import subprocess
import sys
from pathlib import Path

MARKER = Path(".copilot-feature-verified")
CONTEXT = (
    "snovac: a new or changed compiler feature is unfinished until it has a "
    "fixture under tests/ (compile-pass, compile-fail, or run-pass) or an "
    "assertion in tests/test_*.c, and `make test` has exited 0 in this "
    "session. Do not edit SNOVAC_VERSION. SemVer is chosen only by the "
    "manual GitHub Actions workflow \"Release with Copilot semver\"."
)


def load_event():
    raw = sys.stdin.read()
    if not raw.strip():
        return {}
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        return {}


def emit(payload):
    json.dump(payload, sys.stdout)
    sys.stdout.write("\n")


def blob(value):
    if value is None:
        return ""
    if isinstance(value, str):
        return value
    try:
        return json.dumps(value)
    except TypeError:
        return str(value)


def command_text(event):
    args = event.get("toolArgs")
    if args is None:
        args = event.get("tool_input")
    return blob(args)


def is_test_command(text):
    folded = text.replace("\n", " ")
    needles = (
        "make test",
        "make unit",
        "tests/battery.sh",
        "tests/run.sh",
    )
    return any(needle in folded for needle in needles)


def git_names():
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--is-inside-work-tree"],
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError:
        return []
    if out.returncode != 0:
        return []
    names = []
    commands = (
        ["git", "diff", "--name-only", "HEAD"],
        ["git", "diff", "--name-only", "--cached"],
        ["git", "ls-files", "--others", "--exclude-standard"],
    )
    for command in commands:
        listed = subprocess.run(command, check=False, capture_output=True, text=True)
        if listed.returncode == 0:
            names.extend(line.strip() for line in listed.stdout.splitlines() if line.strip())
    return names


def needs_verification(path):
    if path == MARKER.name or path.endswith("/" + MARKER.name):
        return False
    if path.startswith(".github/"):
        return False
    if path.startswith("tests/"):
        return True
    return path.endswith(".c") or path.endswith(".h")


def mark(event):
    if is_test_command(command_text(event)):
        MARKER.write_text("ok\n", encoding="utf-8")
    emit({})


def stop(event):
    if event.get("stop_hook_active") is True and MARKER.is_file():
        emit({"decision": "allow"})
        return
    changed = [path for path in git_names() if needs_verification(path)]
    if not changed:
        emit({"decision": "allow"})
        return
    if MARKER.is_file():
        emit({"decision": "allow"})
        return
    preview = ", ".join(changed[:8])
    emit(
        {
            "decision": "block",
            "reason": (
                "snovac feature gate: these files changed and `make test` has "
                f"not exited 0 in this session: {preview}. "
                "Add a fixture under tests/compile-pass, tests/compile-fail, "
                "tests/run-pass, or tests/test_*.c when the change is a new "
                "behavior, then run `make test` from the repository root and "
                "fix every failure before stopping."
            ),
        }
    )


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    event = load_event()
    if mode == "start":
        MARKER.unlink(missing_ok=True)
        emit({"additionalContext": CONTEXT})
        return
    if mode == "mark":
        mark(event)
        return
    if mode == "stop":
        stop(event)
        return
    emit({})


if __name__ == "__main__":
    main()
