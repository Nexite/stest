"""The stest pytest plugin's CAN features (via pytester): the "stest can <bus>" failure
report section and ``--stest-can-log DIR`` candump files. Python devices only (a Sim with
buses and no nodes is valid), so no firmware is needed."""

from __future__ import annotations

# Inner test module: a device sends `n` frames (id 0x123, data = 2-byte frame index) 1 ms
# apart on bus "drive" (and one frame on bus "body" when two buses are asked for).
_SENDER = """
import pytest
from stest import CanDevice, CanFrame
from stest.can import format_candump


def run(sim, n, two_buses=False):
    bus = sim.can_bus("drive", 500_000)
    bus.attach(CanDevice("ecu"))
    tx = bus.attach(CanDevice("tx"))
    k = [0]

    def tick():
        if k[0] < n:
            tx.send(CanFrame(0x123, k[0].to_bytes(2, "big")))
            k[0] += 1

    tx.every(1, tick)
    body = None
    if two_buses:
        body = sim.can_bus("body", 250_000)
        body.attach(CanDevice("bcm"))
        body.send(CanFrame(0x7FF, b"\\xaa"), sender="tool")
    sim.start()
    sim.advance(ms=n + 2)
    assert len(bus.log) == n
    return bus, body
"""


def test_failure_report_has_can_bus_section(pytester):
    pytester.makepyfile(
        _SENDER
        + """
def test_fail(sim):
    run(sim, 3)
    assert False, "boom"
"""
    )
    r = pytester.runpytest("-rA")
    r.assert_outcomes(failed=1)
    r.stdout.fnmatch_lines(
        [
            "*Captured stest can drive call*",
            "(0.001*) drive 123#0000",
            "(0.002*) drive 123#0001",
            "(0.003*) drive 123#0002",
        ]
    )


def test_can_section_keeps_last_200_frames(pytester):
    pytester.makepyfile(
        _SENDER
        + """
def test_fail(sim):
    run(sim, 230)
    assert False, "boom"
"""
    )
    r = pytester.runpytest("-rA")
    r.assert_outcomes(failed=1)
    out = r.stdout.str()
    r.stdout.fnmatch_lines(["*Captured stest can drive call*", "... (30 earlier frames omitted)", "(0.031*) drive 123#001E"])
    assert "drive 123#001D" not in out  # frame 29 is the last omitted one
    assert "drive 123#00E5" in out  # frame 229, the last one
    assert out.count(" drive 123#") == 200


def test_can_section_per_bus_and_empty_bus(pytester):
    pytester.makepyfile(
        """
from stest import CanDevice


def test_fail(sim):
    sim.can_bus("quiet", 500_000).attach(CanDevice("d"))
    sim.start()
    sim.advance(ms=1)
    assert False
"""
    )
    r = pytester.runpytest("-rA")
    r.assert_outcomes(failed=1)
    r.stdout.fnmatch_lines(["*Captured stest can quiet call*", "(no frames)"])


def test_passing_test_has_no_can_section(pytester):
    pytester.makepyfile(
        _SENDER
        + """
def test_ok(sim):
    run(sim, 3)
"""
    )
    r = pytester.runpytest("-rA")
    r.assert_outcomes(passed=1)
    assert "stest can" not in r.stdout.str()


def test_can_log_option_writes_candump_files(pytester):
    logdir = pytester.path / "logs" / "can"  # created by the plugin
    pytester.makepyfile(
        _SENDER
        + """
import pytest


def test_ok(sim):
    run(sim, 3, two_buses=True)


@pytest.mark.parametrize("p", ["a/b"])
def test_failing_param(sim, p):
    run(sim, 2)
    assert False


def test_no_bus(sim):
    pass
"""
    )
    r = pytester.runpytest(f"--stest-can-log={logdir}", "-p", "no:cacheprovider")
    r.assert_outcomes(passed=2, failed=1)
    names = sorted(p.name for p in logdir.iterdir())
    assert names == ["test_failing_param_a_b_-drive.log", "test_ok-body.log", "test_ok-drive.log"]
    drive = (logdir / "test_ok-drive.log").read_text()
    assert drive.splitlines() == [
        "(0.001128) drive 123#0000",
        "(0.002128) drive 123#0001",
        "(0.003128) drive 123#0002",
    ]
    body = (logdir / "test_ok-body.log").read_text()
    assert body.endswith(" body 7FF#AA\n") and body.count("\n") == 1
    failing = (logdir / "test_failing_param_a_b_-drive.log").read_text()
    assert failing.count(" drive 123#") == 2  # written for failing tests too


def test_can_log_matches_bus_export(pytester):
    """The file is exactly CanBus.export_candump's output."""
    logdir = pytester.path / "out"
    pytester.makepyfile(
        _SENDER
        + """
def test_ok(sim, tmp_path):
    bus, _ = run(sim, 5)
    bus.export_candump(tmp_path / "ref.log")
    with open(r"%s", "w") as f:
        f.write((tmp_path / "ref.log").read_text())
"""
        % (pytester.path / "ref.log")
    )
    r = pytester.runpytest(f"--stest-can-log={logdir}")
    r.assert_outcomes(passed=1)
    assert (logdir / "test_ok-drive.log").read_text() == (pytester.path / "ref.log").read_text()
