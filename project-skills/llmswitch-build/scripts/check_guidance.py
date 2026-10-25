#!/usr/bin/env python3
"""Check repository-owned guidance size limits using only the standard library."""

import argparse
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[3],
        help="repository root (defaults to this script's repository)",
    )
    root = parser.parse_args().root.resolve()
    skills_dir = root / "project-skills"
    skills = sorted(skills_dir.rglob("SKILL.md"))
    if not skills:
        print("FAIL: project-skills/ contains no SKILL.md")
        return 1

    failed = False
    for path in [root / "AGENTS.md", *skills]:
        label = path.relative_to(root)
        try:
            content = path.read_bytes().decode("utf-8")
        except (OSError, UnicodeError) as error:
            print(f"FAIL: {label}: {error}")
            failed = True
            continue
        # Match physical LF-delimited lines, including a final unterminated line.
        line_count = content.count("\n") + int(bool(content) and not content.endswith("\n"))
        char_count = len(content)
        is_root = path == root / "AGENTS.md"
        line_limit = 150 if is_root else 100
        valid = line_count <= line_limit and (not is_root or char_count <= 6000)
        limits = f"{line_limit} lines" + (", 6000 characters" if is_root else "")
        print(
            f"{'PASS' if valid else 'FAIL'}: {label}: "
            f"{line_count} lines, {char_count} characters (limits: {limits})"
        )
        failed |= not valid
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
