"""Typed helpers over peripheral models' MODEL_CALL ops and events (``node.timer(...)``,
``node.spi(...)``, ...)."""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

from .errors import StestError

if TYPE_CHECKING:
    from .node import Node


class Timer:
    """A timer model (TIM1..TIM17), e.g. ``node.timer("TIM7")``.

    Created by [`Node.timer`][stest.Node.timer]. Every property queries the node
    (``call(name, "state")``), so values are current as of the last completed quantum.
    Each query raises [`StestError`][stest.StestError] if the node has no model ``name``.
    The full state schema is in ``chips/stm32l4/models/tim.hpp``.

    Attributes:
        node (Node): The node.
        name (str): The model name, e.g. ``"TIM7"``.
    """

    def __init__(self, node: Node, name: str):
        self.node = node
        self.name = name

    def state(self) -> dict[str, Any]:
        """The timer's raw state.

        Returns:
            ``{"running", "cnt", "arr", "psc", "clk_hz", "update_hz", "counter",
                "channels": [...]}``, plus ``"rcr"`` on timers with a repetition counter;
                ``clk_hz`` is the timer clock and ``counter`` is ``"up"``, ``"down"`` or
                ``"center1"``..``"center3"``; one ``channels`` entry per output-compare channel
                (see [`TimerChannel`][stest.TimerChannel]).
        """
        return self.node.call(self.name, "state")

    @property
    def running(self) -> bool:
        """Whether the counter is counting (``CR1.CEN`` set; it stops on its own in one-pulse
        mode)."""
        return bool(self.state()["running"])

    @property
    def update_hz(self) -> float:
        """Update-event rate in Hz, from the active PSC/ARR/RCR and the timer clock.

        0.0 when the timer is stopped, blocked (ARR = 0) or has ``UDIS`` set.
        """
        return float(self.state()["update_hz"])

    def channel(self, n: int) -> TimerChannel:
        """A handle to one output-compare channel.

        Args:
            n: Channel number, 1-based. Checked when the channel is queried.

        Returns:
            A [`TimerChannel`][stest.TimerChannel].
        """
        return TimerChannel(self, n)

    def __repr__(self) -> str:
        return f"<Timer {self.node.name}.{self.name}>"


class TimerChannel:
    """Output-compare channel ``n`` (1-based) of a [`Timer`][stest.Timer].

    Every property queries the node. Duties are the configured duty the output would have,
    reported even while the output is disabled, not a measured pin level.

    Attributes:
        timer (Timer): The timer.
        n (int): The channel number, 1-based.
    """

    def __init__(self, timer: Timer, n: int):
        self.timer = timer
        self.n = n

    def state(self) -> dict[str, Any]:
        """The channel's raw state.

        Returns:
            ``{"ch", "enabled", "mode", "polarity", "ccr", "ref_duty", "duty"}``, plus
                ``{"enabled_n", "polarity_n", "duty_n", "dead_time_ns"}`` on channels with a
                complementary output.

        Raises:
            StestError: The timer has fewer than ``n`` channels.
        """
        chans = self.timer.state()["channels"]
        if not 1 <= self.n <= len(chans):
            raise StestError(f"{self.timer.name} has no channel {self.n} (it has {len(chans)})")
        return chans[self.n - 1]

    @property
    def enabled(self) -> bool:
        """Whether output OCx is enabled (``CCxE``, and ``BDTR.MOE`` on timers with a BDTR)."""
        return bool(self.state()["enabled"])

    @property
    def mode(self) -> str:
        """The output-compare mode: ``"frozen"``, ``"active_on_match"``,
        ``"inactive_on_match"``, ``"toggle"``, ``"force_inactive"``, ``"force_active"``,
        ``"pwm1"`` or ``"pwm2"``."""
        return self.state()["mode"]

    @property
    def duty(self) -> float | None:
        """Fraction (0.0..1.0) of the period output OCx is active: the PWM duty.

        Equal to [`ref_duty`][stest.TimerChannel.ref_duty], minus the dead time once per
        period when OCx and OCxN are both enabled. Polarity (``CCxP``) is not applied: it
        changes which level is active, not the fraction. ``None`` for modes with a static
        level (frozen, active / inactive on match).
        """
        return self.state()["duty"]

    @property
    def ref_duty(self) -> float | None:
        """Fraction (0.0..1.0) of the period the internal reference OCxREF is active (no
        dead time).

        PWM1 counting up: ``CCR / (ARR + 1)``; down: ``(CCR + 1) / (ARR + 1)``; center-aligned:
        ``CCR / ARR``; PWM2 is ``1 -`` PWM1; toggle 0.5; forced 0 or 1. ``None`` for frozen and
        active / inactive on match.
        """
        return self.state()["ref_duty"]

    @property
    def duty_n(self) -> float | None:
        """Fraction (0.0..1.0) of the period the complementary output OCxN is active.

        ``1 - ref_duty`` minus the dead time when OCx and OCxN are both enabled; ``ref_duty``
        when only OCxN is enabled. ``None`` for modes with a static level.

        Raises:
            StestError: The channel has no complementary output (CHxN).
        """
        return self._comp("duty_n")

    @property
    def dead_time_ns(self) -> float:
        """The dead time inserted between OCx and OCxN, in ns (from ``BDTR.DTG``, ``CR1.CKD``
        and the timer clock).

        Raises:
            StestError: The channel has no complementary output.
        """
        return float(self._comp("dead_time_ns"))

    def _comp(self, key: str) -> Any:
        st = self.state()
        if key not in st:
            raise StestError(f"{self.timer.name} channel {self.n} has no complementary output")
        return st[key]

    def __repr__(self) -> str:
        return f"<TimerChannel {self.timer.node.name}.{self.timer.name} ch{self.n}>"


class Adc:
    """ADC ``n`` (1-based) of the chip's ADC model, e.g. ``node.adc(1)``.

    Created by [`Node.adc`][stest.Node.adc]. Inputs are pin voltages:
    ``set(channel, volts)`` sets the voltage of the pin wired to that channel (the part
    TOML's ``[[analog]]`` table), so every ADC sharing the pin sees it, exactly like
    ``node.pin("PB1").set_voltage(volts)``. It works whatever the firmware's RCC clock state
    (the outside world drives the pin). Values are clamped to 0..VREF+ when converted.

    Attributes:
        node (Node): The node.
        n (int): The ADC number, 1-based.
    """

    MODEL = "ADC"

    def __init__(self, node: Node, n: int):
        self.node = node
        self.n = int(n)

    def set(self, channel: int, volts: float) -> None:
        """Apply a voltage to the pin wired to ``channel``.

        Args:
            channel: ADC channel number (as in ``SQR1``), e.g. ``16``.
            volts: The voltage in volts.

        Raises:
            StestError: The channel has no pin on this package.
        """
        self.node.call(self.MODEL, "set_input", adc=self.n, channel=int(channel), volts=float(volts))

    def get(self, channel: int) -> float | None:
        """The voltage on the pin wired to ``channel``.

        Args:
            channel: ADC channel number.

        Returns:
            The voltage in volts (an external voltage, else a DAC output on the pin), or
                ``None`` if nothing drives the pin.
        """
        v = self.node.call(self.MODEL, "get_input", adc=self.n, channel=int(channel))
        return None if v is None else float(v)

    def __repr__(self) -> str:
        return f"<Adc {self.node.name}.ADC{self.n}>"


class Dac:
    """A DAC model, e.g. ``node.dac(1)`` (model ``DAC1``).

    Created by [`Node.dac`][stest.Node.dac]. Every query goes to the node and raises
    [`StestError`][stest.StestError] if it has no such model. See
    ``chips/stm32l4/models/dac.hpp``.

    Attributes:
        node (Node): The node.
        name (str): The model name, e.g. ``"DAC1"``.
    """

    def __init__(self, node: Node, name: str):
        self.node = node
        self.name = name

    def state(self) -> dict[str, Any]:
        """The DAC's raw state.

        Returns:
            ``{"vref", "channels": [{"ch", "enabled", "dhr", "dor", "volts", "pin"}]}``;
                ``volts`` is ``None`` while the channel is disabled, ``pin`` is ``None`` when the
                output is internal only.
        """
        return self.node.call(self.name, "state")

    def channel(self, n: int) -> DacChannel:
        """A handle to one output channel.

        Args:
            n: Channel number, 1-based. Checked when the channel is queried.

        Returns:
            A [`DacChannel`][stest.DacChannel].
        """
        return DacChannel(self, n)

    def __repr__(self) -> str:
        return f"<Dac {self.node.name}.{self.name}>"


class DacChannel:
    """Output channel ``n`` (1-based) of a [`Dac`][stest.Dac].

    Attributes:
        dac (Dac): The DAC.
        n (int): The channel number, 1-based.
    """

    def __init__(self, dac: Dac, n: int):
        self.dac = dac
        self.n = n

    def state(self) -> dict[str, Any]:
        """The channel's raw state.

        Returns:
            ``{"ch", "enabled", "dhr", "dor", "volts", "pin"}``.

        Raises:
            StestError: The DAC has fewer than ``n`` channels.
        """
        chans = self.dac.state()["channels"]
        if not 1 <= self.n <= len(chans):
            raise StestError(f"{self.dac.name} has no channel {self.n} (it has {len(chans)})")
        return chans[self.n - 1]

    @property
    def enabled(self) -> bool:
        """Whether the channel is enabled (``CR.ENx``)."""
        return bool(self.state()["enabled"])

    @property
    def dor(self) -> int:
        """The data output register value, 0..4095.

        ``DOR`` takes the holding register's value one APB1 clock after the write (or after
        the software trigger when ``TENx`` = 1).
        """
        return int(self.state()["dor"])

    @property
    def volts(self) -> float:
        """The output voltage in volts, ``DOR / 4095 * VREF+``.

        Raises:
            StestError: The channel is disabled (no output).
        """
        st = self.state()
        if st["volts"] is None:
            raise StestError(f"{self.dac.name} channel {self.n} is disabled (no output)")
        return float(st["volts"])

    def __repr__(self) -> str:
        return f"<DacChannel {self.dac.node.name}.{self.dac.name} ch{self.n}>"


@dataclass(frozen=True)
class SpiBurst:
    """One SPI burst: frames sent back to back while BSY stayed high.

    ``mosi`` / ``miso`` hold one byte per frame for data sizes up to 8 bits, two bytes
    little-endian per frame above.

    Attributes:
        t_start_ns: Virtual time in ns at which the first frame started.
        mosi: The frame values the master wrote to ``DR`` (sent on MOSI).
        miso: The frame values received on MISO.
        t_end_ns: Virtual time in ns at which BSY fell (the ``spi`` event's time).
    """

    t_start_ns: int
    mosi: bytes
    miso: bytes
    t_end_ns: int


class SpiBus:
    """An SPI model, e.g. ``node.spi("SPI1")``. See ``chips/stm32l4/models/spi.hpp``.

    Created by [`Node.spi`][stest.Node.spi]. The model is the master's peer: it records what
    the firmware sends and answers with queued MISO bytes.

    ``bursts`` lists what the master clocked out, one [`SpiBurst`][stest.SpiBurst] per BSY period: a
    burst ends when BSY falls (the TX FIFO ran dry), so a burst is not a transaction. ST's
    ``HAL_SPI_Transmit`` keeps the FIFO fed (one burst per call), but
    ``HAL_SPI_TransmitReceive`` alternates TX and RX, which drops BSY between frame pairs.
    To group bursts into transactions, use the chip select's ``node.pin(...).history``:
    every burst with ``cs_fall <= t_start_ns`` and ``t_end_ns <= cs_rise`` belongs to that
    chip-select window.

    Attributes:
        node (Node): The node.
        name (str): The model name, e.g. ``"SPI1"``.
    """

    def __init__(self, node: Node, name: str):
        self.node = node
        self.name = name

    @property
    def bursts(self) -> list[SpiBurst]:
        """Every completed [`SpiBurst`][stest.SpiBurst] so far, oldest first (as of the last
        completed quantum). Returns a copy."""
        return list(self.node._spi_bursts.get(self.name, ()))

    def queue_miso(self, data: bytes) -> None:
        """Append bytes to what the slave returns on MISO.

        The queue is consumed one byte per frame (two, little-endian, for frames wider than
        8 bits). When it is empty a frame reads all ones (0xFF).

        Args:
            data: ``bytes``, ``bytearray`` or ``memoryview``.

        Raises:
            TypeError: ``data`` is not bytes-like.
        """
        if not isinstance(data, (bytes, bytearray, memoryview)):
            raise TypeError(f"queue_miso expects bytes, not {type(data).__name__}")
        self.node.call(self.name, "queue_miso", data=bytes(data).hex())

    def wait_for_bytes(self, n: int, timeout_ms: float) -> None:
        """Advance the simulation until the completed bursts hold at least ``n`` MOSI bytes.

        Counts every burst since the start (not only new ones), checked now and at every
        quantum boundary.

        Args:
            n: Total number of MOSI bytes.
            timeout_ms: Budget in virtual milliseconds.

        Raises:
            WaitTimeout: Fewer than ``n`` bytes were sent within ``timeout_ms``.
        """
        self.node.sim.wait_for(
            lambda: sum(len(b.mosi) for b in self.node._spi_bursts.get(self.name, ())) >= n,
            timeout_ms,
            what=f"{self.node.name}.{self.name} to send {n} bytes",
        )

    def state(self) -> dict[str, Any]:
        """The SPI model's raw state.

        Returns:
            ``{"enabled", "master", "data_bits", "cpol", "cpha", "lsbfirst", "baud_hz",
                "frame_ns", "busy", "tx_level", "rx_level", "ovr", "miso_queued"}``
                (``tx_level`` / ``rx_level`` in bytes).
        """
        return self.node.call(self.name, "state")

    def __repr__(self) -> str:
        return f"<SpiBus {self.node.name}.{self.name}>"
