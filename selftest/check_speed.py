"""selftest_blinky_speed: check_speed.py <exe> <run_ms> <max_wall_s>"""
import subprocess
import sys
import time

exe, run_ms, limit = sys.argv[1], sys.argv[2], float(sys.argv[3])
t0 = time.monotonic()
p = subprocess.run([exe, "--run-ms", run_ms], capture_output=True, text=True)
wall = time.monotonic() - t0
per_vs = wall / (int(run_ms) / 1000)
print(f"{exe} --run-ms {run_ms}: wall {wall:.3f} s ({per_vs:.3f} s per virtual s), limit {limit} s")
if p.returncode != 0:
    print(p.stdout, p.stderr)
    sys.exit(f"exit {p.returncode}")
if wall > limit:
    sys.exit(f"too slow: {wall:.3f} s > {limit} s")
