#!/usr/bin/env python3
"""Runs clang-tidy over the project's sources in parallel and summarises the findings.

    python tools/run_clang_tidy.py -p build/debug [--clang-tidy PATH] [--jobs N]
                                   [--filter REGEX] [--details CHECK ...]

Uses the compile database of a configured build (CMAKE_EXPORT_COMPILE_COMMANDS is on in
every preset) and the repository's .clang-tidy. Prints the number of findings per check,
then every finding of the checks named with --details. Exit status 1 if any finding is
reported (for CI gating), 0 otherwise. On Windows run it from a Visual Studio developer
prompt, so clang-tidy finds the MSVC headers of the database's cl.exe commands.
"""

import argparse
import collections
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys

FINDING = re.compile(r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): (?:warning|error): (?P<msg>.+) \[(?P<check>[\w\-.,]+)\]$")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-p", "--build", required=True, help="build directory with compile_commands.json")
    parser.add_argument("--clang-tidy", default=shutil.which("clang-tidy") or "clang-tidy")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--filter", default=r"[\\/](src|app)[\\/]", help="regex of source files to check")
    parser.add_argument("--details", nargs="*", default=[], help="checks whose findings are listed")
    parser.add_argument("--all-details", action="store_true", help="list every finding")
    args = parser.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(args.build, "compile_commands.json"), encoding="utf-8") as f:
        database = json.load(f)
    files = sorted({entry["file"] for entry in database
                    if re.search(args.filter, entry["file"]) and "_autogen" not in entry["file"]
                    and not os.path.abspath(entry["file"]).startswith(os.path.abspath(args.build))
                    and entry["file"].endswith(".cpp")})

    def run(file: str) -> str:
        result = subprocess.run([args.clang_tidy, "-p", args.build, "--quiet", file],
                                capture_output=True, text=True, encoding="utf-8", errors="replace")
        return result.stdout

    findings = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for output in pool.map(run, files):
            for line in output.splitlines():
                match = FINDING.match(line.strip())
                if match and os.path.abspath(match["file"]).startswith(root):
                    key = (os.path.relpath(os.path.abspath(match["file"]), root), int(match["line"]),
                           match["check"])
                    findings[key] = match["msg"]  # headers are reported once per includer

    by_check = collections.Counter(check for (_, _, check) in findings)
    print(f"{len(files)} files checked, {len(findings)} findings")
    for check, count in by_check.most_common():
        print(f"{count:6}  {check}")
    for check in (sorted(by_check) if args.all_details else args.details):
        print(f"\n--- {check}")
        for (file, line, found), message in sorted(findings.items()):
            if found == check:
                print(f"{file}:{line}: {message}")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
