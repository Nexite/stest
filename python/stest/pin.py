"""GPIO pin handle of one node ([`Pin`][stest.Pin])."""

from __future__ import annotations

import re
from typing import TYPE_CHECKING

from .errors import StestError

if TYPE_CHECKING:
    from .node import Node

_PIN_RE = re.compile(r"P([A-Z])(\d{1,2})")


def parse_pin(name: str) -> tuple[str, int]:
    """Split a pin name: ``"PA5"`` -> ``("A", 5)``. ValueError on a bad name."""
    m = _PIN_RE.fullmatch(name)
    if not m or int(m.group(2)) > 15:
        raise ValueError(f"bad pin name {name!r} (expected e.g. 'PA5')")
    return m.group(1), int(m.group(2))


class Pin:
    """A GPIO pin of one node, e.g. ``node.pin("PA5")``.

    Created by [`Node.pin`][stest.Node.pin]; creating a handle has no effect on the node.
    Every query and drive is a request to the pin's GPIO model (``GPIO<port>``, e.g.
    ``GPIOA``) made between quanta, while the node is paused, so values are as of the last
    completed quantum.

    Attributes:
        node (Node): The node the pin belongs to.
        name (str): The pin name, e.g. ``"PA5"``.
        port (str): The port letter, e.g. ``"A"``.
        index (int): The pin number within the port, 0..15.
        model (str): The GPIO model the requests go to, e.g. ``"GPIOA"``.
    """

    def __init__(self, node: Node, name: str):
        self.node = node
        self.name = name
        self.port, self.index = parse_pin(name)
        self.model = f"GPIO{self.port}"

    @property
    def level(self) -> int:
        """The pin's current digital level, 0 or 1 (what the firmware reads from ``IDR``).

        An external drive ([`set_high`][stest.Pin.set_high], [`drive_at`][stest.Pin.drive_at])
        wins over the firmware's output; otherwise an output pin follows ``ODR`` (open drain
        with ``ODR`` = 1 releases the line), and any other pin reads its pull: 1 with a
        pull-up, 0 with a pull-down or none.

        Raises:
            StestError: The node has no model ``GPIO<port>`` (e.g. a port the chip lacks).
        """
        return int(self.node.call(self.model, "get_pin", pin=self.index))

    @property
    def history(self) -> list[tuple[int, int]]:
        """Every level change so far as ``(t_ns, level)`` tuples, oldest first.

        ``t_ns`` is the virtual time of the change in ns. Built from the node's ``gpio``
        events, so it covers changes caused by the firmware and by external drives alike,
        up to the last completed quantum. Returns a copy.
        """
        return list(self.node._pin_history.get((self.port, self.index), ()))

    def set_high(self) -> None:
        """Drive the pin high from outside, now.

        The external drive overrides the firmware's output until [`release`][stest.Pin.release].
        It takes effect at the node's current virtual time (the end of the last quantum).
        Use [`drive_at`][stest.Pin.drive_at] for an exact later instant.
        """
        self.node.call(self.model, "drive_pin", pin=self.index, level=1)

    def set_low(self) -> None:
        """Drive the pin low from outside, now.

        The external drive overrides the firmware's output until [`release`][stest.Pin.release].
        It takes effect at the node's current virtual time (the end of the last quantum).
        """
        self.node.call(self.model, "drive_pin", pin=self.index, level=0)

    def release(self) -> None:
        """Stop driving the pin from outside, now; the firmware's output or the pull shows again."""
        self.node.call(self.model, "release_pin", pin=self.index)

    def drive_at(self, level: int | None, t_ns: int) -> None:
        """Drive the pin at exactly virtual time ``t_ns``, or release it.

        Queued through [`Node.post`][stest.Node.post]: the drive takes effect during the next
        quantum that reaches ``t_ns`` and its ``gpio`` event is stamped ``t_ns`` whatever the
        quantum size. Calls queue up and apply in order.

        Args:
            level: 0 or 1 to drive the pin, ``None`` to release it.
            t_ns: Virtual time in ns. Must not be before the node's current time.

        Raises:
            ValueError: ``level`` is not 0, 1 or ``None`` (a ``bool`` is rejected too).
            TypeError: ``t_ns`` is not an ``int``.
            NodeCrashed: Raised by the next advance if ``t_ns`` was already in the node's past.
        """
        if level not in (0, 1, None) or isinstance(level, bool):
            raise ValueError(f"drive_at: level must be 0, 1 or None (got {level!r})")
        self.node.post(self.model, "drive_pin", {"pin": self.index, "level": level}, t_ns)

    def set_voltage(self, volts: float) -> None:
        """Apply an analog voltage to the pin from outside, e.g. an ADC input.

        The external voltage wins over an on-chip driver (a DAC output) and does not change
        the digital level (``IDR``). It works whatever the firmware's RCC clock state, since the
        outside world drives the pin. The ADC clamps the value to 0..VREF+ when it converts.

        Args:
            volts: The voltage in volts.

        Raises:
            StestError: The pin has no analog function: only ADC inputs from the part's
                ``[[analog]]`` table and DAC outputs accept a voltage. The message names the
                valid pins (``PB11 has no analog function on STM32L496RG; analog pins: ...``).
        """
        try:
            self.node.call(self.model, "set_voltage", pin=self.index, volts=float(volts))
        except StestError as e:
            msg = str(e)
            marker = f"MODEL_CALL {self.model}.set_voltage: "
            if marker in msg and "has no analog function" in msg:
                raise StestError(f"node {self.node.name!r}: {msg.split(marker, 1)[1]}") from None
            raise

    def release_voltage(self) -> None:
        """Stop applying an external voltage; a DAC output on the pin shows again."""
        self.node.call(self.model, "release_voltage", pin=self.index)

    @property
    def voltage(self) -> float:
        """The pin's analog voltage in volts.

        The external voltage from [`set_voltage`][stest.Pin.set_voltage] if one is set, else the
        output of an on-chip DAC driving the pin.

        Raises:
            StestError: Neither is present (the pin floats).
        """
        v = self.node.call(self.model, "get_voltage", pin=self.index)
        if v is None:
            raise StestError(f"{self.node.name}.{self.name} has no analog voltage (not set, not driven by a DAC)")
        return float(v)

    def wait_high(self, timeout_ms: float) -> None:
        """Advance the simulation until the pin reads 1.

        The level is checked now and at every quantum boundary (see
        [`Sim.wait_for`][stest.Sim.wait_for]), so the simulation stops at the end of the
        quantum in which the pin went high. Use [`history`][stest.Pin.history] for the exact time.

        Args:
            timeout_ms: Budget in virtual milliseconds.

        Raises:
            WaitTimeout: The pin was not high within ``timeout_ms``.
        """
        self.node.sim.wait_for(lambda: self.level == 1, timeout_ms, what=f"{self.node.name}.{self.name} high")

    def wait_low(self, timeout_ms: float) -> None:
        """Advance the simulation until the pin reads 0.

        Same rules as [`wait_high`][stest.Pin.wait_high].

        Args:
            timeout_ms: Budget in virtual milliseconds.

        Raises:
            WaitTimeout: The pin was not low within ``timeout_ms``.
        """
        self.node.sim.wait_for(lambda: self.level == 0, timeout_ms, what=f"{self.node.name}.{self.name} low")

    def __repr__(self) -> str:
        return f"<Pin {self.node.name}.{self.name}>"
