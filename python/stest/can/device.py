"""Python virtual CAN devices."""

from __future__ import annotations

from typing import TYPE_CHECKING, Callable

from ..errors import StestError
from .frame import CanFrame
from .log import LoggedFrame

if TYPE_CHECKING:
    from .bus import CanBus


class CanDevice:
    """A virtual CAN device implemented in Python.

    Subclass it (calling ``super().__init__(name)``), override
    [`on_frame`][stest.CanDevice.on_frame], and attach it with
    [`bus.attach(device)`][stest.CanBus.attach]:

    ```python
    class Echo(CanDevice):
        def on_frame(self, lf):
            if lf.frame.id == 0x1F0:
                self.send(CanFrame(0x1F1, lf.frame.data))

    bus.attach(Echo("echo"))
    ```

    Device code runs on the virtual clock: ``on_frame`` runs at the frame's end of frame
    (EOF) and an [`every`][stest.CanDevice.every] callback at its due time. Inside a callback
    [`now_ns`][stest.CanDevice.now_ns] is that instant and [`send`][stest.CanDevice.send]
    requests the frame at it, so device timing never depends on the quantum. (Callbacks are executed at the first quantum end at or after their time;
    the scheduler's lookahead guarantees no arbitration after that instant has been decided
    yet.) Outside callbacks, ``now_ns`` is the simulation time (``sim.now_ns``).

    ``ack`` (default True): the device acknowledges frames, like a real controller in normal
    mode. With ``ack=False`` it only listens; a frame nobody else acknowledges is retried
    forever, as on a real bus. ``ack`` is fixed once the device is attached.

    Devices are always error active and keep no error counters: errors on their frames
    (no ACK, ``CanBus.inject_errors``, same-identifier collisions) are retried at once, forever,
    and they never become error passive or bus-off. Only firmware nodes have fault confinement.

    An exception raised by ``on_frame`` or an ``every`` callback ends the simulation with a
    [`StestError`][stest.StestError] naming the device (the original exception chained).

    Args:
        name: Device name, unique among the bus's senders (its frames'
            ``LoggedFrame.sender``).
        ack: Whether the device acknowledges frames. ``None`` (default) keeps the class
            attribute ``ack`` (``True`` unless a subclass overrides it).

    Raises:
        ValueError: ``name`` is empty or not a string.

    Attributes:
        name (str): The device name.
        ack (bool): Whether the device acknowledges frames; read-only once attached
            (setting it then raises [`StestError`][stest.StestError]).
        bus (CanBus | None): The bus the device is attached to, or ``None``.
    """

    ack: bool = True

    def __init__(self, name: str, *, ack: bool | None = None):
        if not name or not isinstance(name, str):
            raise ValueError("CanDevice name must be a non-empty string")
        self.name = name
        if ack is not None:
            self.ack = bool(ack)
        self.bus: CanBus | None = None
        self._attach_t = 0

    def on_frame(self, lf: LoggedFrame) -> None:
        """Called for every frame delivered on the bus except the device's own.

        Runs at the frame's EOF (``self.now_ns == lf.t_ns``), in EOF order across all buses.
        The default does nothing; override it.

        Args:
            lf: The delivered frame.
        """

    @property
    def now_ns(self) -> int:
        """The device's virtual time in ns: the running callback's instant, else
        ``sim.now_ns``.

        Raises:
            StestError: The device is not attached to a bus.
        """
        return self._require_bus()._sim._can_now()

    def send(self, frame: CanFrame) -> None:
        """Request a frame on the device's bus at [`now_ns`][stest.CanDevice.now_ns].

        Requests queue without limit; the device's pending frames contend in arbitration
        order (the frame that would win on the wire first, then request order). Errors are retried
        forever.

        Args:
            frame: The [`CanFrame`][stest.CanFrame] to send.

        Raises:
            StestError: The device is not attached to a bus.
            TypeError: ``frame`` is not a ``CanFrame``.
        """
        bus = self._require_bus()
        bus._device_send(self.name, frame)

    def every(self, period_ms: float, fn: Callable[[], None]) -> None:
        """Run ``fn()`` periodically on the virtual clock.

        First at ``now_ns + period``, then at exact integer-ns multiples of the period from
        the registration time. There is no way to cancel it.

        Args:
            period_ms: Period in virtual milliseconds (rounded to the nearest ns).
            fn: Callable with no arguments; it may call [`send`][stest.CanDevice.send].

        Raises:
            StestError: The device is not attached to a bus.
            ValueError: The period rounds to less than 1 ns.
        """
        bus = self._require_bus()
        period = round(period_ms * 1_000_000)
        if period <= 0:
            raise ValueError("every: period_ms must be positive (at least 1 ns)")
        bus._sim._can_every(period, fn, self.name)

    def __setattr__(self, name: str, value) -> None:
        if name == "ack" and getattr(self, "bus", None) is not None:
            raise StestError(
                f"CAN device {self.name!r}: ack cannot change after attach (the engine would see the "
                "change at a quantum-dependent time)"
            )
        object.__setattr__(self, name, value)

    def _require_bus(self) -> CanBus:
        bus = getattr(self, "bus", None)
        if bus is None:
            raise StestError(
                f"CAN device {getattr(self, 'name', '?')!r} is not attached to a bus (bus.attach(device) first)"
            )
        return bus

    def __repr__(self) -> str:
        bus = getattr(self, "bus", None)
        return f"<{type(self).__name__} {self.name!r} on {bus.name if bus else None!r}>"
