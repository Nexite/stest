#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Interception speed gate: build the benchmarks (Release), run each 5x, take medians.

PASS iff median(mmio_s) + median(sync_s) <= 0.40 (wall seconds per virtual second).
sync_s is one virtual second of RUN_UNTIL/STOPPED round trips at --quantum-us (default 100;
88 is the quantum a 500 kbit/s CAN bus imposes, see docs/can.md).

--can-two-node adds a GATED row: two can_node firmwares exchanging frames on one 500 kbit/s bus
(q = 88 us), measured end to end through the Python harness (spike/gate_can_two_node.py; builds
can_node1/can_node2, needs uv). PASS needs its median <= 2 x 0.40 (0.40 per node) as well.

--can-worst-case adds an informational row that never changes the verdict: one can_err node
alone on a CAN bus, retrying a frame nobody acknowledges forever (q = 88 us), measured end to
end through the Python harness (spike/gate_can_noack.py; builds can_err, needs uv).

Usage: uv run spike/run_gate.py [--build-dir DIR] [--runs N] [--quantum-us US] [--can-two-node]
                                [--can-worst-case]
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
    ap.add_argument("--can-two-node", action="store_true",
                    help="also gate two CAN nodes on one bus (q = 88 us, budget 2 x 0.40)")
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
    ok = total <= BUDGET
    print("PASS" if ok else "FAIL")
    if a.can_two_node:
        ok = can_two_node(bd, a.runs) and ok
        print(f"verdict (standard + CAN two-node) = {'PASS' if ok else 'FAIL'}")
    if a.can_worst_case:
        can_worst_case(bd, a.runs)
    return 0 if ok else 1

def _harness(bd, script, args, runs, key):
    """Runs spike/<script> through uv in the build dir's own venv; returns (values, stdout) or
    None after printing why it failed."""
    cmd = ["uv", "run", "--project", str(ROOT), "python", str(ROOT / "spike" / script), *args,
           "--runs", str(runs)]
    # the build dir's own venv (as CTest's python_tests): host and container never share one
    env = {**os.environ, "UV_PROJECT_ENVIRONMENT": str(bd.resolve() / "py-venv")}
    env.pop("VIRTUAL_ENV", None)  # the gate script's own uv environment
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    vals = [float(v) for v in re.findall(rf"{key}=([0-9.]+)", r.stdout)]
    if r.returncode != 0 or len(vals) != runs:
        sys.stdout.flush()
        sys.stderr.write(r.stdout + r.stderr)
        print(f"{key}: not measured (exit {r.returncode})")
        return None
    return vals, r.stdout

def _build(bd, *targets):
    b = subprocess.run(["cmake", "--build", str(bd), "--target", *targets], capture_output=True, text=True)
    if b.returncode != 0:
        sys.stderr.write(b.stdout[-4000:] + b.stderr[-4000:])
        return f"building {' '.join(targets)} failed, exit {b.returncode}"
    return None

def can_two_node(bd, runs):
    """Gated: two CAN nodes on one bus must stay within 0.40 per node per virtual second."""
    budget = 2 * BUDGET
    print(f"-- gated: CAN two nodes on one 500 kbit/s bus, q = 88 us (budget {budget:.2f} = 2 x {BUDGET:.2f}) --")
    sys.stdout.flush()
    try:
        err = _build(bd, "can_node1", "can_node2")
        if err:
            print(f"two_node_s: not measured ({err})")
            return False
        exes = [str(bd / "selftest" / e) for e in ("can_node1", "can_node2")]
        res = _harness(bd, "gate_can_two_node.py", exes, runs, "two_node_s")
    except Exception as e:  # e.g. uv not on PATH
        print(f"two_node_s: not measured ({type(e).__name__}: {e})")
        return False
    if res is None:
        return False
    vals = res[0]
    med = statistics.median(vals)
    print("two_node_s runs: " + " ".join(f"{x:.6f}" for x in vals))
    print(f"two_node_s median = {med:.6f} (wall s per virtual s, 2 nodes; budget {budget:.2f})")
    print("CAN two-node: " + ("PASS" if med <= budget else "FAIL"))
    return med <= budget

def can_worst_case(bd, runs):
    """Informational: never changes the verdict or the exit status."""
    try:
        _can_worst_case(bd, runs)
    except Exception as e:  # e.g. uv not on PATH
        print(f"noack_s: not measured ({type(e).__name__}: {e})")

def _can_worst_case(bd, runs):
    print("-- informational (not gated): CAN worst case, lone node, endless no-ACK retries, q = 88 us --")
    sys.stdout.flush()
    err = _build(bd, "can_err")
    if err:
        print(f"noack_s: not measured ({err})")
        return
    res = _harness(bd, "gate_can_noack.py", [str(bd / "selftest" / "can_err")], runs, "noack_s")
    if res is None:
        return
    vals, out = res
    print("noack_s runs: " + " ".join(f"{x:.6f}" for x in vals))
    att = re.findall(r"attempts_per_virtual_s=([0-9]+)", out)
    print("attempts per virtual s: " + " ".join(att))
    print(f"noack_s median = {statistics.median(vals):.6f} (worst case, wall s per virtual s; "
          f"informational, budget {BUDGET:.2f} not enforced)")

if __name__ == "__main__":
    sys.exit(main())
