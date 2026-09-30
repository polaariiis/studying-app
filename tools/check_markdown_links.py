#!/usr/bin/env python3
"""Checks the repository's Markdown files for broken relative links and anchors.

    python tools/check_markdown_links.py

Relative links must name an existing file or directory; "#anchor" parts must match a heading
of the target Markdown file (GitHub's slug rules: lower case, spaces to hyphens, punctuation
removed). External (http/https/mailto) links are not fetched. Exit status 1 on any problem.
"""
import re
import subprocess
import sys
from pathlib import Path

LINK = re.compile(r"(?<!\!)\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
CODE_FENCE = re.compile(r"^\s*(```|~~~)")


def slug(heading: str) -> str:
    text = re.sub(r"`([^`]*)`", r"\1", heading)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = text.strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def anchors(path: Path) -> set[str]:
    found: set[str] = set()
    counts: dict[str, int] = {}
    in_code = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if CODE_FENCE.match(line):
            in_code = not in_code
            continue
        if in_code:
            continue
        match = HEADING.match(line)
        if match:
            base = slug(match.group(2))
            n = counts.get(base, 0)
            counts[base] = n + 1
            found.add(base if n == 0 else f"{base}-{n}")
    return found


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    files = subprocess.run(["git", "ls-files", "*.md"], cwd=root, capture_output=True,
                           text=True, check=True).stdout.split()
    files += [f for f in sys.argv[1:] if f not in files]
    problems = 0
    for name in files:
        path = root / name
        if not path.is_file():
            continue
        in_code = False
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if CODE_FENCE.match(line):
                in_code = not in_code
                continue
            if in_code:
                continue
            for target in LINK.findall(line):
                if re.match(r"^[a-z]+:", target):
                    continue
                file_part, _, anchor = target.partition("#")
                resolved = (path.parent / file_part).resolve() if file_part else path
                if not resolved.exists():
                    print(f"{name}:{number}: missing target {target}")
                    problems += 1
                    continue
                if anchor and resolved.suffix == ".md" and anchor not in anchors(resolved):
                    print(f"{name}:{number}: no heading for #{anchor} in {file_part or name}")
                    problems += 1
    print(f"{len(files)} Markdown files checked, {problems} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
