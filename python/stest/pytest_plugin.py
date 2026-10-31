"""pytest plugin: the ``sim`` fixture, options, the ``stest`` marker, failure reports.

Registered through the ``pytest11`` entry point (installed with the package).

* fixture ``sim``: a function-scoped :class:`stest.Sim`, closed on teardown (also when the
  test raised or timed out).
* options ``--stest-quantum-us`` (default 1000) and ``--stest-wall-timeout`` (seconds,
  default 30).
* option ``--stest-poll-skip`` (poll fast-forward threshold; 0 disables; default: runtime's 64).
* option ``--stest-strict-analog``: converting an ADC channel with no input voltage aborts
  the node (default: it reads 0 V with one stderr warning).
* option ``--stest-no-debugger``: power-on state without an attached debugger (ITM/DWT trace
  disabled; ``ITM_SendChar`` drops output). Default: a debugger is attached.
* marker ``@pytest.mark.stest(quantum_us=..., wall_timeout_s=..., poll_skip=..., strict_analog=...,
  debugger_attached=...)`` overrides them per test (marker > CLI > default).
* on failure each node's last 200 stderr lines, ITM port-0 log, last 200 firmware output lines
  (``Node.output``: ITM port 0 + stdout in virtual-time order), last 200 raw stdout lines and
  the merged event log are attached to the report as ``stest node <name> stderr`` / ``log`` /
  ``output`` / ``stdout`` and ``stest events`` sections.
"""

from __future__ import annotations

import pytest

from .sim import Sim

EVENT_LOG_LINES = 500
ITM_LOG_LINES = 200
_SIM_KEY = pytest.StashKey[Sim]()


def pytest_addoption(parser: pytest.Parser) -> None:
    g = parser.getgroup("stest", "stest firmware simulation")
    g.addoption("--stest-wall-timeout", type=float, default=30.0, metavar="SECONDS",
                help="wall-clock limit for every blocking wait on a node (default: 30)")
    g.addoption("--stest-quantum-us", type=int, default=1000, metavar="US",
                help="lockstep quantum in virtual microseconds (default: 1000)")
    g.addoption("--stest-poll-skip", type=int, default=None, metavar="N",
                help="poll fast-forward threshold in store-free loads; 0 disables "
                     "(default: the runtime's, 64)")
    g.addoption("--stest-strict-analog", action="store_true", default=None,
                help="abort a node that converts an ADC channel with no input voltage set "
                     "(default: read 0 V and warn once per channel)")
    g.addoption("--stest-no-debugger", action="store_true", default=None,
                help="start without an attached debugger: ITM/DWT trace and DHCSR.C_DEBUGEN off, "
                     "so CMSIS ITM_SendChar drops output (default: debugger attached)")


def pytest_configure(config: pytest.Config) -> None:
    config.addinivalue_line(
        "markers",
        "stest(quantum_us=..., wall_timeout_s=..., poll_skip=..., strict_analog=..., debugger_attached=...): "
        "override the simulation quantum, wall-clock timeout, poll fast-forward threshold, strict "
        "analog inputs and debugger-attached power-on state of the `sim` fixture for this test",
    )


@pytest.fixture
def sim(request: pytest.FixtureRequest):
    cfg = request.config
    quantum_us = cfg.getoption("--stest-quantum-us")
    wall_timeout_s = cfg.getoption("--stest-wall-timeout")
    poll_skip = cfg.getoption("--stest-poll-skip")
    strict_analog = cfg.getoption("--stest-strict-analog")
    debugger_attached = False if cfg.getoption("--stest-no-debugger") else None
    marker = request.node.get_closest_marker("stest")
    if marker is not None:
        unknown = set(marker.kwargs) - {"quantum_us", "wall_timeout_s", "poll_skip", "strict_analog",
                                         "debugger_attached"}
        if unknown or marker.args:
            raise pytest.UsageError(
                "@pytest.mark.stest accepts only quantum_us=, wall_timeout_s=, poll_skip=, "
                f"strict_analog= and debugger_attached= (got {marker.args} {sorted(unknown)})"
            )
        quantum_us = marker.kwargs.get("quantum_us", quantum_us)
        wall_timeout_s = marker.kwargs.get("wall_timeout_s", wall_timeout_s)
        poll_skip = marker.kwargs.get("poll_skip", poll_skip)
        strict_analog = marker.kwargs.get("strict_analog", strict_analog)
        debugger_attached = marker.kwargs.get("debugger_attached", debugger_attached)
    s = Sim(quantum_us=quantum_us, wall_timeout_s=wall_timeout_s, poll_skip=poll_skip,
            strict_analog=strict_analog, debugger_attached=debugger_attached)
    request.node.stash[_SIM_KEY] = s
    try:
        yield s
    finally:
        s.close()


@pytest.hookimpl(wrapper=True)
def pytest_runtest_makereport(item: pytest.Item, call: pytest.CallInfo):
    # Sections must be added before the report is built, so decide on call.excinfo.
    s = item.stash.get(_SIM_KEY, None)
    if call.excinfo is not None and s is not None:
        for n in s.nodes:
            lines = n.stderr_lines(200)
            item.add_report_section(
                call.when, f"stest node {n.name} stderr", "\n".join(lines) if lines else "(no stderr output)"
            )
            log = n.log
            item.add_report_section(
                call.when, f"stest node {n.name} log", "\n".join(log[-ITM_LOG_LINES:]) if log else "(no ITM output)"
            )
            output = n.output
            item.add_report_section(
                call.when,
                f"stest node {n.name} output",
                "\n".join(output[-ITM_LOG_LINES:]) if output else "(no firmware output)",
            )
            out = n.stdout_lines(200)
            item.add_report_section(
                call.when, f"stest node {n.name} stdout", "\n".join(out) if out else "(no stdout output)"
            )
        if s.event_log:
            log = s.event_log[-EVENT_LOG_LINES:]
            text = "\n".join(f"{t} {node} {js}" for t, node, js in log)
            if len(s.event_log) > len(log):
                text = f"... ({len(s.event_log) - len(log)} earlier events omitted)\n" + text
            item.add_report_section(call.when, "stest events", text)
    return (yield)
