"""Typed helpers over peripheral models' MODEL_CALL ops (``node.timer(...)``, ...)."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from .errors import StestError

if TYPE_CHECKING:
    from .node import Node


class Timer:
    """A timer model (TIM1..TIM17), e.g. ``node.timer("TIM7")``.

    Every property queries the node (``call(name, "state")``), so values are current as of
    the last completed quantum. See ``chips/stm32l4/models/tim.hpp`` for the state schema.
    """

    def __init__(self, node: Node, name: str):
        self.node = node
        self.name = name

    def state(self) -> dict[str, Any]:
        """{"running", "cnt", "arr", "psc", "clk_hz", "update_hz", "counter", "channels": [...]}"""
        return self.node.call(self.name, "state")

    @property
    def running(self) -> bool:
        return bool(self.state()["running"])

    @property
    def update_hz(self) -> float:
        """Update-event rate (Hz) from the active PSC/ARR/RCR and the timer clock; 0.0 when
        stopped, blocked (ARR = 0) or with UDIS set."""
        return float(self.state()["update_hz"])

    def channel(self, n: int) -> TimerChannel:
        return TimerChannel(self, n)

    def __repr__(self) -> str:
        return f"<Timer {self.node.name}.{self.name}>"


class TimerChannel:
    """Output-compare channel ``n`` (1-based) of a :class:`Timer`."""

    def __init__(self, timer: Timer, n: int):
        self.timer = timer
        self.n = n

    def state(self) -> dict[str, Any]:
        chans = self.timer.state()["channels"]
        if not 1 <= self.n <= len(chans):
            raise StestError(f"{self.timer.name} has no channel {self.n} (it has {len(chans)})")
        return chans[self.n - 1]

    @property
    def enabled(self) -> bool:
        return bool(self.state()["enabled"])

    @property
    def mode(self) -> str:
        return self.state()["mode"]

    @property
    def duty(self) -> float | None:
        """Active fraction of OCx (PWM duty; dead time removed when OCx and OCxN are both
        enabled). None for modes with a static level (frozen, active/inactive on match)."""
        return self.state()["duty"]

    @property
    def ref_duty(self) -> float | None:
        return self.state()["ref_duty"]

    @property
    def duty_n(self) -> float | None:
        """Active fraction of the complementary output OCxN (timers with CHxN only)."""
        return self._comp("duty_n")

    @property
    def dead_time_ns(self) -> float:
        return float(self._comp("dead_time_ns"))

    def _comp(self, key: str) -> Any:
        st = self.state()
        if key not in st:
            raise StestError(f"{self.timer.name} channel {self.n} has no complementary output")
        return st[key]

    def __repr__(self) -> str:
        return f"<TimerChannel {self.timer.node.name}.{self.timer.name} ch{self.n}>"
