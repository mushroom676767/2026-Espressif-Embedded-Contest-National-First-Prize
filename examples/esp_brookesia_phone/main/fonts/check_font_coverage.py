#!/usr/bin/env python3
"""Check that non-ASCII text literals are covered by the bundled CJK font."""

from __future__ import annotations

import argparse
from collections import defaultdict
from pathlib import Path
import re
import sys


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp"}
TEXT_SUFFIXES = {".json", ".txt"}
DEFAULT_SCAN_DIRS = ("main", "components", "spiffs")


def parse_font_symbols(font_path: Path) -> set[str]:
    text = font_path.read_text(encoding="utf-8")
    match = re.search(r"\* Opts: .*?--symbols (.*?) --size ", text)
    if match is None:
        raise ValueError(f"cannot parse --symbols from {font_path}")
    return set(match.group(1))


def iter_c_string_characters(text: str):
    """Yield (character, line) from C/C++ string literals, excluding comments."""
    index = 0
    line = 1
    length = len(text)

    while index < length:
        char = text[index]
        next_char = text[index + 1] if index + 1 < length else ""

        if char == "/" and next_char == "/":
            index += 2
            while index < length and text[index] != "\n":
                index += 1
            continue

        if char == "/" and next_char == "*":
            index += 2
            while index < length:
                if text[index] == "\n":
                    line += 1
                if text[index] == "*" and index + 1 < length and text[index + 1] == "/":
                    index += 2
                    break
                index += 1
            continue

        if char in {"'", '"'}:
            delimiter = char
            is_string = delimiter == '"'
            index += 1
            while index < length:
                char = text[index]
                if char == "\\":
                    if index + 1 < length and text[index + 1] == "\n":
                        line += 1
                    index += 2
                    continue
                if char == delimiter:
                    index += 1
                    break
                if char == "\n":
                    line += 1
                elif is_string:
                    yield char, line
                index += 1
            continue

        if char == "\n":
            line += 1
        index += 1


def iter_text_characters(text: str):
    for line_number, line in enumerate(text.splitlines(), start=1):
        for char in line:
            yield char, line_number


def collect_missing(project_root: Path, font_path: Path, symbols: set[str]):
    missing = defaultdict(list)
    decode_failures = []

    for directory_name in DEFAULT_SCAN_DIRS:
        directory = project_root / directory_name
        if not directory.exists():
            continue
        for path in directory.rglob("*"):
            if not path.is_file() or path.parent == font_path.parent:
                continue
            suffix = path.suffix.lower()
            if suffix not in SOURCE_SUFFIXES and suffix not in TEXT_SUFFIXES:
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError as error:
                decode_failures.append((path, error))
                continue

            iterator = (
                iter_c_string_characters(text)
                if suffix in SOURCE_SUFFIXES
                else iter_text_characters(text)
            )
            for char, line_number in iterator:
                if ord(char) > 0x7F and char not in symbols:
                    location = (path.relative_to(project_root), line_number)
                    if location not in missing[char]:
                        missing[char].append(location)

    return missing, decode_failures


def main() -> int:
    default_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", type=Path, default=default_root)
    parser.add_argument(
        "--font",
        type=Path,
        default=Path("main/fonts/register_cjk_font_20.c"),
    )
    args = parser.parse_args()

    project_root = args.project_root.resolve()
    font_path = args.font
    if not font_path.is_absolute():
        font_path = project_root / font_path
    font_path = font_path.resolve()

    try:
        symbols = parse_font_symbols(font_path)
        missing, decode_failures = collect_missing(project_root, font_path, symbols)
    except (OSError, ValueError) as error:
        print(f"font coverage check failed: {error}", file=sys.stderr)
        return 2

    for path, error in decode_failures:
        print(f"invalid UTF-8: {path.relative_to(project_root)}: {error}", file=sys.stderr)

    for char in sorted(missing, key=ord):
        codepoint = f"U+{ord(char):04X}"
        locations = ", ".join(f"{path}:{line}" for path, line in missing[char])
        print(f"missing glyph {char!r} ({codepoint}): {locations}", file=sys.stderr)

    if decode_failures or missing:
        return 1

    print(f"font coverage OK: {len(symbols)} bundled symbols cover all UI literals")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
