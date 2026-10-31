"""Sim: owns the global virtual clock and advances all nodes in lockstep quanta."""

from __future__ import annotations

import atexit
import os
import time
import weakref
from typing import Callable, NoReturn

from . import protocol as proto
from .errors import NodeCrashed, StestError, WaitTimeout, WallClockTimeout
from .node import Node


# Every Sim that has spawned nodes and not been closed. If the interpreter exits without
# the Sim being closed (or a test run is interrupted), the atexit handler kills and reaps
# every node so none is left spinning in firmware.
_LIVE: weakref.WeakSet[Sim] = weakref.WeakSet()


@atexit.register
def _kill_live_sims() -> None:
    for sim in list(_LIVE):
        try:
            sim._kill_all(grace_s=0.0)
        except BaseException:
            pass


def fmt_ms(t_ns: int) -> str:
    """Integer ns -> "<ms with 3 decimals> ms" (exact, no float rounding)."""
    return f"{t_ns // 1_000_000}.{(t_ns % 1_000_000) // 1_000:03d} ms"


class Sim:
    """A simulation of one or more firmware nodes sharing a global virtual clock.

    Scheduler: each quantum, RUN_UNTIL(t+q) is sent to every node, then STOPPED is read
    from each in node-creation order (nodes run concurrently in between). Quantum
    boundaries lie on the absolute grid k*q. Events are merged by (t_ns, node index,
    per-node sequence). Every blocking read is bounded by ``wall_timeout_s`` of real
    time. On any node failure every node is killed and reaped before the exception
    (NodeCrashed / WallClockTimeout) propagates; the Sim is then unusable.
    """

    def __init__(self, quantum_us: int = 1000, wall_timeout_s: float = 30.0, access_cost_ns: int = 25):
        if quantum_us <= 0:
            raise ValueError("quantum_us must be positive")
        if wall_timeout_s <= 0:
            raise ValueError("wall_timeout_s must be positive")
        if access_cost_ns < 0:
            raise ValueError("access_cost_ns must be >= 0")
        self.quantum_ns = int(quantum_us) * 1000
        self.wall_timeout_s = float(wall_timeout_s)
        self.access_cost_ns = int(access_cost_ns)
        self.nodes: list[Node] = []
        # Merged event log: (t_ns, node name, event JSON text), in merge order.
        self.event_log: list[tuple[int, str, str]] = []
        self._now = 0
        self._started = False
        self._closed = False
        self._dead: StestError | None = None

    @property
    def quantum_us(self) -> int:
        return self.quantum_ns // 1000

    # ---- setup ----
    def node(self, name: str, exe: str | os.PathLike, env: dict | None = None) -> Node:
        if self._started:
            raise StestError("nodes must be created before sim.start()")
        if any(n.name == name for n in self.nodes):
            raise StestError(f"duplicate node name {name!r}")
        n = Node(self, len(self.nodes), name, exe, env)
        self.nodes.append(n)
        return n

    def start(self) -> None:
        """Spawns every node and waits for its HELLO."""
        if self._started:
            raise StestError("sim.start() called twice")
        if not self.nodes:
            raise StestError("sim.start() with no nodes")
        self._started = True
        _LIVE.add(self)
        extra = {"STEST_ACCESS_COST_NS": str(self.access_cost_ns)}
        try:
            for n in self.nodes:
                n._spawn(extra)
        except BaseException:
            self._kill_all()
            raise
        deadline = time.monotonic() + self.wall_timeout_s
        for n in self.nodes:
            mtype, payload = n._recv("HELLO", deadline)
            if mtype != proto.Msg.HELLO:
                self._node_gone(n, f"protocol error: expected HELLO, got message type {mtype}")
            try:
                version, anchor = proto.parse_hello(payload)
            except proto.ProtocolError as e:
                self._node_gone(n, f"protocol error: {e}")
            if version != proto.PROTO_VERSION:
                self._node_gone(n, f"protocol version {version}, expected {proto.PROTO_VERSION}")
            n.proto_version, n.anchor_addr = version, anchor

    # ---- time ----
    @property
    def now_ns(self) -> int:
        """Global virtual time: the end of the last completed quantum."""
        return self._now

    def advance(self, *, ms: float = 0, us: float = 0, ns: int = 0) -> None:
        """Advances every node by the given virtual duration (sum of the arguments)."""
        dur = round(ms * 1_000_000) + round(us * 1_000) + int(ns)
        if dur < 0:
            raise ValueError("cannot advance by a negative duration")
        self._check_running()
        end = self._now + dur
        while self._now < end:
            self._step(min(self._next_boundary(), end))

    def wait_for(self, cond: Callable[[], bool], timeout_ms: float, *, what: str | None = None) -> None:
        """Advances quantum by quantum until ``cond()`` is true (checked now and at every
        quantum boundary). Raises WaitTimeout after ``timeout_ms`` of virtual time, with
        ``now_ns`` = start + timeout."""
        self._check_running()
        start = self._now
        deadline = start + round(timeout_ms * 1_000_000)
        if cond():
            return
        while self._now < deadline:
            self._step(min(self._next_boundary(), deadline))
            if cond():
                return
        desc = what or getattr(cond, "__name__", "condition")
        raise WaitTimeout(
            f"wait for {desc} timed out after {timeout_ms:g} ms of virtual time "
            f"(virtual t={fmt_ms(start)} -> {fmt_ms(self._now)})"
        )

    def _next_boundary(self) -> int:
        return (self._now // self.quantum_ns + 1) * self.quantum_ns

    def _step(self, t: int) -> None:
        msg = proto.run_until(t)
        for n in self.nodes:
            n._send(msg)
        deadline = time.monotonic() + self.wall_timeout_s
        merged = []
        for n in self.nodes:
            mtype, payload = n._recv(f"RUN_UNTIL t={fmt_ms(t)}", deadline)
            if mtype != proto.Msg.STOPPED:
                self._node_gone(n, f"protocol error: expected STOPPED, got message type {mtype}")
            try:
                node_t, events = proto.parse_stopped(payload)
            except proto.ProtocolError as e:
                self._node_gone(n, f"protocol error: {e}")
            n.now_ns = node_t
            merged.extend(n._record_events(events))
        merged.sort(key=lambda e: e[:3])
        self.event_log.extend((t_ns, name, js) for t_ns, _, _, name, js in merged)
        self._now = t

    # ---- lifecycle / failure ----
    def _check_usable(self) -> None:
        if self._dead is not None:
            raise type(self._dead)(f"simulation is no longer usable after: {self._dead}")
        if self._closed:
            raise StestError("simulation is closed")

    def _check_running(self) -> None:
        self._check_usable()
        if not self._started:
            raise StestError("call sim.start() first")

    def _fail(self, make: Callable[[], StestError]) -> NoReturn:
        """Kill and reap every node, then raise make() (built after the kill, so stderr
        tails are complete). The Sim is unusable afterwards."""
        self._kill_all()
        exc = make()
        self._dead = exc
        raise exc

    def _kill_all(self, grace_s: float = 0.5) -> None:
        _LIVE.discard(self)
        for n in self.nodes:
            if n._alive:
                try:
                    n._proc.terminate()
                except ProcessLookupError:
                    pass
        deadline = time.monotonic() + grace_s
        for n in self.nodes:
            n._wait_exit(max(0.0, deadline - time.monotonic()))
        for n in self.nodes:
            n._close_channel()
            n._join_stderr()

    def _tail(self, n: Node) -> str:
        lines = n.stderr_tail
        if not lines:
            return f"\n--- no stderr output from {n.name!r} ---"
        return f"\n--- last {len(lines)} stderr lines of {n.name!r} ---\n" + "\n".join(lines)

    def _node_gone(self, n: Node, detail: str | None) -> NoReturn:
        """The node sent ERROR (detail = its message), closed the channel, or broke the
        protocol. Collect its exit status and stderr, kill everything, raise NodeCrashed."""
        # Let it exit on its own first so the status is its own (a crashing process may
        # take a moment to die: core dump / crash reporter); SIGKILL after that.
        n._wait_exit(10.0 if detail is None else 5.0)
        how = n._exit_description()
        at = f"virtual t={fmt_ms(n.now_ns)}"

        def make() -> StestError:
            if detail is not None:
                head = f"node {n.name!r} aborted at {at}: {detail} ({how})"
            else:
                head = f"node {n.name!r} crashed at {at}: {how}"
            return NodeCrashed(head + self._tail(n))

        self._fail(make)

    def _node_timeout(self, n: Node, what: str) -> NoReturn:
        at = fmt_ms(n.now_ns)
        self._fail(
            lambda: WallClockTimeout(
                f"node {n.name!r} did not answer within {self.wall_timeout_s:g} s of wall-clock time "
                f"(last reported virtual t={at}; waiting for {what})" + self._tail(n)
            )
        )

    def close(self) -> None:
        """SHUTDOWN to every node, SIGKILL whatever is left after 2 s; reaps all."""
        if self._closed:
            return
        self._closed = True
        _LIVE.discard(self)
        for n in self.nodes:
            if n._chan and n._alive:
                try:
                    n._chan.send(proto.shutdown())
                except proto.ChannelClosed:
                    pass
        deadline = time.monotonic() + 2.0
        for n in self.nodes:
            n._wait_exit(max(0.0, deadline - time.monotonic()))
            n._close_channel()
            n._join_stderr()

    def __enter__(self) -> Sim:
        return self

    def __exit__(self, *exc) -> None:
        self.close()
