"""Sim: owns the global virtual clock and advances all nodes in lockstep quanta."""

from __future__ import annotations

import atexit
import heapq
import logging
import os
import time
from typing import TYPE_CHECKING, Callable, NoReturn

from . import protocol as proto
from .errors import NodeCrashed, StestError, WaitTimeout, WallClockTimeout
from .node import Node

if TYPE_CHECKING:
    from .can.bus import CanBus

_log = logging.getLogger("stest")

# Every Sim that has spawned nodes and not been closed. If the interpreter exits without
# the Sim being closed (or a test run is interrupted), the atexit handler kills and reaps
# every node so none is left spinning in firmware.
_LIVE: set[Sim] = set()  # strong refs: a Sim dropped without close() must still be killed at exit


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

    Typical use: create nodes (and CAN buses), call [`start`][stest.Sim.start], then advance
    time with [`advance`][stest.Sim.advance] or one of the ``wait_*`` helpers, and
    [`close`][stest.Sim.close] at the end (or use the Sim as a context manager; the pytest
    ``sim`` fixture closes it for you).

    ```python
    with Sim() as sim:
        node = sim.node("app", "build/selftest/blinky")
        sim.start()
        node.pin("PA5").wait_high(timeout_ms=600)
    ```

    Scheduler: each quantum, every node is told to run until the quantum end, then the
    harness waits for all of them (nodes run concurrently in between). Quantum boundaries
    lie on the absolute grid ``k * quantum``. Events are merged into
    ``event_log`` by (time, node creation index, per-node order), so identical inputs give
    an identical log. Python sees the nodes only between quanta: waits are checked at
    quantum boundaries.

    Failure: every blocking read of a node is bounded by ``wall_timeout_s`` of real time.
    When a node crashes, aborts or times out, every node is killed and reaped before the
    exception ([`NodeCrashed`][stest.NodeCrashed] or [`WallClockTimeout`][stest.WallClockTimeout])
    propagates. The Sim is then unusable: later calls raise the same exception type again.

    CAN: [`can_bus`][stest.Sim.can_bus] creates buses before ``start()``; the quantum is then
    capped at the shortest frame time of the fastest bus
    ([`lookahead_ns`][stest.can.lookahead_ns]) so the bus engine, run at every quantum end,
    never posts a result into a node's past. A Sim with buses but no nodes is valid (Python
    devices only).

    The ``poll_skip``, ``strict_analog`` and ``debugger_attached`` arguments are passed to
    every node as environment variables at ``start()``; left at ``None``, the node inherits
    the variable from the Python process's environment (or a node's ``env=``), else the
    runtime default applies. A value given here overrides both.

    Args:
        quantum_us: Lockstep quantum in virtual microseconds (default 1000). Smaller
            quanta make waits stop closer to the event that satisfied them, at a cost in
            speed. Capped by CAN buses (see [`quantum_ns`][stest.Sim.quantum_ns]).
        wall_timeout_s: Real-time limit in seconds for every blocking wait on a node
            (default 30). A node that does not answer in time ends the simulation with
            [`WallClockTimeout`][stest.WallClockTimeout].
        access_cost_ns: Virtual time in ns charged for each firmware MMIO access (default
            25). Node env ``STEST_ACCESS_COST_NS``; always set from this argument.
        poll_skip: Poll fast-forward threshold: after this many store-free loads re-reading
            already-seen values, a node jumps its clock to the next scheduled event or the
            quantum end. ``0`` disables it. ``None`` (default): ``STEST_POLL_SKIP`` from the
            environment, else 64.
        strict_analog: ``True``: converting an ADC channel with no input voltage aborts the
            node. ``False``: it reads 0 V with one stderr warning per (ADC, channel).
            ``None`` (default): ``STEST_STRICT_ANALOG`` from the environment (``"1"`` means
            strict), else not strict.
        debugger_attached: ``False``: the bare-board power-on state (ITM, ``DEMCR.TRCENA``
            and ``DHCSR.C_DEBUGEN`` off, ITM software-locked), so CMSIS ``ITM_SendChar``
            drops output. ``True``: a debugger is attached. ``None`` (default):
            ``STEST_DEBUGGER`` from the environment (only ``"0"`` detaches), else attached.

    Raises:
        ValueError: ``quantum_us`` or ``wall_timeout_s`` is not positive, or
            ``access_cost_ns`` or ``poll_skip`` is negative.

    Attributes:
        nodes (list[Node]): The nodes, in creation order.
        event_log (list[tuple[int, str, str]]): Every node event so far as ``(t_ns, node
            name, event JSON text)``, merged in deterministic order, as of the last
            completed quantum. JSON keys are sorted; parse the text with ``json.loads``
            rather than matching it.
        wall_timeout_s (float): The real-time limit given to the constructor.
        access_cost_ns (int): The MMIO access cost given to the constructor.
        poll_skip (int | None): As given to the constructor.
        strict_analog (bool | None): As given to the constructor.
        debugger_attached (bool | None): As given to the constructor.
    """

    def __init__(self, quantum_us: int = 1000, wall_timeout_s: float = 30.0, access_cost_ns: int = 25,
                 poll_skip: int | None = None, strict_analog: bool | None = None,
                 debugger_attached: bool | None = None):
        if quantum_us <= 0:
            raise ValueError("quantum_us must be positive")
        if wall_timeout_s <= 0:
            raise ValueError("wall_timeout_s must be positive")
        if access_cost_ns < 0:
            raise ValueError("access_cost_ns must be >= 0")
        self._quantum_ns = int(quantum_us) * 1000
        self._quantum_requested_ns = self._quantum_ns
        self.wall_timeout_s = float(wall_timeout_s)
        self.access_cost_ns = int(access_cost_ns)
        if poll_skip is not None and poll_skip < 0:
            raise ValueError("poll_skip must be >= 0 (0 disables poll fast-forward)")
        self.poll_skip = poll_skip
        # True: converting an ADC channel with no input voltage aborts the node (else it
        # reads 0 V with one stderr warning). None: inherit STEST_STRICT_ANALOG.
        self.strict_analog = strict_analog
        # False: power-on state of an unattached board (ITM/TRCENA/C_DEBUGEN all off, so CMSIS
        # ITM_SendChar drops output). None/True: a debugger is attached (STEST_DEBUGGER unset
        # or any value but "0"; the default).
        self.debugger_attached = debugger_attached
        self.nodes: list[Node] = []
        # Merged event log: (t_ns, node name, event JSON text), in merge order.
        self.event_log: list[tuple[int, str, str]] = []
        self._now = 0
        self._started = False
        self._closed = False
        self._dead: StestError | None = None
        self._merging: list = []  # this quantum's event merge keys not yet in event_log
        # CAN (see stest.can.bus): buses in creation order; hooks run at each quantum end T
        # before the engine (node CAN event wiring feeds requests there); a heap of device
        # callbacks (t, class 0 frame delivery | 1 timer, k1, k2, payload) run in that order
        # at the first quantum end >= t, with _can_cb_t = t (the device's "now").
        self._buses: list[CanBus] = []
        self._can_hooks: list[Callable[[int], None]] = []
        self._can_queue: list[tuple] = []
        self._can_cb_t: int | None = None
        self._can_timers = 0

    @property
    def quantum_ns(self) -> int:
        """The lockstep quantum in virtual ns (read-only).

        ``quantum_us * 1000`` as constructed, lowered by [`can_bus`][stest.Sim.can_bus] to
        the bus lookahead when that is shorter (an INFO message on the ``stest`` logger
        reports the cap at ``start()``).
        """
        return self._quantum_ns

    @property
    def quantum_us(self) -> int:
        """The lockstep quantum in whole virtual microseconds (``quantum_ns // 1000``,
        rounded down when a CAN bus capped the quantum to a non-multiple of 1 us)."""
        return self.quantum_ns // 1000

    # ---- setup ----
    def node(
        self, name: str, exe: str | os.PathLike, env: dict | None = None, can: dict | None = None
    ) -> Node:
        """Add a firmware node; it is spawned by [`start`][stest.Sim.start].

        Args:
            name: Unique node name (used in events, error messages and as the node's CAN
                sender name).
            exe: Path to the firmware executable built with ``stest_add_firmware``.
            env: Extra environment variables for the node process, on top of the Python
                process's environment. The Sim's own settings (``poll_skip``, ...) override
                them when not ``None``.
            can: Wires the node's CAN controllers to buses of this Sim, e.g.
                ``{"CAN2": bus}`` (a [`CanBus`][stest.CanBus] or a bus name); one controller
                per bus. On each bus the node is the sender ``name``
                ([`LoggedFrame.sender`][stest.LoggedFrame]). Controller names are checked
                against the firmware's chip at ``start()``.

        Returns:
            The new [`Node`][stest.Node].

        Raises:
            StestError: Called after ``start()``; a duplicate node name; an unknown bus name
                or a bus of another Sim; two controllers on one bus; a bus that already has
                a sender called ``name``.
        """
        if self._started:
            raise StestError("nodes must be created before sim.start()")
        if any(n.name == name for n in self.nodes):
            raise StestError(f"duplicate node name {name!r}")
        wiring = []
        for periph, b in (can or {}).items():
            bus = self._resolve_bus(b)
            if any(w[1] is bus for w in wiring):
                raise StestError(f"node {name!r}: two CAN controllers on bus {bus.name!r}")
            if name in bus._order:
                raise StestError(f"node {name!r}: bus {bus.name!r} already has a sender named {name!r}")
            wiring.append((str(periph), bus))
        n = Node(self, len(self.nodes), name, exe, env)
        self.nodes.append(n)
        for periph, bus in wiring:
            n.can[periph] = bus
            bus._attach_node(name, *_node_can_callbacks(n, periph))
        if wiring:
            self._can_hooks.append(n._can_flush)  # node index order: deterministic
        return n

    def can_bus(self, name: str, bitrate: int = 500_000) -> CanBus:
        """Create a CAN bus; caps the quantum at ``lookahead_ns(bitrate)``.

        Args:
            name: Unique bus name (used in logs, waits and ``Sim.node(can=...)``).
            bitrate: Bit rate in bit/s, an ``int`` in 1..1_000_000 (default 500_000).
                Firmware nodes must configure exactly this rate.

        Returns:
            The new [`CanBus`][stest.CanBus].

        Raises:
            StestError: Called after ``start()``, or a duplicate bus name.
            ValueError: ``bitrate`` is not an ``int`` in 1..1_000_000.
        """
        from .can.bus import CanBus
        from .can.frame import lookahead_ns

        if self._started:
            raise StestError("CAN buses must be created before sim.start()")
        if any(b.name == name for b in self._buses):
            raise StestError(f"duplicate CAN bus name {name!r}")
        if isinstance(bitrate, bool) or not isinstance(bitrate, int) or not 0 < bitrate <= 1_000_000:
            raise ValueError(f"CAN bitrate must be an int in 1..1000000 bit/s (got {bitrate!r})")
        bus = CanBus(self, name, bitrate, len(self._buses))
        self._buses.append(bus)
        self._quantum_ns = min(self._quantum_ns, lookahead_ns(bitrate))
        return bus

    def _resolve_bus(self, b) -> CanBus:
        from .can.bus import CanBus

        if isinstance(b, CanBus):
            if b._sim is not self:
                raise StestError(f"CAN bus {b.name!r} belongs to another Sim")
            return b
        for bus in self._buses:
            if bus.name == b:
                return bus
        raise StestError(f"no CAN bus named {b!r} (create it with sim.can_bus() first)")

    def _can_lookahead_ns(self) -> int | None:
        from .can.frame import lookahead_ns

        return min((lookahead_ns(b.bitrate) for b in self._buses), default=None)

    def start(self) -> None:
        """Spawn every node process and wait until each is ready (at virtual time 0).

        Loads each node's debug symbols for [`Node.var`][stest.Node.var] (a missing DWARF
        is reported on the first ``var()`` call, not here) and checks CAN wiring.

        Raises:
            StestError: Called twice; no nodes and no CAN buses; a node executable does not
                exist; a ``can=`` controller name that is not a CAN controller of the
                node's chip (all nodes are then killed).
            NodeCrashed: A node failed to start or speaks another protocol version.
            WallClockTimeout: A node did not report ready within ``wall_timeout_s``.
        """
        if self._started:
            raise StestError("sim.start() called twice")
        if not self.nodes and not self._buses:
            raise StestError("sim.start() with no nodes (and no CAN buses)")
        self._started = True
        cap = self._can_lookahead_ns()
        if cap is not None and self._quantum_ns > cap:  # can_bus() caps it; keep the invariant explicit
            raise StestError(f"internal: quantum {self._quantum_ns} ns exceeds the CAN lookahead {cap} ns")
        if self.quantum_ns < self._quantum_requested_ns:
            limit = min(self._buses, key=lambda b: (-b.bitrate, b._index))
            _log.info(
                "stest: quantum capped from %d ns to %d ns by CAN bus %r (%d bit/s): the quantum must "
                "not exceed the shortest frame time",
                self._quantum_requested_ns, self.quantum_ns, limit.name, limit.bitrate,
            )
        _LIVE.add(self)
        extra = {"STEST_ACCESS_COST_NS": str(self.access_cost_ns)}
        if self.poll_skip is not None:
            extra["STEST_POLL_SKIP"] = str(int(self.poll_skip))
        if self.strict_analog is not None:
            extra["STEST_STRICT_ANALOG"] = "1" if self.strict_analog else "0"
        if self.debugger_attached is not None:
            extra["STEST_DEBUGGER"] = "1" if self.debugger_attached else "0"
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
                self._node_gone(
                    n,
                    f"node speaks control protocol version {version}, but this stest Python expects "
                    f"version {proto.PROTO_VERSION} (rebuild the firmware with the matching stest runtime)",
                )
            n.proto_version, n.anchor_addr = version, anchor
            n._load_symbols()
            if n.can:
                self._can_check_wiring(n)

    # ---- time ----
    @property
    def now_ns(self) -> int:
        """Global virtual time in ns: where the last [`advance`][stest.Sim.advance] or wait
        stopped (a quantum boundary, or the exact end of an ``advance`` / wait timeout)."""
        return self._now

    def advance(self, *, ms: float = 0, us: float = 0, ns: int = 0) -> None:
        """Advance every node by a virtual duration (the sum of the arguments).

        Runs quantum by quantum and stops at exactly ``now_ns + duration``, which need not
        be a quantum boundary (the last step is then a partial quantum).

        Args:
            ms: Virtual milliseconds (float; rounded to the nearest ns).
            us: Virtual microseconds (float; rounded to the nearest ns).
            ns: Virtual nanoseconds (int).

        Raises:
            ValueError: The total is negative.
            StestError: ``start()`` was not called, or the Sim is closed.
            NodeCrashed: A node crashed or aborted (strict mode) meanwhile.
            WallClockTimeout: A node did not answer within ``wall_timeout_s``.
        """
        dur = round(ms * 1_000_000) + round(us * 1_000) + int(ns)
        if dur < 0:
            raise ValueError("cannot advance by a negative duration")
        self._check_running()
        end = self._now + dur
        while self._now < end:
            self._step(min(self._next_boundary(), end))

    def wait_for(self, cond: Callable[[], bool], timeout_ms: float, *, what: str | None = None) -> None:
        """Advance quantum by quantum until ``cond()`` returns true.

        ``cond`` is checked now (returning at once if true), then at every quantum boundary
        and at the deadline. Firmware state can only be observed between quanta, so the
        wait stops at the end of the quantum in which the condition became true.

        Args:
            cond: A callable with no arguments, e.g. ``lambda: node.var("ready")``.
            timeout_ms: Budget in virtual milliseconds.
            what: Description of the condition for the timeout message (default: the
                callable's ``__name__``).

        Raises:
            WaitTimeout: ``cond()`` stayed false for ``timeout_ms``; ``now_ns`` is then
                exactly start + ``timeout_ms``.
            StestError: ``start()`` was not called, or the Sim is closed.
            NodeCrashed: A node crashed or aborted meanwhile.
            WallClockTimeout: A node did not answer within ``wall_timeout_s``.
        """
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
        plain = None  # shared RUN_UNTIL for nodes with an empty inbox
        for n in self.nodes:
            if n._inbox:
                msg = proto.run_until(t, n._inbox)
                n._inbox, n._inbox_bytes = [], 0
            else:
                msg = plain = plain or proto.run_until(t)
            n._send(msg)
        deadline = time.monotonic() + self.wall_timeout_s
        merged = self._merging = []
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
        self._flush_events()
        for n in self.nodes:
            if n._can_unwired is not None:
                self._can_unwired_tx(n, *n._can_unwired)
        self._now = t
        if self._buses:
            self._can_boundary(t)

    # ---- CAN scheduling (see stest.can.bus) ----
    def _can_boundary(self, T: int) -> None:
        """At quantum end T: node CAN events -> requests (hooks), device callbacks due by T
        (frame deliveries and timers, in time order), then each bus's engine."""
        for h in self._can_hooks:
            h(T)
        if self._can_queue:
            self._run_can_queue(T)
        for b in self._buses:
            if b._needs_run():
                b._run(T)
            if b._err_q:
                b._flush_errors(T)

    def _run_can_queue(self, T: int) -> None:
        q = self._can_queue
        while q and q[0][0] <= T:
            t, cls, k1, _, payload = heapq.heappop(q)
            self._can_cb_t = t
            try:
                if cls == 0:
                    payload()  # CanBus._deliver: reports a raising on_frame itself
                else:
                    period, fn, dev = payload
                    heapq.heappush(q, (t + period, 1, k1, 0, payload))
                    try:
                        fn()
                    except Exception as e:
                        self._device_failed(dev, "every", e)
            finally:
                self._can_cb_t = None

    def _can_unwired_tx(self, n: Node, t: int, periph: str) -> NoReturn:
        """Node ``n``'s controller ``periph`` requested a transmission (can_tx) but the test did
        not wire it to a bus: strict, like any other unmodelled use (NodeCrashed, all nodes
        killed). Mode changes of unwired controllers are fine (firmware may initialise a
        controller the test does not use)."""
        at = fmt_ms(t)
        self._fail(
            lambda: NodeCrashed(
                f"node {n.name!r} {periph} requested a transmission but is not wired to a bus: "
                f"Sim.node({n.name!r}, ..., can={{{periph!r}: bus}}) (at virtual t={at})"
            )
        )

    def _can_check_wiring(self, n: Node) -> None:
        """After HELLO: every controller name in ``n.can`` must be a CAN controller model of
        the node (its ``can_info`` model op). Else StestError naming the valid ones."""
        bad = []
        for periph in n.can:
            try:
                n.call(periph, "can_info")
            except StestError:
                bad.append(periph)
        if not bad:
            return
        valid = []
        for name in [f"{p}{i}" for p in ("CAN", "FDCAN") for i in range(1, 4)]:
            try:
                n.call(name, "can_info")
                valid.append(name)
            except StestError:
                pass
        periph = bad[0]
        msg = (
            f"node {n.name!r}: can={{{periph!r}: ...}}: {periph} is not a CAN controller of this firmware's chip "
            f"(CAN controllers: {', '.join(valid) or 'none'})"
        )
        self._fail(lambda: StestError(msg))

    def _can_bitrate_mismatch(self, n: Node, periph: str, bus: CanBus, bitrate, t: int) -> NoReturn:
        """Node ``n``'s controller entered normal mode at ``t`` with a bit rate other than its
        bus's. On hardware such a node only sees error frames (and drives them onto the bus);
        stest treats it as a configuration error and ends the simulation like a strict abort:
        every node killed, NodeCrashed (the same type as a node's own strict abort, so tests
        handle every "this firmware cannot run here" failure alike). The comparison is exact:
        the model's integer bit rate (PCLK1 // (BRP+1)(1+TS1+1+TS2+1)) against
        ``CanBus.bitrate``, with no tolerance."""
        at = fmt_ms(t)

        def make() -> StestError:
            return NodeCrashed(
                f"node {n.name!r} {periph} entered normal mode at virtual t={at}: bit rate {bitrate} does not "
                f"match bus {bus.name!r} ({bus.bitrate}) (check the CAN bit timing: Prescaler, BS1, BS2, PCLK1)"
            )

        self._fail(make)

    def _device_failed(self, device: str, what: str, e: Exception) -> NoReturn:
        """A device callback raised mid-boundary: the buses' state is no longer consistent
        with virtual time, so the simulation ends (every node killed), like NodeCrashed."""
        if isinstance(e, StestError) and self._dead is e:
            raise e  # already ended (e.g. a callback called advance() and that failed)

        def make() -> StestError:
            exc = StestError(f"CAN device {device!r} {what} callback raised: {type(e).__name__}: {e}")
            exc.__cause__ = e
            return exc

        self._fail(make)

    def _can_schedule(self, t: int, cls: int, k1: int, k2: int, payload) -> None:
        heapq.heappush(self._can_queue, (t, cls, k1, k2, payload))

    def _can_every(self, period_ns: int, fn: Callable[[], None], device: str) -> None:
        self._can_timers += 1
        heapq.heappush(
            self._can_queue, (self._can_now() + period_ns, 1, self._can_timers, 0, (period_ns, fn, device))
        )

    def _can_now(self) -> int:
        """Virtual time for device sends: the running callback's time, else now_ns."""
        return self._now if self._can_cb_t is None else self._can_cb_t

    def _flush_events(self, extra: list | None = None) -> None:
        """Merge this quantum's recorded events (plus ``extra``: a crashing node's ERROR
        events) into event_log by (t_ns, node index, per-node sequence)."""
        merged, self._merging = self._merging + (extra or []), []
        merged.sort(key=lambda e: e[:3])
        self.event_log.extend((t_ns, name, js) for t_ns, _, _, name, js in merged)

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
        """Shut every node down and reap it (idempotent).

        Each node is asked to exit; whatever is still running after 2 s of real time is
        killed. Logs, ``event_log`` and pin histories stay readable afterwards. A Sim that
        is never closed is cleaned up when the interpreter exits.
        """
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


def _node_can_callbacks(n: Node, periph: str):
    """Engine result hooks for node ``n``'s controller ``periph``: inbox messages for the
    model's ``deliver()`` ops ``rx`` {id, ext, rtr, dlc, data (hex), tec, rec, epoch},
    ``tx_result`` {mailbox, ok, arb_lost, error, seq[, aborted][, tec, rec, epoch]}, ``error``
    {lec, tec, rec, epoch} and ``counters`` {tec, rec, epoch}, applied at exactly the engine's
    times (see stest.can.bus)."""

    def on_rx(lf, t: int, fc: dict | None = None) -> None:
        f = lf.frame
        args = {"id": f.id, "ext": f.ext, "rtr": f.rtr, "dlc": f.dlc, "data": f.data.hex()}
        if fc is not None:
            args.update(fc)
        n.post(periph, "rx", args, t)

    def on_tx_result(key, result: dict, t: int) -> None:
        n.post(periph, "tx_result", {"mailbox": key, **result}, t)

    def on_fault(op: str, args: dict, t: int) -> None:
        n.post(periph, op, args, t)

    return on_rx, on_tx_result, on_fault
