"""Speed-gate CAN row: two firmware nodes exchanging frames on one 500 kbit/s bus.

``can_node1`` and ``can_node2`` (selftest/firmware/can) each send a frame every 10 ms and
receive the other's; the bus caps the quantum at 88 us. This is normal CAN traffic through the
whole harness (Python bus engine, inbox posts, bxCAN model, HAL RX interrupts), so unlike the
no-ACK worst case it is gated: the budget is 0.40 wall s per virtual s per node, i.e. 0.80 for
the pair. Prints the wall seconds per virtual second of each run (``two_node_s=``).

Run by ``spike/run_gate.py --can-two-node`` (with the stest package on the path):
    uv run --project <repo> python spike/gate_can_two_node.py <can_node1> <can_node2> [--runs N] [--ms MS]
"""

from __future__ import annotations

import argparse
import time

from stest import Sim


def one_run(exe1: str, exe2: str, virtual_ms: int) -> float:
    with Sim() as s:
        bus = s.can_bus("gate", 500_000)
        n1 = s.node("n1", exe1, can={"CAN2": bus})
        n2 = s.node("n2", exe2, can={"CAN2": bus})
        s.start()
        s.advance(ms=20)  # init: HAL_CAN_Start and the first frames (from 10 ms)
        f0, rx0 = len(bus.log), n1.var("rx_count") + n2.var("rx_count")
        t0 = time.perf_counter()
        s.advance(ms=virtual_ms)
        wall = time.perf_counter() - t0
        assert s.quantum_ns == 88_000
        frames = len(bus.log) - f0
        rx = n1.var("rx_count") + n2.var("rx_count") - rx0
        expect = 2 * virtual_ms // 10  # one frame per node per 10 ms, each received by the other
        assert frames >= expect - 2 and rx >= expect - 2, f"traffic too low: {frames} frames, {rx} received"
        return wall / (virtual_ms / 1000)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("exe1")
    ap.add_argument("exe2")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--ms", type=int, default=1000, help="virtual ms measured per run (default 1000)")
    a = ap.parse_args()
    for _ in range(a.runs):
        print(f"two_node_s={one_run(a.exe1, a.exe2, a.ms):.6f}", flush=True)


if __name__ == "__main__":
    main()
