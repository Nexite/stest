"""GPIO pin handle of one node."""

from __future__ import annotations

import re
from typing import TYPE_CHECKING

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

    def wait_high(self, timeout_ms: float) -> None:
        """Advance the simulation until the pin reads 1 (checked every quantum)."""
        self.node.sim.wait_for(lambda: self.level == 1, timeout_ms, what=f"{self.node.name}.{self.name} high")

    def wait_low(self, timeout_ms: float) -> None:
        self.node.sim.wait_for(lambda: self.level == 0, timeout_ms, what=f"{self.node.name}.{self.name} low")

    def __repr__(self) -> str:
        return f"<Pin {self.node.name}.{self.name}>"
