"""Simulated CAN bus: arbitration, ACK, queuing and delivery timing on the global clock.

Engine (run by the Sim at every quantum end T, after the node events up to T are known
and device callbacks due by T have run):

    while requests are pending:
        t_idle = max(bus_free_at, earliest t_req)       # bus free = previous end + IFS; a node's
                                                        # requests also wait for its suspend / rejoin
        if t_idle >= T: break                           # later requests (stamped <= that instant) may still join
        aborts with t_abort <= t_idle withdraw their request (no attempt on the wire) and report
            "aborted" at max(t_abort + lookahead, result time of its previous attempt)
        bring every node's fault state up to t_idle (mode changes, bus-off recovery)
        contenders = each sender's top request with t_req <= t_idle (bus-off nodes: none)
                     (node TXFP=0: lowest arbitration key, then lowest key; TXFP=1: lowest seq;
                      devices: lowest arbitration key, then request order)
        winners = every contender with the lowest arbitration key; losers = the others
        ACK = a non-winning node in "normal" mode at SOF (t_idle) and not bus-off, or a non-winning
              device with ack=True
        outcome: winners with different frames -> bit error; else an injected error (inject_errors);
                 else ACK -> ok; else ACK error (see "Errors" below)
        ok:     eof = t_idle + duration; bus_free_at = eof + IFS; winners: tx_result ok at eof
                (request done); listening nodes: rx at eof; devices' on_frame + log append at eof
                (run at the first quantum end >= eof)
        error:  bus_free_at = end of the error frame + IFS; winners: tx_result error at the post
                time; request stays pending (dropped if NART); listening nodes: "error"
        losers: tx_result arb_lost (at the same time); request stays pending (dropped if NART)

The brief's algorithm decides idle instants <= T; this one decides only t_idle < T, because a
request stamped exactly T can still arrive after the engine ran at T (a test's ``send()`` at
``sim.now_ns == T``, a device callback due at T running at a later boundary, a node event at
T reported in the next quantum). Deciding T early would make arbitration depend on where
quantum boundaries fall.

Causality: the Sim caps its quantum at ``lookahead_ns(bitrate)`` (the floor of 44 bit times)
for every bus. Every request is stamped at or after the previous quantum end T_prev, so
every decided frame starts at t_idle >= T_prev and, being at least 45 bits long (one stuff
bit is unavoidable), ends at eof > T_prev + lookahead >= T. Results are therefore always
posted strictly after the current quantum end (never into a node's past) and every frame is
delivered to devices at a later boundary, in EOF order. ``_run`` asserts this.

Bit time: ``frame.duration_ns(bitrate)`` is ceil(bits * 1e9 / bitrate) per frame; the IFS is
ceil(3 * 1e9 / bitrate) ns. Both are exact for bitrates dividing 1e9 (500 k, 250 k, 1 M, ...);
otherwise each rounds up by < 1 ns.

Errors and fault confinement (ISO 11898-1, simplified; the engine is the single owner of the
firmware nodes' error counters and hands the node absolute values with every delivery):

- An attempt fails with an error frame instead of completing:
    "ack"      nobody acknowledged: the error flag starts after the ACK slot (bit_length - 8 bits
               into the frame); transmitter LEC "ack".
    "injected" ``inject_errors()``: the error flag starts right after the arbitration field
               (``CanFrame.arbitration_bits``); transmitter LEC "bit_recessive" (a dominant
               disturbance), receivers LEC "stuff" (they see the error flag).
    "bit"      several senders won arbitration with the same identifier but different control or
               data bits: every transmitter detects a bit error after the first differing bit
               (``CanFrame.bits_to_difference``), LEC "bit_recessive"; receivers "stuff"; all retry.
  Simplification: an error frame occupies the bus for 20 bit times (6-bit flag, up to 6 bits of
  superposed flags, 8-bit delimiter) from the error, then the IFS. Error passive nodes send the
  same (no passive-flag timing). The transmitter's result is posted at max(end of the error
  frame, SOF + lookahead): the lookahead keeps it causal (an injected error in a standard frame
  ends 33-35 bits after SOF, before a quantum end that may be up to 44 bits after SOF).
- Counters (per firmware node): transmit error TEC += 8, except an ACK error while error passive
  (TEC unchanged: a lone node stays at 128); receive error REC += 1 (saturating at 255, the
  register width); successful transmission TEC -= 1; successful reception REC -= 1 (min 0), or
  REC = 120 when it was above 128 (RM0351 bxCAN "Error management": "decremented by 1 or reset
  to 120 if its value was higher than 128"; ISO 11898-1 allows 119..127).
  Error passive: TEC or REC > 127. Bus-off: TEC > 255. Counters change at the attempt (attempts
  never overlap, so this is their real order).
- Participation at SOF: a node in normal mode and not bus-off acknowledges and receives (error
  passive nodes too). A bus-off node neither transmits, acknowledges nor receives; its pending
  requests stay pending (as the bxCAN mailboxes do). An error passive transmitter waits 8 more
  bit times after the IFS following its own transmission (suspend transmission); other senders
  may start at the end of the IFS.
- Bus-off recovery: 128 occurrences of 11 consecutive recessive bits, counted while the node is in
  normal mode: from bus-off when ABOM = 1 (``can_mode`` "abom"), or (ABOM = 0) from the first
  entry into normal mode after an entry into init mode (RM0351: INRQ set then cleared). Leaving
  normal mode stops the count; it restarts at the next normal entry. Counting on a busy bus: each
  transmission (frame or error frame) ends with 11 recessive bits (ACK delimiter + EOF + IFS, or
  error delimiter + IFS) and counts one occurrence at the end of its IFS; idle bus time counts
  one per complete 11 bit times between transmissions; a transmission in progress when the count
  starts counts nothing and the count starts at its IFS end. The 128th occurrence is only known
  once the bus up to it is decided (a frame may still start before it), i.e. at the first
  quantum end at or after it, so the node rejoins (and is told: ``counters`` {tec 0, rec 0})
  exactly one lookahead after the 128th occurrence: quantum-independent and causal. RM0351 asks
  for "at least" the 128 x 11 bits, so this is a delay within the standard.
- ``can_mode`` "reset" (MCR.RESET) zeroes the counters and leaves bus-off; it starts a new
  counter "epoch", sent with every counter so the model drops counters computed before the reset.
- ``Python CanDevice``s are always error active and have no counters (they never go error passive
  or bus-off and retry forever).
- ``CanBus.errors``: {"t_ns", "kind", "sender"} entries in time order, appended at the first
  quantum end at or after t_ns: kinds "ack", "injected", "bit" (t_ns = start of the error flag,
  sender = each transmitter), and the firmware nodes' state changes "error_passive", "bus_off",
  "error_active" (t_ns = the counter change, or the rejoin instant after bus-off recovery).
"""

from __future__ import annotations

import bisect
import functools
import heapq
from typing import TYPE_CHECKING, Any, Callable

from ..errors import StestError
from .device import CanDevice
from .frame import IFS_BITS, CanFrame, lookahead_ns
from .log import LoggedFrame, format_asc, format_candump

if TYPE_CHECKING:
    from ..sim import Sim
    from .dbc import Dbc

MODES = ("sleep", "init", "normal")
ERROR_FRAME_BITS = 20  # 6-bit error flag + up to 6 bits of superposed flags + 8-bit delimiter
SUSPEND_BITS = 8  # error passive transmitter's suspend transmission
RECOVERY_OCCURRENCES = 128  # bus-off recovery: 128 occurrences of ...
RECESSIVE_RUN_BITS = 11  # ... 11 consecutive recessive bits
_TX_LEC = {"ack": "ack", "injected": "bit_recessive", "bit": "bit_recessive"}


# tx_result payloads (fresh dicts per call: receivers may keep or extend them); "seq" is the
# request's seq, so a late result for an aborted request is never taken for its successor's
def _ok(req: _Req) -> dict:
    return {"ok": True, "arb_lost": False, "error": None, "seq": req.seq}


def _arb_lost(req: _Req) -> dict:
    return {"ok": False, "arb_lost": True, "error": None, "seq": req.seq}


def _no_ack(req: _Req) -> dict:
    return {"ok": False, "arb_lost": False, "error": "ack", "seq": req.seq}


def _tx_error(req: _Req, lec: str) -> dict:
    return {"ok": False, "arb_lost": False, "error": lec, "seq": req.seq}


def _aborted(req: _Req) -> dict:
    return {"ok": False, "arb_lost": False, "error": None, "aborted": True, "seq": req.seq}


@functools.lru_cache(maxsize=4096)
def _frame_ns(frame: CanFrame, bitrate: int) -> int:
    return frame.duration_ns(bitrate)


@functools.lru_cache(maxsize=4096)
def _error_bits(frame: CanFrame, kind: str) -> int:
    """Bits from SOF through the bit at which ``kind`` ("ack" | "injected") is detected."""
    return frame.bit_length() - 8 if kind == "ack" else frame.arbitration_bits()


class _Req:
    __slots__ = ("key", "frame", "t_req", "txfp", "seq", "nart", "akey", "abort_t", "last_eof")

    def __init__(self, key: Any, frame: CanFrame, t_req: int, txfp: int, seq: int, nart: bool):
        self.key, self.frame, self.t_req, self.txfp, self.seq, self.nart = key, frame, t_req, txfp, seq, nart
        self.akey = frame.arbitration_key()
        self.abort_t: int | None = None  # time of the abort bound to this request
        self.last_eof = 0  # EOF of its latest attempt


class _DevQueue:
    """A device's / plain sender's requests: a heap by request time, and the ready ones
    (t_req <= the latest idle instant) in a heap by (arbitration key, request order), so a
    decision is O(log n) however long the backlog. Only the ready top is ever removed."""

    __slots__ = ("waiting", "ready", "n")

    def __init__(self) -> None:
        self.waiting: list[tuple] = []
        self.ready: list[tuple] = []
        self.n = 0

    def add(self, req: _Req) -> None:
        heapq.heappush(self.waiting, (req.t_req, req.seq, req))
        self.n += 1

    def earliest(self) -> int:
        # a ready request's t_req is <= the previous idle instant <= bus_free_at, which is
        # all t_idle = max(bus_free_at, earliest) needs
        return self.ready[0][2].t_req if self.ready else self.waiting[0][0]

    def top(self, t_idle: int) -> _Req | None:
        w = self.waiting
        while w and w[0][0] <= t_idle:
            r = heapq.heappop(w)[2]
            heapq.heappush(self.ready, (r.akey, r.key, r))
        return self.ready[0][2] if self.ready else None

    def remove(self, req: _Req) -> None:
        assert self.ready and self.ready[0][2] is req
        heapq.heappop(self.ready)
        self.n -= 1


class _NodeQueue:
    """A node's pending mailbox requests (a handful): scanned per decision."""

    __slots__ = ("reqs",)

    def __init__(self) -> None:
        self.reqs: list[_Req] = []

    @property
    def n(self) -> int:
        return len(self.reqs)

    def add(self, req: _Req) -> None:
        self.reqs.append(req)

    def earliest(self) -> int:
        return min(r.t_req for r in self.reqs)

    def top(self, t_idle: int) -> _Req | None:
        ready = [r for r in self.reqs if r.t_req <= t_idle]
        if not ready:
            return None
        # the TXFP in force at SOF: that of the latest request made by then (never a
        # later-stamped one the engine happens to know already: that would depend on T)
        if max(ready, key=lambda r: r.seq).txfp == 1:
            return min(ready, key=lambda r: (r.seq, r.key))
        return min(ready, key=lambda r: (r.akey, r.key))

    def remove(self, req: _Req) -> None:
        self.reqs.remove(req)


class _NodePort:
    """A firmware node's controller on this bus (see CanBus._attach_node), with its fault
    confinement state (the engine is its only owner; see the module doc)."""

    __slots__ = (
        "name", "on_rx", "on_tx_result", "on_fault", "mode_t", "mode_v", "mode_x", "bitrate", "q",
        "tec", "rec", "epoch", "bus_off", "recover_at", "rec_cursor", "rec_left", "abom", "saw_init",
        "sync_i", "suspend_until", "state", "last_post", "rejoin_entry",
    )

    def __init__(self, name: str, on_rx: Callable, on_tx_result: Callable, on_fault: Callable | None):
        self.name, self.on_rx, self.on_tx_result = name, on_rx, on_tx_result
        self.on_fault = on_fault
        self.mode_t: list[int] = []
        self.mode_v: list[str] = []
        self.mode_x: list[tuple] = []  # (abom | None, reset)
        self.bitrate: int | None = None
        self.q = _NodeQueue()
        self.tec = self.rec = self.epoch = 0
        self.bus_off = False
        self.recover_at: int | None = None  # bus-off: rejoin instant once the recovery is complete
        self.rec_cursor: int | None = None  # bus-off recovery count in progress: counted up to here
        self.rec_left = RECOVERY_OCCURRENCES
        self.abom = False
        self.saw_init = False  # init mode entered since bus-off (ABOM = 0 recovery)
        self.sync_i = 0  # mode history entries applied to the fault state
        # earliest next SOF of its requests: suspend transmission (error passive) after its own
        # transmission; the rejoin instant after bus-off
        self.suspend_until = 0
        self.state = "active"  # as last reported in CanBus.errors
        self.last_post = 0  # latest counter-carrying delivery (they must stay in order)
        self.rejoin_entry: dict | None = None

    def mode_at(self, t: int) -> str:
        i = bisect.bisect_right(self.mode_t, t) - 1
        return self.mode_v[i] if i >= 0 else "sleep"  # bxCAN resets into sleep

    def passive(self) -> bool:
        return self.tec > 127 or self.rec > 127

    def derived_state(self) -> str:
        if self.bus_off and self.recover_at is None:
            return "bus_off"
        return "passive" if self.passive() else "active"

    def fc(self) -> dict:
        return {"tec": self.tec, "rec": self.rec, "epoch": self.epoch}


class CanBus:
    """A simulated classic CAN bus of a [`Sim`][stest.Sim].

    Create with [`Sim.can_bus`][stest.Sim.can_bus] before ``sim.start()``; wire firmware
    nodes with ``sim.node(..., can={"CAN2": bus})`` and Python devices with
    [`attach`][stest.CanBus.attach]. A bus engine decides arbitration, ACK, error frames and
    fault confinement on the global virtual clock, at every quantum end, so every timing is
    deterministic and independent of the quantum. The engine's rules are in the
    [CAN guide](../../can.md) and the module docstring of ``stest/can/bus.py``.

    Senders on a bus: firmware nodes (by node name), attached devices (by device name) and
    plain senders created by [`send`][stest.CanBus.send]; every name is unique per bus.

    Attributes:
        name (str): The bus name.
        bitrate (int): Bit rate in bit/s.
        log (list[LoggedFrame]): Every delivered frame in EOF order. A frame is appended at
            the first quantum end at or after its EOF. Error frames are not logged.
        errors (list[dict]): Error frames and node error-state changes in time order, each
            ``{"t_ns", "kind", "sender"}``; kinds ``"ack"``, ``"injected"``, ``"bit"`` (an
            error frame; ``t_ns`` is the start of the error flag, one entry per
            transmitter) and ``"error_passive"``, ``"bus_off"``, ``"error_active"`` (a
            firmware node's state change). Appended at the first quantum end at or after
            ``t_ns``.
        lookahead_ns (int): The quantum cap this bus imposes, ``lookahead_ns(bitrate)``.
        ifs_ns (int): The 3-bit intermission in ns, rounded up.
    """

    def __init__(self, sim: Sim, name: str, bitrate: int, index: int):
        self._sim = sim
        self.name = name
        self.bitrate = bitrate
        self._index = index
        self.ifs_ns = -(-IFS_BITS * 1_000_000_000 // bitrate)
        self.log: list[LoggedFrame] = []  # delivered frames in EOF order (appended at the boundary after EOF)
        # error frames and node error-state changes (see the module doc), appended at the first
        # quantum end >= t_ns from the heap _err_q
        self.errors: list[dict] = []
        self._err_q: list[tuple[int, int, dict]] = []
        self._nerr = 0
        self._inject: list[list] = []  # [t_from, remaining, target], in call order
        self.w_ns = -(-RECESSIVE_RUN_BITS * 1_000_000_000 // bitrate)  # 11 bit times
        self.suspend_ns = -(-SUSPEND_BITS * 1_000_000_000 // bitrate)
        self._order: dict[str, int] = {}  # sender name -> sender order (registration order)
        self._nodes: dict[str, _NodePort] = {}
        self._devices: dict[str, CanDevice] = {}
        self._queues: dict[str, _DevQueue | _NodeQueue] = {}  # sender -> its pending requests
        self._npending = 0
        self.lookahead_ns = lookahead_ns(bitrate)
        self._aborts: list[tuple[int, int, str, _Req]] = []  # (t, n, sender, request), sorted
        self._nabort = 0
        self._free_at = 0
        self._dseq = 0  # device/plain request counter (their key and seq)
        self._nframes = 0
        self._cursor = 0  # wait_for_frame consumed log[:cursor]

    # ---- public API ----
    def attach(self, device: CanDevice) -> CanDevice:
        """Attach a Python virtual device to the bus.

        The device receives frames whose SOF is at or after the current virtual time and
        can send from then on. A device can be attached to one bus only.

        Args:
            device: The [`CanDevice`][stest.CanDevice] (or subclass) instance.

        Returns:
            ``device``, for chaining (``ecu = bus.attach(MyDevice("ecu"))``).

        Raises:
            TypeError: ``device`` is not a ``CanDevice``.
            StestError: The device is already attached, or the bus already has a sender with
                its name.
        """
        if not isinstance(device, CanDevice):
            raise TypeError(f"attach: expected a CanDevice (got {type(device).__name__})")
        if getattr(device, "bus", None) is not None:
            raise StestError(f"CAN device {device.name!r} is already attached to bus {device.bus.name!r}")
        self._register(device.name, "device")
        device.bus = self
        device._attach_t = self._sim._can_now()
        device._ack = bool(device.ack)  # fixed from attach on (CanDevice refuses later changes)
        self._devices[device.name] = device
        self._queues[device.name] = _DevQueue()
        return device

    def send(self, frame: CanFrame, sender: str = "python") -> None:
        """Request a frame at the current virtual time (``sim.now_ns``).

        Args:
            frame: The [`CanFrame`][stest.CanFrame].
            sender: An attached device's name sends as that device. Any other name
                (default ``"python"``) is a send-only sender, created on first use, that never
                acknowledges and has no error counters (errors are retried forever).

        Raises:
            StestError: ``sender`` is a firmware node on this bus.
            TypeError: ``frame`` is not a ``CanFrame``.
        """
        if sender in self._nodes:
            raise StestError(f"bus {self.name!r}: {sender!r} is a firmware node; it cannot send from Python")
        if sender not in self._devices and sender not in self._order:
            self._register(sender, "plain")
            self._queues[sender] = _DevQueue()
        self._device_send(sender, frame)

    def inject_errors(self, count: int = 1, *, target: str | None = None) -> None:
        """Corrupt upcoming frames with an error frame.

        Affects the next ``count`` frames whose SOF is at or after the current virtual time:
        an error frame right after the arbitration field, a transmit error for the
        transmitter (TEC += 8), a receive error for every receiver (REC += 1), then
        retransmission. Each retransmission is a new frame (32 errors on one frame drive its
        node to bus-off). Calls queue up: a frame takes the first call that applies to it.

        Args:
            count: Number of frames to corrupt (positive ``int``, default 1).
            target: Only corrupt frames sent by this sender (``count`` then counts only its
                frames). ``None`` (default): any sender.

        Raises:
            ValueError: ``count`` is not a positive ``int``.
            StestError: ``target`` is not a sender on this bus.
        """
        if isinstance(count, bool) or not isinstance(count, int) or count < 1:
            raise ValueError(f"inject_errors: count must be a positive integer (got {count!r})")
        if target is not None and target not in self._order:
            known = ", ".join(self._order) or "none"
            raise StestError(f"bus {self.name!r}: inject_errors: no sender named {target!r} (senders: {known})")
        self._inject.append([self._sim._can_now(), count, target])

    def wait_for_frame(
        self,
        *,
        id: int | None = None,
        timeout_ms: float,
        match: Callable[[CanFrame], bool] | None = None,
    ) -> LoggedFrame:
        """Advance the simulation until a new delivered frame matches.

        Only frames not consumed by an earlier ``wait_for_frame`` / ``wait_for_message`` on
        this bus are searched; the call consumes the log up to and including the returned
        frame. Checked now and at every quantum boundary.

        Args:
            id: Identifier to match (``None``: any). Standard and extended frames with the
                same numeric identifier both match; use ``match`` to tell them apart.
            timeout_ms: Budget in virtual milliseconds.
            match: Predicate on the [`CanFrame`][stest.CanFrame] (``None``: any).

        Returns:
            The matching [`LoggedFrame`][stest.LoggedFrame].

        Raises:
            WaitTimeout: No new frame matched within ``timeout_ms``.
        """
        found: list[LoggedFrame] = []

        def cond() -> bool:
            log = self.log
            for i in range(self._cursor, len(log)):
                lf = log[i]
                if (id is None or lf.frame.id == id) and (match is None or match(lf.frame)):
                    self._cursor = i + 1
                    found.append(lf)
                    return True
            return False

        what = f"CAN frame{'' if id is None else f' id {id:#x}'}{' matching predicate' if match else ''} on bus {self.name!r}"
        self._sim.wait_for(cond, timeout_ms, what=what)
        return found[0]

    def messages(self, dbc: Dbc) -> list[tuple[int, str, dict]]:
        """The delivered frames decoded with a DBC.

        Args:
            dbc: The [`Dbc`][stest.Dbc].

        Returns:
            ``(t_ns, message name, signals)`` per frame in EOF order; frames the DBC does
                not know are skipped.

        Raises:
            StestError: A frame matches a message but cannot be decoded.
        """
        out = []
        for lf in self.log:
            d = dbc.decode(lf.frame)
            if d is not None:
                out.append((lf.t_ns, d[0], d[1]))
        return out

    def wait_for_message(
        self,
        dbc: Dbc,
        message: str,
        *,
        timeout_ms: float,
        where: Callable[[dict], bool] | None = None,
    ) -> tuple[int, dict]:
        """Advance the simulation until a new frame of a DBC message arrives.

        Like [`wait_for_frame`][stest.CanBus.wait_for_frame], sharing its consumption cursor.

        Args:
            dbc: The [`Dbc`][stest.Dbc].
            message: Message name.
            timeout_ms: Budget in virtual milliseconds.
            where: Predicate on the decoded signals dict (``None``: any).

        Returns:
            ``(t_ns, signals)``: the frame's EOF time in ns and its decoded signals.

        Raises:
            StestError: Unknown message name.
            WaitTimeout: No such frame within ``timeout_ms``.
        """
        msg = dbc.message(message)
        decoded: list[dict] = []

        def match(frame: CanFrame) -> bool:
            if frame.id != msg.frame_id or frame.ext != msg.is_extended_frame or frame.rtr:
                return False
            d = dbc.decode(frame)
            if d is None or (where is not None and not where(d[1])):
                return False
            decoded.append(d[1])
            return True

        lf = self.wait_for_frame(timeout_ms=timeout_ms, match=match)
        return lf.t_ns, decoded[-1]

    def export_candump(self, path) -> None:
        """Write the whole log in candump format (see
        [`format_candump`][stest.can.format_candump]), with the bus name as the channel.

        Args:
            path (str | os.PathLike): Output file path (overwritten).
        """
        with open(path, "w") as f:
            f.write(format_candump(self.log, self.name))

    def export_asc(self, path) -> None:
        """Write the whole log as Vector ASC (see [`format_asc`][stest.can.format_asc]).

        Args:
            path (str | os.PathLike): Output file path (overwritten). The channel number
                is the bus's creation index + 1.
        """
        with open(path, "w") as f:
            f.write(format_asc(self.log, self._index + 1))

    def __repr__(self) -> str:
        return f"<CanBus {self.name!r} {self.bitrate} bit/s, {len(self.log)} frames>"

    # ---- node-facing API (used by the Sim's firmware-node wiring) ----
    def _attach_node(
        self,
        sender: str,
        on_rx: Callable[[LoggedFrame, int, dict], None],
        on_tx_result: Callable[[Any, dict, int], None],
        on_fault: Callable[[str, dict, int], None] | None = None,
    ) -> None:
        """Register firmware-node controller ``sender`` (it starts in mode "sleep", error
        active). The engine calls, with every t >= the quantum end at which it runs (so
        callbacks may ``Node.post`` at t), and with ``fc`` = the node's counters after the
        attempt ``{"tec", "rec", "epoch"}`` (absolute values; counter-carrying calls are made in
        non-decreasing t):

        - ``on_rx(lf, t, fc)``: a frame this node did not send, whose SOF found it in mode
          normal and not bus-off; t = EOF (after any arb_lost result for the same frame).
        - ``on_tx_result(key, result, t)`` for each attempt of a request:
          ``{"ok": bool, "arb_lost": bool, "error": None | "ack" | "bit_recessive", "seq": int,
          **fc}`` at EOF (ok, arb_lost of a good frame) or at the failed attempt's post time
          (see the module doc); and once for an effective abort, without counters:
          ``{"ok": False, "arb_lost": False, "error": None, "aborted": True, "seq": int}`` (see
          ``_abort``). ``seq`` is the request's.
        - ``on_fault(op, args, t)``: ``("error", {"lec": "stuff", **fc}, t)`` when it received an
          error frame (it was a receiver), and ``("counters", fc, t)`` when it rejoins after
          bus-off recovery (fc = 0, 0)."""
        self._register(sender, "node")
        port = self._nodes[sender] = _NodePort(sender, on_rx, on_tx_result, on_fault)
        self._queues[sender] = port.q

    def _request(
        self, sender: str, key: Any, frame: CanFrame, t_req: int, txfp: int, seq: int, *, nart: bool = False
    ) -> None:
        """Node ``sender`` requests ``frame`` from mailbox ``key`` (an int) at ``t_req``.
        ``txfp`` 0 = the node's mailboxes contend by identifier (then lowest key), 1 = by
        request order (lowest ``seq``); the latest request's txfp applies. ``nart``: one
        attempt only (dropped after a lost arbitration or missing ACK). Requests are only
        made while the node is in normal mode (the engine does not gate on mode). A mailbox
        may be reloaded once its pending request has an abort at or before ``t_req``."""
        port = self._port(sender)
        if not isinstance(frame, CanFrame):
            raise TypeError(f"_request: expected a CanFrame (got {type(frame).__name__})")
        if txfp not in (0, 1):
            raise ValueError(f"_request: txfp must be 0 or 1 (got {txfp!r})")
        t_req = int(t_req)
        if any(r.key == key and (r.abort_t is None or r.abort_t > t_req) for r in port.q.reqs):
            raise StestError(f"bus {self.name!r}: {sender!r} requested mailbox {key!r} while it is still pending")
        port.q.add(_Req(key, frame, t_req, txfp, int(seq), bool(nart)))
        self._npending += 1

    def _abort(self, sender: str, key: Any, t: int) -> None:
        """Abort ``sender``'s pending request ``key`` at ``t``. Calls arrive in the node's
        event order, so this binds to the request made before the abort (with none pending,
        or only an already-aborted one, it has no effect, like ABRQ on an empty mailbox).

        If no attempt of the request is on the wire at ``t`` (t <= the next SOF), the abort
        takes effect: the request is withdrawn and ``on_tx_result(key, {..., "aborted": True,
        "seq": seq}, max(t + lookahead_ns, EOF of its previous attempt))`` is posted.
        Hardware acknowledges the abort at once; it is reported up to 44 bit times late
        (the lookahead) because only then is the decision causal and quantum-independent.
        An attempt already on the wire at ``t`` is not affected: its result arrives at EOF as
        usual (and if it failed, the abort takes effect before the retry)."""
        port = self._port(sender)
        req = next((r for r in port.q.reqs if r.key == key and r.abort_t is None), None)
        if req is None:
            return
        req.abort_t = int(t)
        self._nabort += 1
        bisect.insort(self._aborts, (int(t), self._nabort, sender, req), key=lambda a: a[:2])

    def _set_node_mode(
        self, sender: str, mode: str, bitrate: int | None, t: int, *, abom: bool | None = None, reset: bool = False
    ) -> None:
        """Node ``sender``'s controller entered ``mode`` (sleep|init|normal) at ``t``; it
        acknowledges frames whose SOF is at or after a change to normal and before a change
        away from it. ``bitrate`` is the node's configured bit rate (when entering normal);
        ``abom``: MCR.ABOM from then on (None: unchanged); ``reset``: MCR.RESET (counters zeroed,
        new counter epoch); the controller must have withdrawn (``_abort``) every pending request
        at or before the reset, as the bxCAN model does. Reports arrive in time order, never
        before an instant the engine has already applied (node events of a quantum are handed
        over at its end)."""
        if mode not in MODES:
            raise ValueError(f"_set_node_mode: mode must be one of {', '.join(MODES)} (got {mode!r})")
        port = self._port(sender)
        t = int(t)
        if reset and any(r.abort_t is None or r.abort_t > t for r in port.q.reqs):
            raise StestError(
                f"CAN bus {self.name!r}: {sender!r} reported a controller reset at t={t} ns with requests "
                "still pending (the controller must withdraw them first)"
            )
        i = bisect.bisect_right(port.mode_t, t)
        if i < port.sync_i:
            raise StestError(
                f"CAN bus {self.name!r}: engine bug: {sender!r} mode change at t={t} ns reported after the "
                "engine applied a later one"
            )
        port.mode_t.insert(i, t)
        port.mode_v.insert(i, mode)
        port.mode_x.insert(i, (None if abom is None else bool(abom), bool(reset)))
        if bitrate is not None:
            port.bitrate = int(bitrate)

    # ---- internals ----
    def _register(self, name: str, kind: str) -> None:
        if not name or not isinstance(name, str):
            raise ValueError("sender name must be a non-empty string")
        if name in self._order:
            raise StestError(f"bus {self.name!r} already has a sender named {name!r}")
        self._order[name] = len(self._order)

    def _port(self, sender: str) -> _NodePort:
        port = self._nodes.get(sender)
        if port is None:
            raise StestError(f"bus {self.name!r}: {sender!r} is not an attached node")
        return port

    def _device_send(self, sender: str, frame: CanFrame) -> None:
        if not isinstance(frame, CanFrame):
            raise TypeError(f"send: expected a CanFrame (got {type(frame).__name__})")
        self._dseq += 1
        self._queues[sender].add(_Req(self._dseq, frame, self._sim._can_now(), 0, self._dseq, False))
        self._npending += 1

    def _apply_aborts(self, limit: int, T: int) -> None:
        aborts = self._aborts
        while aborts and aborts[0][0] <= limit:
            t_abort, _, sender, req = aborts.pop(0)
            port = self._nodes[sender]
            if not any(r is req for r in port.q.reqs):
                continue  # completed (or dropped) before the abort: no effect
            port.q.remove(req)
            self._npending -= 1
            t_post = max(t_abort + self.lookahead_ns, req.last_eof)
            if t_post < T:
                raise StestError(
                    f"CAN bus {self.name!r}: engine bug: abort of {sender!r} mailbox {req.key!r} at t={t_abort} ns "
                    f"would be reported at {t_post} ns, before the quantum end {T} ns (into the past)"
                )
            port.on_tx_result(req.key, _aborted(req), t_post)

    def _needs_run(self) -> bool:
        """Whether ``_run`` has work at this quantum end: requests or aborts, a bus-off node
        (its recovery is counted at every quantum end) or a mode change not yet applied."""
        if self._npending or self._aborts:
            return True
        return any(p.bus_off or p.sync_i < len(p.mode_t) for p in self._nodes.values())

    def _earliest(self) -> int | None:
        """The earliest instant a pending request may start (gated by bus-off and suspend)."""
        best = None
        for sender, q in self._queues.items():
            if not q.n:
                continue
            e = q.earliest()
            port = self._nodes.get(sender)
            if port is not None:
                if port.bus_off:
                    if port.recover_at is None:
                        continue
                    e = max(e, port.recover_at)
                e = max(e, port.suspend_until)
            if best is None or e < best:
                best = e
        return best

    def _run(self, T: int) -> None:
        """Decide every bus-idle instant before quantum end ``T`` (see the module doc), then
        bring the nodes' fault state up to T. Cost per decision: O(senders + log(backlog))."""
        while self._npending:
            e = self._earliest()
            if e is None:
                break
            t_idle = max(self._free_at, e)
            if t_idle >= T:
                break
            if self._aborts and self._aborts[0][0] <= t_idle:
                self._apply_aborts(t_idle, T)
                continue
            self._decide(t_idle, T)
        if self._aborts:
            self._apply_aborts(T, T)  # any request not on the wire now starts at >= T
        for port in self._nodes.values():
            if port.bus_off or port.sync_i < len(port.mode_t):
                self._sync(port, T)  # the bus is idle from the last decided transmission up to T

    def _take_injection(self, sof: int, won: set[str]) -> bool:
        for e in self._inject:
            if e[0] <= sof and (e[2] is None or e[2] in won):
                e[1] -= 1
                if e[1] == 0:
                    self._inject.remove(e)
                return True
        return False

    def _decide(self, t_idle: int, T: int) -> None:
        nodes = self._nodes
        for port in nodes.values():
            if port.bus_off or port.sync_i < len(port.mode_t):  # else nothing to bring up to date
                self._sync(port, t_idle)
        tops = []  # (arbitration key, sender order, sender, request, queue)
        for sender, q in self._queues.items():
            if q.n:
                port = nodes.get(sender)
                if port is not None and (port.bus_off or t_idle < port.suspend_until):
                    continue
                top = q.top(t_idle)
                if top is not None:
                    tops.append((top.akey, self._order[sender], sender, top, q))
        if not tops:
            raise StestError(f"CAN bus {self.name!r}: engine bug: no eligible request at idle t={t_idle} ns")
        tops.sort(key=lambda c: c[:2])
        akey = tops[0][0]
        winners = [c for c in tops if c[0] == akey]  # past arbitration: all transmitters of this id
        losers = [c for c in tops if c[0] != akey]
        frame = winners[0][3].frame
        won = {c[2] for c in winners}
        listening = [n for n, p in nodes.items() if n not in won and not p.bus_off and p.mode_at(t_idle) == "normal"]
        ack = bool(listening) or any(
            d._ack and d._attach_t <= t_idle for n, d in self._devices.items() if n not in won
        )
        differ = [c[3].frame for c in winners if c[3].frame != frame]
        if differ:
            kind = "bit"
            err_bits = min(frame.bits_to_difference(g) for g in differ)
        elif self._take_injection(t_idle, won):
            kind = "injected"
            err_bits = _error_bits(frame, kind)
        elif not ack:
            kind = "ack"
            err_bits = _error_bits(frame, kind)
        else:
            kind = None
        br = self.bitrate
        if kind is None:
            eof = t_post = t_idle + _frame_ns(frame, br)
            if eof <= T:
                raise StestError(
                    f"CAN bus {self.name!r}: engine bug: frame {frame} decided at idle t={t_idle} ns ends at "
                    f"{eof} ns, not after the quantum end {T} ns, so its results would be posted into the past "
                    f"(quantum {self._sim.quantum_ns} ns, lookahead {self._sim._can_lookahead_ns()} ns)"
                )
            free = eof + self.ifs_ns
        else:
            t_err = t_idle + -(-err_bits * 1_000_000_000 // br)
            t_end = t_idle + -(-(err_bits + ERROR_FRAME_BITS) * 1_000_000_000 // br)
            t_post = max(t_end, t_idle + self.lookahead_ns)
            if t_post < T:
                raise StestError(
                    f"CAN bus {self.name!r}: engine bug: error in frame {frame} at idle t={t_idle} ns would be "
                    f"reported at {t_post} ns, before the quantum end {T} ns (into the past)"
                )
            free = t_end + self.ifs_ns
        for c in tops:
            c[3].last_eof = t_post  # an abort result never precedes this attempt's result
        for c in winners:
            if kind is None or c[3].nart:
                c[4].remove(c[3])
                self._npending -= 1
        for c in losers:
            if c[3].nart:
                c[4].remove(c[3])
                self._npending -= 1
        self._free_at = free
        # ---- fault confinement (transmitters, then receivers)
        mine = {c[2]: c[3] for c in winners if c[2] in nodes}
        for name in mine:
            port = nodes[name]
            if kind is None:
                port.tec = max(0, port.tec - 1)
            elif not (kind == "ack" and port.passive()):  # ISO: no-ACK while passive keeps TEC
                port.tec += 8
        lost = {c[2]: (c[3], nodes[c[2]].fc()) for c in losers if c[2] in nodes}  # before their RX update
        for name in listening:
            port = nodes[name]
            if kind is None:
                port.rec = 120 if port.rec > 128 else max(0, port.rec - 1)  # RM0351 bxCAN
            else:
                port.rec = min(255, port.rec + 1)
        if kind is not None:
            for c in winners:
                self._note(t_err, kind, c[2])
        t_state = t_post if kind is None else t_err
        for name in mine:
            port = nodes[name]
            if port.tec > 255:
                port.bus_off = True
                port.recover_at = None
                port.saw_init = False
                port.rec_left = RECOVERY_OCCURRENCES
                port.rec_cursor = free if port.abom and port.mode_at(t_idle) == "normal" else None
            port.suspend_until = free + self.suspend_ns if port.passive() and not port.bus_off else 0
            self._note_state(port, t_state)
        for name in listening:
            self._note_state(nodes[name], t_state)
        for port in nodes.values():
            if port.rec_cursor is not None:
                self._count_transmission(port, t_idle, free)
        # ---- deliveries, per node in attach order
        lf = LoggedFrame(t_post, self.name, frame, winners[0][2]) if kind is None else None
        rx = set(listening)
        for name, port in nodes.items():
            if name in mine:
                req = mine[name]
                res = _ok(req) if kind is None else _tx_error(req, _TX_LEC[kind])
                res.update(port.fc())
                self._post_check(port, t_post)
                port.on_tx_result(req.key, res, t_post)
            elif name in lost:
                req, fc = lost[name]
                res = _arb_lost(req)
                res.update(fc)
                self._post_check(port, t_post)
                port.on_tx_result(req.key, res, t_post)
            if name in rx:
                self._post_check(port, t_post)
                if kind is None:
                    port.on_rx(lf, t_post, port.fc())
                elif port.on_fault is not None:
                    port.on_fault("error", {"lec": "stuff", **port.fc()}, t_post)
        if lf is not None:
            self._nframes += 1
            self._sim._can_schedule(t_post, 0, self._index, self._nframes, lambda: self._deliver(lf, won, t_idle))

    # ---- fault confinement helpers
    def _post_check(self, port: _NodePort, t: int) -> None:
        if t < port.last_post:
            raise StestError(
                f"CAN bus {self.name!r}: engine bug: counters for {port.name!r} posted at {t} ns, before an "
                f"earlier delivery at {port.last_post} ns (absolute counters must arrive in order)"
            )
        port.last_post = t

    def _note(self, t: int, kind: str, sender: str) -> dict:
        self._nerr += 1
        e = {"t_ns": t, "kind": kind, "sender": sender}
        heapq.heappush(self._err_q, (t, self._nerr, e))
        return e

    def _note_state(self, port: _NodePort, t: int) -> dict | None:
        st = port.derived_state()
        if st == port.state:
            return None
        port.state = st
        return self._note(t, {"passive": "error_passive", "bus_off": "bus_off", "active": "error_active"}[st], port.name)

    def _flush_errors(self, T: int) -> None:
        q = self._err_q
        while q and q[0][0] <= T:
            e = heapq.heappop(q)[2]
            if not e.pop("_void", False):
                self.errors.append(e)

    def _sync(self, port: _NodePort, t: int) -> None:
        """Bring ``port``'s fault state up to ``t``: mode changes (ABOM, MCR.RESET, recovery
        start/stop) and bus-off recovery over the idle bus, in time order. Every transmission
        starting before ``t`` has been decided (and counted) when this is called."""
        mt = port.mode_t
        while True:
            i = port.sync_i
            nxt = mt[i] if i < len(mt) else None
            lim = t if nxt is None or nxt > t else nxt
            self._recovery_upto(port, lim)
            if nxt is None or nxt > t:
                return
            port.sync_i += 1
            self._mode_change(port, nxt, port.mode_v[i], port.mode_x[i])

    def _recovery_upto(self, port: _NodePort, lim: int) -> None:
        if not port.bus_off:
            return
        if port.recover_at is not None:
            if port.recover_at <= lim:
                port.bus_off = False  # rejoined (counters already 0)
                port.recover_at = None
                port.rejoin_entry = None
            return
        c = port.rec_cursor
        if c is not None and c < lim and (lim - c) // self.w_ns >= port.rec_left:
            self._recovered(port, c + port.rec_left * self.w_ns)  # completed on the idle bus

    def _count_transmission(self, port: _NodePort, sof: int, free: int) -> None:
        """A transmission occupying [sof, free) (free = end of its IFS) during a recovery count."""
        c = port.rec_cursor
        if free <= c:
            return
        if sof < c:  # in progress when the count started: counts nothing
            port.rec_cursor = free
            return
        n = (sof - c) // self.w_ns  # idle bus before it
        if n >= port.rec_left:
            self._recovered(port, c + port.rec_left * self.w_ns)
            return
        port.rec_left -= n + 1  # + its own 11 recessive bits ending at the end of its IFS
        if port.rec_left <= 0:
            self._recovered(port, free)
        else:
            port.rec_cursor = free

    def _recovered(self, port: _NodePort, t_rec: int) -> None:
        """The 128th occurrence completed at ``t_rec`` (known now, at a quantum end >= t_rec):
        the node rejoins, error active with zero counters, one lookahead later."""
        port.recover_at = t_rec + self.lookahead_ns
        port.rec_cursor = None
        port.tec = port.rec = 0
        port.suspend_until = port.recover_at  # its pending requests start no earlier
        self._post_check(port, port.recover_at)
        if port.on_fault is not None:
            port.on_fault("counters", port.fc(), port.recover_at)
        port.rejoin_entry = self._note_state(port, port.recover_at)

    def _mode_change(self, port: _NodePort, t: int, mode: str, x: tuple) -> None:
        abom, reset = x
        if abom is not None:
            port.abom = abom
        if reset:  # MCR.RESET: counters (ESR) reset, bus-off left, a new counter epoch
            if port.rejoin_entry is not None:
                port.rejoin_entry["_void"] = True  # superseded by this reset
                port.rejoin_entry = None
            port.tec = port.rec = 0
            port.bus_off = False
            port.recover_at = port.rec_cursor = None
            port.saw_init = False
            port.suspend_until = t  # (no request is pending: withdrawn before the reset)
            port.epoch += 1
            port.last_post = 0  # deliveries of the old epoch still in flight are dropped by the model
            self._note_state(port, t)
        if not port.bus_off or port.recover_at is not None:
            return
        if mode == "normal":
            if port.rec_cursor is None and (port.abom or port.saw_init):
                port.rec_cursor = max(t, self._free_at)  # a transmission in progress counts nothing
                port.rec_left = RECOVERY_OCCURRENCES
        else:
            port.rec_cursor = None  # the count needs normal mode; it restarts at the next normal entry
            if mode == "init":
                port.saw_init = True

    def _deliver(self, lf: LoggedFrame, won: set[str], sof: int) -> None:
        """At EOF (run at the first quantum end >= EOF): log the frame, call devices. A
        raising on_frame ends the simulation (Sim._device_failed)."""
        self.log.append(lf)
        for name, d in list(self._devices.items()):
            if name not in won and d._attach_t <= sof:
                try:
                    d.on_frame(lf)
                except Exception as e:
                    self._sim._device_failed(d.name, "on_frame", e)
