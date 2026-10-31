"""GPIO pin handle of one node."""

from __future__ import annotations

import re
from typing import TYPE_CHECKING

from .errors import StestError

if TYPE_CHECKING:
    from .node import Node

_PIN_RE = re.compile(r"P([A-Z])(\d{1,2})")


def parse_pin(name: str) -> tuple[str, int]:
    """"PA5" -> ("A", 5)"""
    m = _PIN_RE.fullmatch(name)
    if not m or int(m.group(2)) > 15:
        raise ValueError(f"bad pin name {name!r} (expected e.g. 'PA5')")
    return m.group(1), int(m.group(2))


class Pin:
    """A GPIO pin, e.g. ``node.pin("PA5")``. Model calls go to model ``GPIO<port>``."""

    def __init__(self, node: Node, name: str):
        self.node = node
        self.name = name
        self.port, self.index = parse_pin(name)
        self.model = f"GPIO{self.port}"

    @property
    def level(self) -> int:
        """Current pin level (0/1), from the GPIO model (MODEL_CALL get_pin)."""
        return int(self.node.call(self.model, "get_pin", pin=self.index))

    @property
    def history(self) -> list[tuple[int, int]]:
        """Every level change so far, as (t_ns, level), from the node's gpio events."""
        return list(self.node._pin_history.get((self.port, self.index), ()))

    def set_high(self) -> None:
        """Drive the pin high from outside (overrides the firmware's output)."""
        self.node.call(self.model, "drive_pin", pin=self.index, level=1)

    def set_low(self) -> None:
        self.node.call(self.model, "drive_pin", pin=self.index, level=0)

    def release(self) -> None:
        """Stop driving the pin externally."""
        self.node.call(self.model, "release_pin", pin=self.index)

    def set_voltage(self, volts: float) -> None:
        """Apply an analog voltage to the pin from outside (ADC input). It wins over an
        on-chip driver (DAC output) and does not change the digital level (IDR). Works
        whatever the firmware's RCC clock state: the outside world drives the pin."""
        self.node.call(self.model, "set_voltage", pin=self.index, volts=float(volts))

    def release_voltage(self) -> None:
        """Stop applying an external voltage (a DAC output on the pin shows again)."""
        self.node.call(self.model, "release_voltage", pin=self.index)

    @property
    def voltage(self) -> float:
        """The pin's analog voltage: set via :meth:`set_voltage`, else driven by an on-chip
        DAC output. StestError if neither (the pin floats)."""
        v = self.node.call(self.model, "get_voltage", pin=self.index)
        if v is None:
            raise StestError(f"{self.node.name}.{self.name} has no analog voltage (not set, not driven by a DAC)")
        return float(v)

    def wait_high(self, timeout_ms: float) -> None:
        """Advance the simulation until the pin reads 1 (checked every quantum)."""
        self.node.sim.wait_for(lambda: self.level == 1, timeout_ms, what=f"{self.node.name}.{self.name} high")

    def wait_low(self, timeout_ms: float) -> None:
        self.node.sim.wait_for(lambda: self.level == 0, timeout_ms, what=f"{self.node.name}.{self.name} low")

    def __repr__(self) -> str:
        return f"<Pin {self.node.name}.{self.name}>"
