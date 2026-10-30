#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Interception speed gate: build the benchmarks (Release), run each 5x, take medians.

PASS iff median(mmio_s) + median(sync_s) <= 0.40 (wall seconds per virtual second).
Usage: uv run spike/run_gate.py [--build-dir DIR] [--runs N]
"""
import argparse, os, re, statistics, subprocess, sys
from pathlib import Path

BUDGET = 0.40
ROOT = Path(__file__).resolve().parent.parent

def run(cmd, **kw):
    subprocess.run(cmd, check=True, **kw)

def measure(exe, key, runs):
    vals = []
    for _ in range(runs):
        out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
        m = re.search(rf"{key}=([0-9.]+)", out)
        if not m:
            sys.exit(f"{exe}: no {key}= in output: {out!r}")
        vals.append(float(m.group(1)))
    return vals

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=str(ROOT / "build-gate"))
    ap.add_argument("--runs", type=int, default=5)
    a = ap.parse_args()
    bd = Path(a.build_dir)
    cfg = ["cmake", "-S", str(ROOT), "-B", str(bd), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"]
    if os.environ.get("STEST_LLVM_ROOT"):
        cfg.append(f"-DSTEST_LLVM_ROOT={os.environ['STEST_LLVM_ROOT']}")
    run(cfg)
    run(["cmake", "--build", str(bd), "--target", "bench_lighting_load", "bench_sync"])
    mm = measure(bd / "spike" / "bench_lighting_load", "mmio_s", a.runs)
    sy = measure(bd / "spike" / "bench_sync", "sync_s", a.runs)
    for name, v in (("mmio_s", mm), ("sync_s", sy)):
        print(f"{name} runs: " + " ".join(f"{x:.6f}" for x in v))
    mmed, smed = statistics.median(mm), statistics.median(sy)
    total = mmed + smed
    print(f"mmio_s median = {mmed:.6f}")
    print(f"sync_s median = {smed:.6f}")
    print(f"total = {total:.6f} (budget {BUDGET:.2f})")
    print("PASS" if total <= BUDGET else "FAIL")
    return 0 if total <= BUDGET else 1

if __name__ == "__main__":
    sys.exit(main())
