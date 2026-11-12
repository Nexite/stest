"""TIM: a 20 kHz TIM7 time base (selftest firmware `tim`) and the Timer helper."""

from __future__ import annotations

import time

import pytest

from stest import Sim
from stest.errors import StestError


def high_fraction(history: list[tuple[int, int]], level0: int, t0: int, t1: int) -> float:
    """Fraction of [t0, t1) a pin spent high, from its (t_ns, level) change history."""
    level, t, high = level0, t0, 0
    for te, lv in history:
        if te <= t0:
            level = lv
            continue
        if te >= t1:
            break
        if level:
            high += te - t
        level, t = lv, te
    if level:
        high += t1 - t
    return high / (t1 - t0)


def level_at(history: list[tuple[int, int]], t: int) -> int:
    level = 0
    for te, lv in history:
        if te > t:
            break
        level = lv
    return level


def test_tim7_20khz(tim_exe):
    with Sim() as s:
        n = s.node("n", tim_exe)
        s.start()
        s.advance(ms=1)  # boot: clocks, TIM7 started
        assert n.var("tick_count") > 0
        n.set_var("tick_count", 0)
        t0 = s.now_ns
        s.advance(ms=100)
        assert n.var("tick_count") == 2000  # one update every 50 us
        pa6 = n.pin("PA6")
        hist = pa6.history
        assert len(hist) >= 40  # 20 PWM periods, two edges each
        frac = high_fraction(hist, level_at(hist, t0), t0, s.now_ns)
        assert frac == pytest.approx(0.25, abs=0.01)
        tim = n.timer("TIM7")
        st = tim.state()
        assert st["running"] is True
        assert st["arr"] == 3999 and st["psc"] == 0 and st["clk_hz"] == 80_000_000
        assert tim.update_hz == pytest.approx(20_000.0)
        assert st["channels"] == []


def test_timer_helper_errors(tim_exe):
    with Sim() as s:
        n = s.node("n", tim_exe)
        s.start()
        s.advance(ms=1)
        with pytest.raises(StestError, match="TIM7 has no channel 1"):
            _ = n.timer("TIM7").channel(1).duty
        with pytest.raises(StestError, match="unknown model"):
            n.timer("TIM99").state()


def test_tim_speed_two_virtual_seconds(tim_exe):
    """Speed budget: <= 0.4 s wall per virtual second with a 20 kHz timer interrupt."""
    with Sim() as s:
        n = s.node("n", tim_exe)
        s.start()
        t = time.perf_counter()
        s.advance(ms=2000)
        wall = time.perf_counter() - t
        assert n.var("tick_count") >= 39_999
    assert wall <= 0.8, f"2 virtual s took {wall:.3f} s wall (budget 0.8 s)"


def test_tim_is_deterministic(tim_exe):
    def run(quantum_us: int):
        with Sim(quantum_us=quantum_us) as s:
            n = s.node("n", tim_exe)
            s.start()
            s.advance(ms=20)
            return list(s.event_log), n.var("tick_count")

    a = run(1000)
    assert a == run(1000)
    # a different quantum only regroups the work: same edges at the same times
    assert a == run(250)
