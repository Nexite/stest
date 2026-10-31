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


class Adc:
    """ADC ``n`` (1-based) of the chip's ADC model (``node.adc(1)``).

    Inputs are pin voltages: ``set(channel, volts)`` sets the voltage of the pin wired to that
    channel (part TOML ``[[analog]]`` table), so every ADC sharing the pin sees it, exactly
    like ``node.pin("PB1").set_voltage(volts)``. Works whatever the firmware's RCC clock state
    (the outside world drives the pin). Values are clamped to 0..VREF+ when converted.
    """

    MODEL = "ADC"

    def __init__(self, node: Node, n: int):
        self.node = node
        self.n = int(n)

    def set(self, channel: int, volts: float) -> None:
        """Drive the pin of ``channel`` to ``volts``. StestError if the channel has no pin."""
        self.node.call(self.MODEL, "set_input", adc=self.n, channel=int(channel), volts=float(volts))

    def get(self, channel: int) -> float | None:
        """The voltage on ``channel``'s pin, or None if nothing drives it."""
        v = self.node.call(self.MODEL, "get_input", adc=self.n, channel=int(channel))
        return None if v is None else float(v)

    def __repr__(self) -> str:
        return f"<Adc {self.node.name}.ADC{self.n}>"


class Dac:
    """A DAC model, e.g. ``node.dac(1)`` (model ``DAC1``). See ``chips/stm32l4/models/dac.hpp``."""

    def __init__(self, node: Node, name: str):
        self.node = node
        self.name = name

    def state(self) -> dict[str, Any]:
        """{"vref", "channels": [{"ch", "enabled", "dhr", "dor", "volts", "pin"}]}"""
        return self.node.call(self.name, "state")

    def channel(self, n: int) -> DacChannel:
        return DacChannel(self, n)

    def __repr__(self) -> str:
        return f"<Dac {self.node.name}.{self.name}>"


class DacChannel:
    """Output channel ``n`` (1-based) of a :class:`Dac`."""

    def __init__(self, dac: Dac, n: int):
        self.dac = dac
        self.n = n

    def state(self) -> dict[str, Any]:
        chans = self.dac.state()["channels"]
        if not 1 <= self.n <= len(chans):
            raise StestError(f"{self.dac.name} has no channel {self.n} (it has {len(chans)})")
        return chans[self.n - 1]

    @property
    def enabled(self) -> bool:
        return bool(self.state()["enabled"])

    @property
    def dor(self) -> int:
        return int(self.state()["dor"])

    @property
    def volts(self) -> float:
        """Output voltage (DOR / 4095 * VREF+). StestError while the channel is disabled."""
        st = self.state()
        if st["volts"] is None:
            raise StestError(f"{self.dac.name} channel {self.n} is disabled (no output)")
        return float(st["volts"])

    def __repr__(self) -> str:
        return f"<DacChannel {self.dac.node.name}.{self.dac.name} ch{self.n}>"
