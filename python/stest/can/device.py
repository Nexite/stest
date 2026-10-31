"""Python virtual CAN devices."""

from __future__ import annotations

from typing import TYPE_CHECKING, Callable

from ..errors import StestError
from .frame import CanFrame
from .log import LoggedFrame

if TYPE_CHECKING:
    from .bus import CanBus


class CanDevice:
    """A virtual CAN device implemented in Python. Subclass it (calling
    ``super().__init__(name)``) and override :meth:`on_frame`; attach with
    ``bus.attach(device)``.

    Device code runs on the virtual clock: :meth:`on_frame` runs at the frame's end of frame
    (EOF) and an :meth:`every` callback at its due time. Inside a callback :attr:`now_ns` is
    that instant and :meth:`send` requests the frame at it, so device timing never depends
    on the quantum. (Callbacks are executed at the first quantum end at or after their time;
    the scheduler's lookahead guarantees no arbitration after that instant has been decided
    yet.) Outside callbacks, :attr:`now_ns` is the simulation time (``sim.now_ns``).

    ``ack`` (default True): the device acknowledges frames, like a real controller in normal
    mode. With ``ack=False`` it only listens; a frame nobody else acknowledges is retried
    forever, as on a real bus.
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
        """Called for every frame delivered on the bus (except the device's own), at the
        frame's EOF (``self.now_ns == lf.t_ns``), in EOF order across all buses."""

    @property
    def now_ns(self) -> int:
        return self._require_bus()._sim._can_now()

    def send(self, frame: CanFrame) -> None:
        """Request ``frame`` on the device's bus at the current virtual time (:attr:`now_ns`).
        Queued without limit; the device's pending frames contend in identifier order."""
        bus = self._require_bus()
        bus._device_send(self.name, frame)

    def every(self, period_ms: float, fn: Callable[[], None]) -> None:
        """Run ``fn()`` every ``period_ms`` of virtual time, first at ``now + period`` (exact
        integer-ns multiples of the period from the registration time)."""
        bus = self._require_bus()
        period = round(period_ms * 1_000_000)
        if period <= 0:
            raise ValueError("every: period_ms must be positive (at least 1 ns)")
        bus._sim._can_every(period, fn)

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
