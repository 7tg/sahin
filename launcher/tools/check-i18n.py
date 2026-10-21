#!/usr/bin/env python3
# Sahin - Keeps the table of translations and the code that asks for them in step.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every sentence the code passes to tr(), trs(), trc() or trf() must have a row in
src/core/i18n_strings.cpp, and every row must have its sentence in both languages with the
same {0}, {1}... in each. The arrays the code draws through trc() (kTabs, kGameStages) count
as passed. Run it with no arguments; it exits 1 and says what is wrong."""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TABLE = ROOT / "src" / "core" / "i18n_strings.cpp"
LANGUAGES = ["en", "tr"]
CALLS = {"tr", "trs", "trc", "trf"}


def tokens(text):
    """C++ tokens that matter here: string literals (as bytes), identifiers and punctuation."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif text.startswith("//", i):
            i = text.find("\n", i)
            i = n if i < 0 else i
        elif text.startswith("/*", i):
            i = text.find("*/", i)
            i = n if i < 0 else i + 2
        elif c == '"':
            i += 1
            data = bytearray()
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    i += 1
                    e = text[i]
                    if e == "x":
                        j = i + 1
                        while j < n and text[j] in "0123456789abcdefABCDEF":
                            j += 1
                        data.append(int(text[i + 1 : j], 16) & 0xFF)
                        i = j
                        continue
                    data += {"n": b"\n", "t": b"\t", "\\": b"\\", '"': b'"', "'": b"'"}.get(e, e.encode()) if e in "nt\\\"'" else e.encode()
                else:
                    data += text[i].encode("utf-8")
                i += 1
            i += 1
            out.append(("str", bytes(data).decode("utf-8", "replace")))
        elif c == "'":
            i += 1
            while i < n and text[i] != "'":
                i += 2 if text[i] == "\\" else 1
            i += 1
        elif c.isalpha() or c == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            out.append(("id", text[i:j]))
            i = j
        else:
            out.append(("p", c))
            i += 1
    return out


def literal_groups(toks):
    """The string literals in a run of tokens; neighbours with nothing between them are one.
    What a comparison reads (== "update") is not a sentence."""
    found, current, before = [], None, []
    for kind, value in toks:
        if kind == "str":
            current = value if current is None else current + value
        else:
            if current is not None and before[-2:] not in (["=", "="], ["!", "="]):
                found.append(current)
            if current is not None:
                before = []
            current = None
            if kind == "p":
                before.append(value)
            else:
                before = []
    if current is not None and before[-2:] not in (["=", "="], ["!", "="]):
        found.append(current)
    return found


def table_rows():
    toks = tokens(TABLE.read_text(encoding="utf-8"))
    start = next(i for i, t in enumerate(toks) if t == ("id", "kStrings"))
    rows, depth, columns, current = [], 0, [], None
    for kind, value in toks[start:]:
        if kind == "p" and value == "{":
            depth += 1
            if depth == 3:
                columns, current = [], None
        elif kind == "p" and value == "}":
            if depth == 3:
                if current is not None:
                    columns.append(current)
                rows.append(columns)
            depth -= 1
            if depth == 0:
                break
        elif depth == 3 and kind == "str":
            current = value if current is None else current + value
        elif depth == 3 and kind == "p" and value == ",":
            columns.append("" if current is None else current)
            current = None
    return rows


def call_arguments(toks, at):
    """The top-level arguments of the call whose '(' is toks[at]."""
    args, current, depth = [], [], 0
    for index in range(at, len(toks)):
        kind, value = toks[index]
        if kind == "p" and value in "([{":
            depth += 1
            if depth == 1:
                continue
        elif kind == "p" and value in ")]}":
            depth -= 1
            if depth == 0:
                args.append(current)
                return args
        elif kind == "p" and value == "," and depth == 1:
            args.append(current)
            current = []
            continue
        current.append(toks[index])
    return args


def used_sentences():
    used = {}
    for path in sorted((ROOT / "src").rglob("*")):
        if path.suffix not in (".cpp", ".hpp") or "third_party" in path.parts or path == TABLE:
            continue
        toks = tokens(path.read_text(encoding="utf-8"))
        for at, (kind, value) in enumerate(toks):
            if kind == "id" and value in CALLS and at + 1 < len(toks) and toks[at + 1] == ("p", "("):
                args = call_arguments(toks, at + 1)
                wanted = args[:1]
                for arg in wanted:
                    for sentence in literal_groups(arg):
                        used.setdefault(sentence, path.relative_to(ROOT))
            # The arrays the code draws through trc(): the panel's tabs and the install's stages.
            if kind == "id" and value in ("kTabs", "kGameStages") and toks[at + 1 : at + 3] == [("p", "["), ("p", "]")]:
                depth, group = 0, []
                for later in toks[at + 3 :]:
                    if later == ("p", "{"):
                        depth += 1
                    elif later == ("p", "}"):
                        depth -= 1
                        if depth == 0:
                            break
                    elif later[0] == "str":
                        group.append(later[1])
                for sentence in group:
                    used.setdefault(sentence, path.relative_to(ROOT))
    return used


def placeholders(text):
    return sorted(set(re.findall(r"\{(\d)\}", text)))


def main():
    problems = []
    rows = table_rows()
    english = set()
    for row in rows:
        if len(row) != len(LANGUAGES):
            problems.append(f"a row has {len(row)} columns, not {len(LANGUAGES)}: {row[:1]}")
            continue
        key = row[0]
        if key in english:
            problems.append(f"the sentence is in the table twice: {key!r}")
        english.add(key)
        for language, words in zip(LANGUAGES[1:], row[1:]):
            if not words:
                problems.append(f"no {language} for {key!r}")
            elif placeholders(words) != placeholders(key):
                problems.append(f"{language} has other {{0}}s than the English: {key!r}")
    # The plan's option titles and descriptions reach the page as data (OptionView), so they are
    # demanded here by name.
    plan = json.loads((ROOT / "assets" / "plans" / "BLUR.json").read_text(encoding="utf-8"))
    for option in plan.get("options", []):
        for key in ("title", "description"):
            if option.get(key) and option[key] not in english:
                problems.append(f"assets/plans/BLUR.json: not in the table: {option[key]!r}")
    for sentence, where in sorted(used_sentences().items()):
        if sentence not in english and not re.fullmatch(r"[^A-Za-z]*", sentence):
            problems.append(f"{where}: not in the table: {sentence!r}")
    for problem in problems:
        print("i18n:", problem, file=sys.stderr)
    if problems:
        return 1
    print(f"i18n: {len(rows)} sentences in {len(LANGUAGES)} languages, all the code's sentences in the table")
    return 0


if __name__ == "__main__":
    sys.exit(main())
