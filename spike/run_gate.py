#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Interception speed gate: build the benchmarks (Release), run each 5x, take medians.

PASS iff median(mmio_s) + median(sync_s) <= 0.40 (wall seconds per virtual second).
sync_s is one virtual second of RUN_UNTIL/STOPPED round trips at --quantum-us (default 100;
88 is the quantum a 500 kbit/s CAN bus imposes, see docs/can.md).

--can-worst-case adds an informational row that never changes the verdict: one can_err node
alone on a CAN bus, retrying a frame nobody acknowledges forever (q = 88 us), measured end to
end through the Python harness (spike/gate_can_noack.py; builds can_err, needs uv).

Usage: uv run spike/run_gate.py [--build-dir DIR] [--runs N] [--quantum-us US] [--can-worst-case]
(use a separate --build-dir per host/container sharing the tree, e.g. build-gate-docker)
"""
import argparse, os, re, statistics, subprocess, sys
from pathlib import Path

BUDGET = 0.40
ROOT = Path(__file__).resolve().parent.parent

def run(cmd, **kw):
    subprocess.run(cmd, check=True, **kw)

def measure(cmd, key, runs):
    exe = cmd[0]
    vals = []
    for _ in range(runs):
        r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
        if r.stderr:
            sys.stderr.write(r.stderr)
            sys.stderr.flush()
        if r.returncode != 0:
            sys.exit(f"{exe} exited with status {r.returncode}")
        out = r.stdout
        m = re.search(rf"{key}=([0-9.]+)", out)
        if not m:
            sys.exit(f"{exe}: no {key}= in output: {out!r}")
        vals.append(float(m.group(1)))
    return vals

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=str(ROOT / "build-gate"))
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--quantum-us", type=int, default=100,
                    help="lockstep quantum of the sync benchmark (default 100; CAN at 500 kbit/s: 88)")
    ap.add_argument("--can-worst-case", action="store_true",
                    help="also measure the CAN no-ACK worst case (informational, not gated)")
    a = ap.parse_args()
    if a.quantum_us <= 0:
        ap.error("--quantum-us must be positive")
    bd = Path(a.build_dir)
    cfg = ["cmake", "-S", str(ROOT), "-B", str(bd), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"]
    if os.environ.get("STEST_LLVM_ROOT"):
        cfg.append(f"-DSTEST_LLVM_ROOT={os.environ['STEST_LLVM_ROOT']}")
    run(cfg)
    run(["cmake", "--build", str(bd), "--target", "bench_lighting_load", "bench_sync"])
    mm = measure([bd / "spike" / "bench_lighting_load"], "mmio_s", a.runs)
    sy = measure([bd / "spike" / "bench_sync", str(a.quantum_us)], "sync_s", a.runs)
    for name, v in (("mmio_s", mm), ("sync_s", sy)):
        print(f"{name} runs: " + " ".join(f"{x:.6f}" for x in v))
    mmed, smed = statistics.median(mm), statistics.median(sy)
    total = mmed + smed
    print(f"mmio_s median = {mmed:.6f}")
    print(f"sync_s median = {smed:.6f}")
    print(f"quantum_us = {a.quantum_us}")
    print(f"total = {total:.6f} (budget {BUDGET:.2f})")
    print("PASS" if total <= BUDGET else "FAIL")
    if a.can_worst_case:
        can_worst_case(bd, a.runs)
    return 0 if total <= BUDGET else 1

def can_worst_case(bd, runs):
    """Informational: never changes the verdict or the exit status."""
    try:
        _can_worst_case(bd, runs)
    except Exception as e:  # e.g. uv not on PATH
        print(f"noack_s: not measured ({type(e).__name__}: {e})")

def _can_worst_case(bd, runs):
    print("-- informational (not gated): CAN worst case, lone node, endless no-ACK retries, q = 88 us --")
    sys.stdout.flush()
    b = subprocess.run(["cmake", "--build", str(bd), "--target", "can_err"], capture_output=True, text=True)
    if b.returncode != 0:
        sys.stderr.write(b.stdout[-4000:] + b.stderr[-4000:])
        print(f"noack_s: not measured (building can_err failed, exit {b.returncode})")
        return
    cmd = ["uv", "run", "--project", str(ROOT), "python", str(ROOT / "spike" / "gate_can_noack.py"),
           str(bd / "selftest" / "can_err"), "--runs", str(runs)]
    # the build dir's own venv (as CTest's python_tests): host and container never share one
    env = {**os.environ, "UV_PROJECT_ENVIRONMENT": str(bd.resolve() / "py-venv")}
    env.pop("VIRTUAL_ENV", None)  # the gate script's own uv environment
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    vals = [float(v) for v in re.findall(r"noack_s=([0-9.]+)", r.stdout)]
    if r.returncode != 0 or len(vals) != runs:
        sys.stdout.flush()
        sys.stderr.write(r.stdout + r.stderr)
        print(f"noack_s: not measured (exit {r.returncode})")
        return
    print("noack_s runs: " + " ".join(f"{x:.6f}" for x in vals))
    att = re.findall(r"attempts_per_virtual_s=([0-9]+)", r.stdout)
    print("attempts per virtual s: " + " ".join(att))
    print(f"noack_s median = {statistics.median(vals):.6f} (worst case, wall s per virtual s; "
          f"informational, budget {BUDGET:.2f} not enforced)")

if __name__ == "__main__":
    sys.exit(main())
