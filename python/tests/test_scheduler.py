"""Lockstep scheduler: virtual time, multi-node determinism, model calls, waits."""

from __future__ import annotations

import pytest

from stest import Sim, StestError, WaitTimeout


def test_blinky_edges_on_virtual_time(sim, blinky_exe):
    n = sim.node("b", blinky_exe)
    sim.start()
    sim.advance(ms=2100)
    assert [round(t / 1e6) for t, _ in n.pin("PA5").history] == [500, 1000, 1500, 2000]
    assert [lvl for _, lvl in n.pin("PA5").history] == [1, 0, 1, 0]
    assert sim.now_ns == 2_100_000_000


def run_two_nodes_log(exe) -> str:
    with Sim() as s:
        a = s.node("a", exe)
        b = s.node("b", exe)
        s.start()
        s.advance(ms=600)
        # Interleave a host-driven input on one node to exercise MODEL_CALL events.
        b.pin("PC13").set_high()
        s.advance(ms=1500)
        assert len(a.pin("PA5").history) == 4
        return "\n".join(f"{t} {node} {json}" for t, node, json in s.event_log)


def test_two_nodes_lockstep_deterministic(blinky_exe):  # run twice, compare merged event logs
    log1 = run_two_nodes_log(blinky_exe)
    log2 = run_two_nodes_log(blinky_exe)
    assert log1 == log2
    lines = log1.splitlines()
    assert len(lines) == 9  # 4 edges per node + the PC13 drive on b
    # Merged order: by time, then node creation order.
    assert [ln.split()[1] for ln in lines[:2]] == ["a", "b"]
    assert any(" b " in ln and '"port":"C"' in ln and '"pin":13' in ln for ln in lines)


def test_two_instances_independent(sim, blinky_exe):
    a = sim.node("a", blinky_exe)
    b = sim.node("b", blinky_exe)
    sim.start()
    a.call("GPIOA", "drive_pin", pin=0, level=1)
    sim.advance(ms=1)
    assert a.call("GPIOA", "get_pin", pin=0) == 1 and b.call("GPIOA", "get_pin", pin=0) == 0


def test_wait_for_times_out_in_virtual_time(sim, blinky_exe):
    n = sim.node("b", blinky_exe)
    sim.start()
    sim.advance(ms=3)
    start = sim.now_ns
    with pytest.raises(WaitTimeout):
        sim.wait_for(lambda: False, timeout_ms=50)
    assert abs(sim.now_ns - (start + 50_000_000)) <= sim.quantum_ns
    # wait_for returns as soon as the condition holds (checked every quantum).
    sim.wait_for(lambda: len(n.pin("PA5").history) >= 1, timeout_ms=1000)
    assert 500_000_000 <= sim.now_ns <= 500_000_000 + 2 * sim.quantum_ns


def test_pin_waits_and_level(sim, blinky_exe):
    n = sim.node("b", blinky_exe)
    sim.start()
    led = n.pin("PA5")
    led.wait_high(timeout_ms=600)
    assert led.level == 1
    led.wait_low(timeout_ms=600)
    assert led.level == 0
    with pytest.raises(WaitTimeout):
        led.wait_high(timeout_ms=100)  # next rising edge is at 1500 ms
    btn = n.pin("PC13")
    btn.set_high()
    assert btn.level == 1
    btn.set_low()
    assert btn.level == 0
    btn.release()


def test_memory_and_errors(sim, blinky_exe):
    n = sim.node("b", blinky_exe)
    sim.start()
    sim.advance(ms=1)
    # The HELLO anchor is a real, readable/writable byte of the node's image.
    assert n.read_mem(n.anchor_addr, 1) == b"\0"
    n.write_mem(n.anchor_addr, b"\x5a")
    assert n.read_mem(n.anchor_addr, 1) == b"\x5a"
    with pytest.raises(StestError, match="read_mem"):
        n.read_mem(0x10, 4)  # unmapped: recoverable error, node still alive
    with pytest.raises(StestError, match="unknown model"):
        n.call("NOPE", "get_pin", pin=0)
    with pytest.raises(StestError, match="unknown op"):
        n.call("GPIOA", "frobnicate")
    sim.advance(ms=1)
    assert sim.now_ns == 2_000_000
