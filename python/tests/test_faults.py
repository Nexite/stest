"""Fault handling: wall-clock timeout, host crash, strict-mode abort; cleanup."""

from __future__ import annotations

import os

import pytest

from stest import NodeCrashed, Sim, StestError, WallClockTimeout


def assert_reaped(pids):
    # kill(pid, 0) still succeeds on an unreaped zombie: ProcessLookupError proves the
    # child is both dead and reaped.
    for pid in pids:
        with pytest.raises(ProcessLookupError):
            os.kill(pid, 0)


def test_wallclock_timeout_on_spinning_node(faults_exe, blinky_exe):
    s = Sim(wall_timeout_s=2)
    try:
        other = s.node("ok", blinky_exe)
        s.node("spin", faults_exe("spin"))
        s.start()
        with pytest.raises(WallClockTimeout, match=r"node 'spin'.*virtual t=10\.\d+ ms"):
            s.advance(ms=100)
        pids = [n.pid for n in s.nodes]
        assert other.pid in pids
    finally:
        s.close()
    assert_reaped(pids)


def test_crash_reports_and_cleans_up(faults_exe, blinky_exe):
    s = Sim()
    try:
        s.node("ok", blinky_exe)
        s.node("crash", faults_exe("crash"))
        s.start()
        pids = [n.pid for n in s.nodes]
        with pytest.raises(NodeCrashed, match=r"node 'crash'.*SIG(SEGV|BUS)") as ei:
            s.advance(ms=100)
        assert "virtual t=10." in str(ei.value)
        # Every node was killed and reaped before the exception reached us.
        assert_reaped(pids)
        with pytest.raises(NodeCrashed):
            s.advance(ms=1)  # the sim is dead
    finally:
        s.close()


def test_unmodelled_access_message(faults_exe):
    with Sim() as s:
        s.node("u", faults_exe("unmodelled"))
        s.start()
        with pytest.raises(NodeCrashed, match=r"stest: unmodelled write32 0x40013800") as ei:
            s.advance(ms=100)
        # The headline carries the ERROR message the node sent over the control channel
        # (not just the stderr tail).
        first = str(ei.value).splitlines()[0]
        assert first == (
            "node 'u' aborted at virtual t=10.000 ms: "
            "stest: unmodelled write32 0x40013800 (exit status 70)"
        )
        pids = [n.pid for n in s.nodes]
    assert_reaped(pids)


def test_write_to_read_only_register_aborts(faults_exe):
    with Sim() as s:
        s.node("ro", faults_exe("ro_write"))
        s.start()
        with pytest.raises(NodeCrashed) as ei:
            s.advance(ms=100)
        first = str(ei.value).splitlines()[0]
        assert first == (
            "node 'ro' aborted at virtual t=10.000 ms: "
            "stest: write to read-only register 0x48000010 (write32, GPIOA) (exit status 70)"
        )


def test_access_with_clock_disabled_aborts(faults_exe):
    with Sim() as s:
        s.node("clk", faults_exe("clkoff"))
        s.start()
        with pytest.raises(
            NodeCrashed,
            match=r"access to GPIOB while its clock is disabled \(RCC AHB2ENR bit 1\)",
        ) as ei:
            s.advance(ms=100)
        first = str(ei.value).splitlines()[0]
        assert first == (
            "node 'clk' aborted at virtual t=10.000 ms: "
            "stest: access to GPIOB while its clock is disabled (RCC AHB2ENR bit 1) (exit status 70)"
        )


def test_stderr_tail_in_crash_message(faults_exe):
    with Sim() as s:
        # 5000 long lines (> any pipe buffer) before the abort: stderr must be drained
        # continuously or the node would block on a full pipe and never abort.
        n = s.node("u", faults_exe("unmodelled"), env={"STEST_FAULT_CHATTER": "5000"})
        s.start()
        with pytest.raises(NodeCrashed) as ei:
            s.advance(ms=100)
        msg = str(ei.value)
        # last 20 stderr lines only: chatter lines 4981..4999 plus the abort line
        assert "chatter 4999 " in msg and "chatter 4981 " in msg and "chatter 4980 " not in msg
        assert len(n.stderr_tail) == 20 and n.stderr_tail[-1].startswith("stest: unmodelled")


def test_requests_after_crash_report_the_crash(faults_exe):
    with Sim() as s:
        n = s.node("crash", faults_exe("crash"))
        s.start()
        with pytest.raises(NodeCrashed):
            s.advance(ms=100)
        with pytest.raises(NodeCrashed, match=r"no longer usable after: node 'crash' crashed at .*SIG(SEGV|BUS)"):
            n.var("ticks")
        with pytest.raises(NodeCrashed, match=r"no longer usable after: node 'crash' crashed"):
            n.pin("PA5").level


def test_requests_after_close_report_closed(blinky_exe):
    s = Sim()
    n = s.node("b", blinky_exe)
    s.start()
    s.advance(ms=1)
    s.close()
    with pytest.raises(StestError, match=r"simulation is closed"):
        n.var("uwTick")
    with pytest.raises(StestError, match=r"simulation is closed"):
        n.pin("PA5").level
