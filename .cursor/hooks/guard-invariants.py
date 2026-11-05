#!/usr/bin/env python3
"""Block agent edits that change Snova keyword spellings or delete
type, pointer, async, or pulsar anchors.

The stop hook repeats the same checks on the working tree. It does not
prove a subtle semantic weakening, and it does not prove a bootstrap
binary. Those limits are stated in the rules.
"""

import json
import re
import sys

KEYWORD_FILE = "src/lex/lex_token.c"
REQUIRED_KEYWORDS = frozenset(
    {
        "package",
        "import",
        "class",
        "struct",
        "enum",
        "interface",
        "trait",
        "method",
        "func",
        "static",
        "public",
        "private",
        "protected",
        "override",
        "let",
        "var",
        "const",
        "return",
        "if",
        "else",
        "while",
        "for",
        "in",
        "match",
        "break",
        "continue",
        "try",
        "catch",
        "throw",
        "defer",
        "async",
        "await",
        "pulsar",
        "this",
        "new",
        "as",
        "is",
        "true",
        "false",
        "null",
    }
)

# Presence checks. Deleting one of these strings from its file is the
# weakening this hook can see. Empty bodies that keep the names are not.
ANCHORS = {
    "src/lex/token.h": (
        "SN_TOK_ASYNC",
        "SN_TOK_AWAIT",
        "SN_TOK_PULSAR",
        "SN_TOK_SHL",
        "SN_TOK_SHR",
    ),
    "src/sema/types.h": (
        "SN_T_REF",
        "SN_T_INT",
        "SN_T_LONG",
        "SN_T_INT64",
        "SN_T_CHAR",
        "SN_T_BYTE",
    ),
    "src/sema/borrow.h": (
        "sn_borrow_func",
        "SNOVA_USE_AFTER_MOVE",
        "SNOVA_NULL_DEREF",
        "SNOVA_PULSAR_NON_SEND_CAPTURE",
    ),
    "src/sema/check_ptr.h": (
        "sn_check_ptr_unary",
        "sn_ptr_peel",
        "sn_ptr_adapt_arg",
    ),
    "src/eval/async.h": ("sn_async_spawn", "sn_async_loop_run"),
    "src/eval/async.c": ("sn_async_loop_run",),
    "src/eval/pulsar.h": ("sn_pulsar_pool_create", "sn_pulsar_pool_submit"),
    "src/eval/pulsar.c": ("sn_pulsar_pool_create",),
    "src/eval/rt_ptr.c": ("sn_rt_take_ref",),
    "src/parse/parse_ptr.c": ("SN_TOK_NULL", "SN_EXPR_UNARY"),
}

PROTECTED_FILES = frozenset(ANCHORS) | {KEYWORD_FILE}

KEYWORD_RE = re.compile(r'\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,\s*SN_TOK_')

SYNTAX_MSG = (
    "Do not change Snova syntax. Keyword spellings in "
    "src/lex/lex_token.c stay as they are. Do not translate them."
)
ANCHOR_MSG = (
    "Do not weaken or remove type safety, pointer semantics, "
    "parallelism, or asynchrony. Restore the deleted anchor."
)
ENGLISH_MSG = (
    "Identifiers, docs, and other textual values are written in English. "
    "Do not translate syntax keywords."
)


def emit(payload):
    sys.stdout.write(json.dumps(payload) + "\n")


def allow():
    emit({"permission": "allow"})


def deny(message):
    emit(
        {
            "permission": "deny",
            "user_message": message,
            "agent_message": message,
        }
    )


def rel_path(path):
    if not path or not isinstance(path, str):
        return ""
    norm = path.replace("\\", "/")
    marker = "/src/"
    if "/.cursor/" in norm:
        return ".cursor/" + norm.split("/.cursor/", 1)[1]
    idx = norm.find(marker)
    if idx != -1:
        return norm[idx + 1 :]
    if norm.startswith("src/") or norm.startswith(".cursor/") or norm.startswith("./"):
        return norm[2:] if norm.startswith("./") else norm
    return norm


def load_input():
    raw = sys.stdin.read()
    if not raw.strip():
        return {}
    return json.loads(raw)


def tool_input(payload):
    value = payload.get("tool_input")
    if isinstance(value, str):
        try:
            value = json.loads(value)
        except json.JSONDecodeError:
            return {}
    return value if isinstance(value, dict) else {}


def edited_path(payload, info):
    for key in ("path", "file_path", "filePath", "target_file", "targetFile"):
        found = info.get(key) or payload.get(key)
        if isinstance(found, str):
            return rel_path(found)
    return ""


def added_text(info):
    parts = []
    for key in ("contents", "content", "new_string", "newString"):
        value = info.get(key)
        if isinstance(value, str):
            parts.append(value)
    return "\n".join(parts)


def non_ascii(text):
    return any(ord(ch) > 127 for ch in text)


def read_text(path):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return handle.read()
    except OSError:
        return None


def resulting_text(path, info):
    contents = info.get("contents", info.get("content"))
    if isinstance(contents, str):
        return contents
    old = info.get("old_string", info.get("oldString"))
    new = info.get("new_string", info.get("newString"))
    if not isinstance(old, str) or not isinstance(new, str):
        return None
    current = read_text(path)
    if current is None or old not in current:
        return None
    count = -1 if info.get("replace_all") or info.get("replaceAll") else 1
    return current.replace(old, new, count)


def keyword_spellings(text):
    return frozenset(KEYWORD_RE.findall(text))


def missing_anchors(path, text):
    if text is None:
        return (path + " deleted",)
    required = ANCHORS.get(path)
    if not required:
        return ()
    return tuple(anchor for anchor in required if anchor not in text)


def check_text(path, text):
    if path == KEYWORD_FILE and text is not None:
        found = keyword_spellings(text)
        if found != REQUIRED_KEYWORDS:
            return SYNTAX_MSG
    missing = missing_anchors(path, text)
    if missing:
        return ANCHOR_MSG + " Missing: " + ", ".join(missing) + "."
    return None


def is_delete(tool_name):
    return tool_name.lower() in {"delete", "delete_file"}


def is_edit(tool_name, info):
    if tool_name.lower() in {
        "write",
        "strreplace",
        "search_replace",
        "applypatch",
        "editnotebook",
    }:
        return True
    return any(
        key in info
        for key in ("contents", "content", "old_string", "oldString", "new_string")
    )


def shell_removes_protected(command):
    if not isinstance(command, str):
        return False
    if not re.search(r"(^|[;&|`(])\s*(rm|git\s+rm)\b", command):
        return False
    return any(path in command.replace("\\", "/") for path in PROTECTED_FILES)


def handle_pre_tool(payload):
    info = tool_input(payload)
    tool_name = str(payload.get("tool_name") or "")
    path = edited_path(payload, info)
    if tool_name.lower() == "shell" or "command" in info and not path:
        command = info.get("command", payload.get("command"))
        if shell_removes_protected(command):
            deny(ANCHOR_MSG)
            return
        allow()
        return
    if path.startswith(".cursor/") and non_ascii(added_text(info)):
        deny(ENGLISH_MSG)
        return
    if path not in PROTECTED_FILES:
        allow()
        return
    if is_delete(tool_name):
        deny(check_text(path, None) or ANCHOR_MSG)
        return
    if not is_edit(tool_name, info):
        allow()
        return
    updated = resulting_text(path, info)
    if updated is None:
        # The edit could not be applied to the on-disk file. The stop
        # hook checks the working tree after the edit lands.
        allow()
        return
    problem = check_text(path, updated)
    if problem:
        deny(problem)
        return
    allow()


def handle_stop():
    problems = []
    for path in sorted(PROTECTED_FILES):
        text = read_text(path)
        problem = check_text(path, text)
        if problem:
            problems.append(path + ": " + problem)
    if not problems:
        emit({})
        return
    emit(
        {
            "followup_message": (
                "A snovac invariant failed on the working tree. "
                + " ".join(problems)
                + " Restore the keyword table or the deleted anchor. "
                "Do not translate syntax keywords. A clean tree still "
                "does not prove build/snl; say so if this change ships a binary."
            )
        }
    )


def main():
    payload = load_input()
    event = str(payload.get("hook_event_name") or "")
    if "--stop" in sys.argv or event == "stop":
        handle_stop()
        return
    handle_pre_tool(payload)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        # Fail open. A hook crash must not block unrelated tool calls.
        if "--stop" in sys.argv:
            emit({})
        else:
            allow()
