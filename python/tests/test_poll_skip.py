"""Poll fast-forward is user-controllable (STEST_POLL_SKIP / Sim(poll_skip=)) and a
counting loop over a constant volatile read is time-dilated by it (pinned behaviour)."""

from __future__ import annotations

import pytest

from stest import Sim

ITERS = 1000          # selftest/firmware/pollcount/main.c
ACCESS_COST_NS = 25   # Sim default
TICK_NS = 250         # SysTick counts the 4 MHz MSI core clock


def run_pollcount(exe, poll_skip, quantum_us=1000) -> int:
    """Virtual ns the firmware measured for its counting loop."""
    with Sim(quantum_us=quantum_us, poll_skip=poll_skip) as s:
        n = s.node("p", exe)
        s.start()
        s.wait_for(lambda: n.var("done") == 1, timeout_ms=2000)
        return n.var("elapsed_ticks") * TICK_NS


def test_poll_skip_disabled_is_exact(pollcount_exe):
    elapsed = run_pollcount(pollcount_exe, poll_skip=0)
    expected = ITERS * ACCESS_COST_NS
    assert expected <= elapsed <= 2 * expected, elapsed


def test_default_threshold_dilates_counting_loop(pollcount_exe):
    # Current behaviour, pinned: every 64 store-free loads jump to the quantum end
    # (no event pending), so the loop takes ~ceil(ITERS/64) quanta instead of 25 us.
    expected = ITERS * ACCESS_COST_NS
    elapsed = run_pollcount(pollcount_exe, poll_skip=None)
    assert elapsed > 100 * expected, elapsed
    # ... and depends on the quantum size.
    assert run_pollcount(pollcount_exe, poll_skip=None, quantum_us=200) < elapsed / 2


def test_poll_skip_validation():
    with pytest.raises(ValueError):
        Sim(poll_skip=-1)
