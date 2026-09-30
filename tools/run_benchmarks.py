#!/usr/bin/env python3
"""Runs StudyBoard's benchmark suites and stores their JSON results with a record of the
machine (docs/BENCHMARKS.md).

    python tools/run_benchmarks.py --build build/ci-full [--label NAME] [--out DIR] [--filter REGEX]

Writes into DIR (default: bench/results/<date>-<label>/): canvas.json (studyapp_benchmarks,
without the stress series), stress-canvas.json (the stress series), app.json
(studyapp_app_benchmarks, when it was built) and machine.json. The GPU and display are not
detected portably: add them to machine.json by hand (docs/HARDWARE_MATRIX.md). The
computer's host name is not recorded (Google Benchmark's host_name is replaced by the
label), so results can be published.
"""
import argparse
import datetime
import json
import os
import platform
import subprocess
import sys
from pathlib import Path


def executable(build: Path, name: str) -> Path | None:
    for candidate in (build / "bench" / name, build / "bench" / f"{name}.exe"):
        if candidate.is_file():
            return candidate
    return None


def run(binary: Path, output: Path, benchmark_filter: str | None, env: dict) -> int:
    command = [str(binary), f"--benchmark_out={output}", "--benchmark_out_format=json"]
    if benchmark_filter:
        command.append(f"--benchmark_filter={benchmark_filter}")
    print("running:", " ".join(command), flush=True)
    return subprocess.run(command, env=env, check=False).returncode


def replace_host_name(result: Path, label: str) -> None:
    """Google Benchmark records the host name; keep only the chosen label."""
    try:
        data = json.loads(result.read_text(encoding="utf-8"))
    except (OSError, ValueError):  # not written, or empty when a filter matched nothing
        return
    data.get("context", {})["host_name"] = label
    result.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def machine(label: str) -> dict:
    info = {
        "date": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "label": label,
        "os": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "logical_cpus": os.cpu_count(),
        "gpu": "fill in (docs/HARDWARE_MATRIX.md)",
        "display": "fill in",
    }
    try:
        commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True,
                                text=True, check=False).stdout.strip()
        info["commit"] = commit or "unknown"
    except OSError:
        info["commit"] = "unknown"
    return info


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", type=Path, required=True,
                        help="build directory with benchmarks (e.g. build/ci-full)")
    parser.add_argument("--label", default="local",
                        help="machine label recorded instead of the host name (default: local)")
    parser.add_argument("--out", type=Path, help="result directory")
    parser.add_argument("--filter", help="only benchmarks matching this regex")
    args = parser.parse_args()

    stamp = datetime.date.today().isoformat()
    out = args.out or Path("bench/results") / f"{stamp}-{args.label}"
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.setdefault("QT_QPA_PLATFORM", "offscreen")  # the application benchmarks need no window

    failures = 0
    canvas = executable(args.build, "studyapp_benchmarks")
    if canvas is None:
        print(f"no studyapp_benchmarks in {args.build}/bench (configure with benchmarks on)",
              file=sys.stderr)
        return 2
    if args.filter:
        failures += run(canvas, out / "canvas.json", args.filter, env) != 0
    else:
        failures += run(canvas, out / "canvas.json", "-BM_Stress", env) != 0
        failures += run(canvas, out / "stress-canvas.json", "BM_Stress", env) != 0
    app = executable(args.build, "studyapp_app_benchmarks")
    if app is not None:
        failures += run(app, out / "app.json", args.filter, env) != 0
    else:
        print("studyapp_app_benchmarks not built (app off): skipped")
    for result in ("canvas.json", "stress-canvas.json", "app.json"):
        replace_host_name(out / result, args.label)
    (out / "machine.json").write_text(json.dumps(machine(args.label), indent=2) + "\n",
                                      encoding="utf-8")
    print(f"results in {out}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
