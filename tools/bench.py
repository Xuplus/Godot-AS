#!/usr/bin/env python
"""Runs demo/bench/run_bench.tscn several times and reports the best time of each benchmark.

    python tools/bench.py                                   # editor build, in demo/
    python tools/bench.py --exe build/windows/godot_as_demo.exe --save docs/bench/windows-release.json
    python tools/bench.py --compare docs/bench/windows-release.json --exe ...   # before/after table

Web and Android print to the browser console / logcat: save that output and pass it with --log
(every BENCH_JSON line in it counts as one run).
    python tools/bench.py --log console.txt --save docs/bench/web-release.json

An exported game always runs its main scene, so export one with run_bench as main scene first:
    python tools/export_demo.py --scene res://bench/run_bench.tscn "Windows Desktop"

Godot is found through the GODOT environment variable (default: godot on PATH).
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run_once(cmd):
    with tempfile.TemporaryDirectory() as tmp:
        log = os.path.join(tmp, "bench.log")
        subprocess.run(cmd + ["--log-file", log], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=900)
        for line in open(log, encoding="utf-8", errors="replace"):
            if line.startswith("BENCH_JSON "):
                return json.loads(line[len("BENCH_JSON "):])
    raise RuntimeError("no BENCH_JSON line in the output of: " + " ".join(cmd))


def best_of(runs):
    """Merges runs, keeping the lowest time of each benchmark and language."""
    merged = dict(runs[0])
    merged["results"] = {}
    for name in runs[0]["results"]:
        merged["results"][name] = {lang: min(r["results"][name][lang] for r in runs) for lang in ("as", "gd")}
    merged["runs"] = len(runs)
    return merged


def ratio(gd, as_):
    return "%.2fx" % (gd / as_) if as_ else "-"


def table(data, before=None):
    print("%s %s, %s build, Godot %s, best of %d runs" % (data["platform"], data["arch"], data["build"], data["godot"], data["runs"]))
    print()
    if before:
        print("| Benchmark | AS before | AS now | AS speedup | GD | GD/AS before | GD/AS now |")
        print("|---|---:|---:|---:|---:|---:|---:|")
    else:
        print("| Benchmark | AngelScript | GDScript | GD/AS |")
        print("|---|---:|---:|---:|")
    for name, r in data["results"].items():
        if before and name in before["results"]:
            b = before["results"][name]
            print("| %s | %.2f | %.2f | %s | %.2f | %s | %s |" % (name, b["as"], r["as"], ratio(b["as"], r["as"]), r["gd"], ratio(b["gd"], b["as"]), ratio(r["gd"], r["as"])))
        else:
            print("| %s | %.2f | %.2f | %s |" % (name, r["as"], r["gd"], ratio(r["gd"], r["as"])))
    print()
    print("Times in ms (the _process row in ms per frame). GD/AS above 1 means AngelScript is faster.")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", help="an exported game with run_bench as main scene (default: the editor build on demo/)")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--log", help="read the runs from BENCH_JSON lines in this file instead of running Godot")
    parser.add_argument("--save", help="write the merged results to this JSON file")
    parser.add_argument("--compare", help="a JSON file saved earlier, shown as 'before'")
    args = parser.parse_args()

    if args.log:
        runs = [json.loads(line[line.index("BENCH_JSON ") + len("BENCH_JSON "):])
                for line in open(args.log, encoding="utf-8", errors="replace") if "BENCH_JSON " in line]
    elif args.exe:
        cmd = [os.path.abspath(args.exe), "--headless"]
    else:
        cmd = [os.environ.get("GODOT", "godot"), "--headless", "--path", os.path.join(ROOT, "demo"), "res://bench/run_bench.tscn"]
    if not args.log:
        runs = []
        for i in range(args.runs):
            print("run %d/%d..." % (i + 1, args.runs), file=sys.stderr)
            runs.append(run_once(cmd))
    data = best_of(runs)
    if args.save:
        os.makedirs(os.path.dirname(os.path.abspath(args.save)), exist_ok=True)
        with open(args.save, "w", encoding="utf-8", newline="\n") as f:
            json.dump(data, f, indent=1)
            f.write("\n")
    before = json.load(open(args.compare, encoding="utf-8")) if args.compare else None
    table(data, before)


if __name__ == "__main__":
    main()
