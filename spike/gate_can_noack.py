"""Speed-gate informational row: the CAN worst case, a lone node retrying forever.

One ``can_err`` node (selftest/firmware/can) alone on a 500 kbit/s bus: nobody acknowledges,
so its frame is retransmitted back to back for the whole run (ACK error, error passive after
16 attempts, then every ~140 us with suspend transmission). The bus caps the quantum at 88 us
and the engine decides an attempt, posts a result and an SCE interrupt every few quanta, so
this is the most expensive single-node CAN load stest has. Prints the wall seconds per
virtual second of each run (``noack_s=``).

Run by ``spike/run_gate.py --can-worst-case`` (with the stest package on the path):
    uv run --project <repo> python spike/gate_can_noack.py <can_err exe> [--runs N] [--ms MS]
"""

from __future__ import annotations

import argparse
import time

from stest import Sim


def one_run(exe: str, virtual_ms: int) -> float:
    with Sim() as s:
        bus = s.can_bus("gate", 500_000)
        s.node("n", exe, can={"CAN2": bus})
        s.start()
        s.advance(ms=20)  # init: HAL_CAN_Start and the first request (from 10 ms)
        assert bus.errors, "no ACK errors: the scenario did not start retrying"
        t0 = time.perf_counter()
        s.advance(ms=virtual_ms)
        wall = time.perf_counter() - t0
        assert s.quantum_ns == 88_000 and bus.log == []
        return wall / (virtual_ms / 1000)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--ms", type=int, default=1000, help="virtual ms measured per run (default 1000)")
    a = ap.parse_args()
    for _ in range(a.runs):
        print(f"noack_s={one_run(a.exe, a.ms):.6f}", flush=True)


if __name__ == "__main__":
    main()
