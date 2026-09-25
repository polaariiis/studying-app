#!/usr/bin/env python3
"""Checks StudyBoard's module dependency rules at the #include level.

CMake target linkage already prevents most boundary violations (each module has its own
include root and links only its allowed dependencies). This script catches what linkage
cannot, e.g. a Qt-free module including a system-wide <sqlite3.h>, or OpenGL calls
outside render_gl. Rules mirror docs/ARCHITECTURE.md section 2.

Usage: check_boundaries.py [repository-root]      exit code 0 = clean, 1 = violations
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".cc", ".cxx", ".inl"}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
GL_CALL_RE = re.compile(r"\bgl[A-Z][A-Za-z0-9]*\s*\(")
COMMENT_RE = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)


@dataclass(frozen=True)
class Rule:
    modules: frozenset[str]  # studyapp modules this one may include
    qt: bool = False  # may include Qt headers
    sqlite: bool = False  # may include sqlite3.h
    opengl: bool = False  # may include OpenGL headers / call gl* functions
    opengl_widget: bool = False  # may include <QOpenGLWidget> (hosting only, no GL calls)
    notes: list[str] = field(default_factory=list)


QT_FREE_DOMAIN = frozenset({"core"})

RULES: dict[str, Rule] = {
    "core": Rule(frozenset()),
    "document": Rule(QT_FREE_DOMAIN),
    "study": Rule(QT_FREE_DOMAIN),
    "render": Rule(QT_FREE_DOMAIN),
    "canvas": Rule(frozenset({"core", "document", "render"})),
    "persistence": Rule(frozenset({"core", "document", "study"}), sqlite=True),
    "application": Rule(frozenset({"core", "document", "study", "persistence"})),
    "render_gl": Rule(frozenset({"core", "render"}), qt=True, opengl=True),
    "platform": Rule(
        frozenset({"core", "document", "study", "render", "canvas", "application"}), qt=True
    ),
    "ui": Rule(
        frozenset({"core", "document", "study", "render", "canvas", "application", "render_gl"}),
        qt=True,
        opengl_widget=True,
    ),
    # The executable is the composition root and may see every module except persistence.
    "app": Rule(
        frozenset(
            {"core", "document", "study", "render", "canvas", "application", "render_gl",
             "platform", "ui"}
        ),
        qt=True,
    ),
}


def is_qt_header(header: str) -> bool:
    first = header.split("/", 1)[0]
    return (
        bool(re.fullmatch(r"Q[A-Za-z0-9]+", header))  # <QString>
        or bool(re.fullmatch(r"Qt[A-Za-z0-9]*", first)) and "/" in header  # <QtCore/QString>
        or bool(re.fullmatch(r"q[a-z0-9_]+\.h", header))  # <qglobal.h>
    )


def is_opengl_header(header: str) -> bool:
    lowered = header.lower()
    return (
        lowered.startswith(("gl/", "opengl/", "gles", "glad/", "khr/", "epoxy/"))
        or "qopengl" in lowered
        or lowered in {"gl.h", "glew.h", "glext.h"}
    )


def is_sqlite_header(header: str) -> bool:
    return Path(header).name.startswith("sqlite3")


def module_of(path: Path, root: Path) -> str | None:
    rel = path.relative_to(root).parts
    if rel[0] == "app":
        return "app"
    if rel[0] == "src" and len(rel) > 1 and rel[1] in RULES:
        return rel[1]
    return None


def check_file(path: Path, module: str, root: Path) -> list[str]:
    rule = RULES[module]
    text = path.read_text(encoding="utf-8", errors="replace")
    code = COMMENT_RE.sub("", text)
    rel = path.relative_to(root).as_posix()
    problems: list[str] = []

    for header in INCLUDE_RE.findall(code):
        match = re.match(r"studyapp/([A-Za-z0-9_]+)/", header)
        if match:
            target = match.group(1)
            if target != module and target not in rule.modules:
                problems.append(f"{rel}: module '{module}' must not include <{header}>")
            continue
        if is_opengl_header(header):
            allowed = rule.opengl or (rule.opengl_widget and header == "QOpenGLWidget")
            if not allowed:
                problems.append(f"{rel}: OpenGL header <{header}> is only allowed in render_gl")
            continue
        if is_qt_header(header) and not rule.qt:
            problems.append(f"{rel}: Qt header <{header}> in Qt-free module '{module}'")
        if is_sqlite_header(header) and not rule.sqlite:
            problems.append(f"{rel}: SQLite header <{header}> outside persistence")

    if not rule.opengl:
        for call in GL_CALL_RE.findall(code):
            problems.append(f"{rel}: OpenGL call '{call.strip()}' outside render_gl")

    return problems


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent)
    root = root.resolve()
    files = [
        p
        for base in (root / "src", root / "app")
        if base.is_dir()
        for p in sorted(base.rglob("*"))
        if p.suffix in SOURCE_SUFFIXES and p.is_file()
    ]

    problems: list[str] = []
    unknown: set[str] = set()
    for path in files:
        module = module_of(path, root)
        if module is None:
            unknown.add(path.relative_to(root).parts[1] if len(path.relative_to(root).parts) > 1 else str(path))
            continue
        problems.extend(check_file(path, module, root))

    for name in sorted(unknown):
        problems.append(f"src/{name}: directory is not a known module; add it to RULES")

    if problems:
        print("Module boundary violations:")
        for problem in problems:
            print(f"  {problem}")
        return 1

    print(f"Module boundaries OK ({len(files)} files checked).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
