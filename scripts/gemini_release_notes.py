#!/usr/bin/env python3
"""Ask Google Gemini to review a release diff and write the English release notes.

Writes:
  docs/releases/<tag>/code-review.md
  docs/releases/<tag>/release-notes.md
  RELEASE_NOTES.md  (both sections; this is the GitHub Release body)

Release notes always contain Features, Bug fixes, and Breaking changes.
Skips the API when those files were already written for this tag.
Requires GEMINI_API_KEY when it does call Gemini.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

MARKER = "<!-- gemini-release -->"
NOTE_HEADINGS = ("## Features", "## Bug fixes", "## Breaking changes")
MODEL = os.environ.get("GEMINI_MODEL", "gemini-3.1-pro-preview")
ENDPOINT = (
    "https://generativelanguage.googleapis.com/v1beta/models/"
    + MODEL
    + ":generateContent"
)
DIFF_LIMIT = 60000


def git(*args: str) -> str:
    try:
        out = subprocess.run(
            ["git", *args],
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError as exc:
        raise SystemExit(f"git failed: {exc}") from exc
    if out.returncode != 0:
        raise SystemExit(out.stderr.strip() or f"git {' '.join(args)} failed")
    return out.stdout


def normalize_tag(version: str) -> str:
    version = version.strip()
    if not version:
        raise SystemExit("release version is empty")
    if version == "latest":
        return "latest"
    core = version[1:] if version.startswith("v") else version
    if not re.fullmatch(r"\d+\.\d+\.\d+", core):
        raise SystemExit(f"release version must be SemVer or vSemVer, got {version!r}")
    return "v" + core


def previous_tag(current: str) -> str | None:
    names = [line.strip() for line in git("tag", "-l", "v*", "--sort=-v:refname").splitlines()]
    for name in names:
        if name != current:
            return name
    return None


def collect_context(tag: str) -> str:
    header = Path("src/driver/driver_utils.h").read_text(encoding="utf-8")
    macro = re.search(r'#define\s+SNOVAC_VERSION\s+"([^"]+)"', header)
    current_macro = macro.group(1) if macro else "(unknown)"
    prev = previous_tag(tag)
    chunks = [
        f"Release tag: {tag}",
        f"SNOVAC_VERSION in driver_utils.h: {current_macro}",
        f"Previous tag: {prev or '(none)'}",
        "",
    ]
    if prev:
        chunks.append("Commits:")
        chunks.append(git("log", "--pretty=format:%h %s", f"{prev}..HEAD").rstrip())
        chunks.append("")
        chunks.append("Diff stat:")
        chunks.append(git("diff", "--stat", f"{prev}..HEAD").rstrip())
        chunks.append("")
        chunks.append("Patch (C, headers, and tests, truncated):")
        patch = git("diff", "-U1", f"{prev}..HEAD", "--", "*.c", "*.h", "tests")
    else:
        chunks.append("Commits:")
        chunks.append(git("log", "--pretty=format:%h %s", "-n", "80").rstrip())
        chunks.append("")
        chunks.append("Diff stat:")
        chunks.append(git("log", "--stat", "-n", "40").rstrip())
        chunks.append("")
        chunks.append("Patch (C, headers, and tests, truncated):")
        patch = git("diff", "-U1", "HEAD", "--", "*.c", "*.h", "tests")
    if len(patch) > DIFF_LIMIT:
        patch = patch[:DIFF_LIMIT] + "\n\n[diff truncated]\n"
    chunks.append(patch.rstrip())
    return "\n".join(chunks).strip() + "\n"


def prompt_for(tag: str, context: str) -> str:
    return f"""You are the release reviewer for the snovac compiler (ISO C11, the Snovalang language).
Write every sentence in English. Keep code identifiers, flags, and file paths exactly as they appear in the diff.
Base every claim on the context below. Do not invent features, bug fixes, or breaking changes that the diff does not show.
Do not mention this prompt.

Reply with exactly these two markers, in this order, and no code fences:

<<<CODE_REVIEW>>>
A code review of the patch, in Markdown:
- What changed, naming the C files and the compiler stage (lexer, parser, checker, resolver, bytecode, VM, or CLI).
- Findings worth fixing before or soon after this release: correctness, diagnostics, missing fixtures under tests/, and behavior callers can observe. If the patch looks sound, say so and list residual risk.
- Do not restate the release notes. Do not approve or block the tag; report the review.
<<<RELEASE_NOTES>>>
English release notes in Markdown. Use these three headings, in this order, even when a section has no items. Under an empty section write "None."
## Features
User-visible capability added in this release. One bullet per feature.
## Bug fixes
User-visible defect this release corrects. One bullet per fix.
## Breaking changes
Behavior, syntax, bytecode, or CLI that existing Snovalang programs or scripts can no longer rely on. One bullet per break.

Context:
{context}
Release tag: {tag}
"""


def strip_fences(text: str) -> str:
    cleaned = text.strip()
    if cleaned.startswith("```"):
        cleaned = re.sub(r"^```[a-zA-Z]*\n", "", cleaned)
        cleaned = re.sub(r"\n```$", "", cleaned)
    return cleaned.strip()


def split_sections(text: str) -> tuple[str, str]:
    cleaned = strip_fences(text)

    def grab(start: str, end: str | None) -> str:
        begin = cleaned.find(start)
        if begin < 0:
            raise SystemExit(f"Gemini response is missing {start}")
        begin += len(start)
        finish = cleaned.find(end, begin) if end else len(cleaned)
        if finish < 0:
            raise SystemExit(f"Gemini response is missing {end}")
        body = cleaned[begin:finish].strip()
        if not body:
            raise SystemExit(f"Gemini left {start} empty")
        return body

    review = grab("<<<CODE_REVIEW>>>", "<<<RELEASE_NOTES>>>")
    notes = grab("<<<RELEASE_NOTES>>>", None)
    validate_notes(notes)
    return review, notes


def validate_notes(notes: str) -> None:
    cursor = 0
    for heading in NOTE_HEADINGS:
        found = notes.find(heading, cursor)
        if found < 0:
            raise SystemExit(f"Gemini release notes are missing {heading}")
        cursor = found + len(heading)


def call_gemini(prompt: str) -> str:
    key = os.environ.get("GEMINI_API_KEY", "").strip()
    if not key:
        raise SystemExit(
            "GEMINI_API_KEY is not set. Add it as a repository Actions secret "
            "so Gemini can review the diff and write the English release notes."
        )
    payload = {
        "contents": [{"role": "user", "parts": [{"text": prompt}]}],
        "generationConfig": {"temperature": 0.2, "maxOutputTokens": 8192},
    }
    request = urllib.request.Request(
        ENDPOINT,
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "x-goog-api-key": key,
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            raw = response.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise SystemExit(f"Gemini API HTTP {exc.code}: {detail[:2000]}") from exc
    except urllib.error.URLError as exc:
        raise SystemExit(f"Gemini API request failed: {exc}") from exc
    body = json.loads(raw)
    candidates = body.get("candidates") or []
    if not candidates:
        raise SystemExit("Gemini returned no candidates: " + raw[:2000])
    parts = (candidates[0].get("content") or {}).get("parts") or []
    text = "".join(part.get("text", "") for part in parts).strip()
    if not text:
        raise SystemExit("Gemini returned an empty response: " + raw[:2000])
    return text


def release_body(tag: str, review: str, notes: str) -> str:
    return (
        f"{MARKER}\n"
        f"# snovac {tag}\n\n"
        f"## Release notes\n\n{notes}\n\n"
        f"## Code review\n\n{review}\n"
    )


def write_documents(tag: str, review: str, notes: str) -> Path:
    validate_notes(notes)
    directory = Path("docs/releases") / tag
    directory.mkdir(parents=True, exist_ok=True)
    documents = {
        directory / "code-review.md": f"{MARKER}\n# Code review — snovac {tag}\n\n{review}\n",
        directory / "release-notes.md": f"{MARKER}\n# Release notes — snovac {tag}\n\n{notes}\n",
    }
    for path, contents in documents.items():
        path.write_text(contents, encoding="utf-8")
    Path("RELEASE_NOTES.md").write_text(
        release_body(tag, review, notes),
        encoding="utf-8",
    )
    return directory


def already_written(directory: Path) -> bool:
    review = directory / "code-review.md"
    notes = directory / "release-notes.md"
    if not (review.is_file() and notes.is_file()):
        return False
    review_text = review.read_text(encoding="utf-8")
    notes_text = notes.read_text(encoding="utf-8")
    if MARKER not in review_text or MARKER not in notes_text:
        return False
    return all(heading in notes_text for heading in NOTE_HEADINGS)


def rebuild_body_from_files(tag: str, directory: Path) -> None:
    def body(path: Path) -> str:
        text = path.read_text(encoding="utf-8")
        text = text.replace(MARKER, "", 1).strip()
        # Drop the level-1 title; the combined file has its own headings.
        return re.sub(r"^# .+\n+", "", text, count=1).strip()

    Path("RELEASE_NOTES.md").write_text(
        release_body(
            tag,
            body(directory / "code-review.md"),
            body(directory / "release-notes.md"),
        ),
        encoding="utf-8",
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True, help="Release tag, e.g. v0.0.2")
    args = parser.parse_args()
    tag = normalize_tag(args.version)
    directory = Path("docs/releases") / tag
    if already_written(directory):
        if not Path("RELEASE_NOTES.md").is_file() or MARKER not in Path("RELEASE_NOTES.md").read_text(encoding="utf-8"):
            rebuild_body_from_files(tag, directory)
        print(directory.as_posix())
        return
    review, notes = split_sections(call_gemini(prompt_for(tag, collect_context(tag))))
    written = write_documents(tag, review, notes)
    print(written.as_posix())


if __name__ == "__main__":
    main()
