"""DBC support (via cantools): encode/decode, DBC-aware devices with periodic senders.

Signal values are physical (scaled) values; enum (``VAL_``) signals decode to their choice
name (a ``str``) and encode from either the name or the raw int. Errors from cantools
(unknown message or signal, a value out of its range, a missing signal, a frame whose
length does not fit the message) are raised as ``StestError`` with the original
exception chained. [`Dbc.decode`][stest.Dbc.decode] returns ``None`` for a frame no message
matches (unknown id, extended/standard mismatch, remote frames).

cantools (and python-can, LGPL-3.0, which it imports) is an optional dependency, the ``dbc``
extra (``pip install 'stest[dbc]'``), imported only when a ``Dbc`` is loaded: ``import
stest`` never loads it, and without it ``Dbc(...)`` raises ``StestError``.
"""

from __future__ import annotations

import os
from typing import Callable

from ..errors import StestError
from .device import CanDevice
from .frame import CanFrame
from .log import LoggedFrame


def _cantools():
    """The cantools module, imported on first use (the optional ``dbc`` extra)."""
    try:
        import cantools
        import cantools.database
    except ImportError as e:
        raise StestError(
            "DBC support needs cantools, which is not installed: install the 'dbc' extra "
            "(pip install 'stest[dbc]', or uv add 'stest[dbc]')"
        ) from e
    return cantools


class Dbc:
    """A loaded CAN database (DBC, or any format cantools reads: KCD, SYM).

    Signal values are physical (scaled) values; enum (``VAL_``) signals decode to their
    choice name (a ``str``) and encode from either the name or the raw int.

    Args:
        path: Path to the database file.

    Raises:
        StestError: cantools is not installed (the ``dbc`` extra), the file cannot be
            loaded, or it is not a CAN database.

    Attributes:
        path (str): The file path.
        db (cantools.database.can.Database): The underlying cantools database.
    """

    def __init__(self, path: str | os.PathLike):
        cantools = _cantools()
        try:
            db = cantools.database.load_file(os.fspath(path))
        except Exception as e:
            raise StestError(f"cannot load DBC {os.fspath(path)!r}: {e}") from e
        if not isinstance(db, cantools.database.Database):
            raise StestError(f"{os.fspath(path)!r} is not a CAN database (DBC/KCD/SYM)")
        self.path = os.fspath(path)
        self.db = db

    def message(self, name: str):
        """The cantools message definition named ``name``.

        Args:
            name: Message name as in the DBC.

        Returns:
            message (cantools.database.can.Message): The message definition
                (``frame_id``, ``signals``, ...).

        Raises:
            StestError: No such message; the message lists the known names.
        """
        try:
            return self.db.get_message_by_name(name)
        except KeyError:
            names = ", ".join(m.name for m in self.db.messages)
            raise StestError(f"DBC {self.path!r} has no message {name!r} (messages: {names})") from None

    def encode(self, message: str, signals: dict) -> CanFrame:
        """Encode a message into a frame.

        Args:
            message: Message name.
            signals: Physical value of every signal of the message (for a multiplexed
                message: the selector plus that multiplexer's signals). Enum signals take
                the choice name or the raw int.

        Returns:
            The [`CanFrame`][stest.CanFrame] (identifier and ``ext`` from the DBC).

        Raises:
            StestError: Unknown message, a missing or unknown signal, or a value out of
                its range (the cantools exception is chained).
        """
        msg = self.message(message)
        try:
            data = msg.encode(dict(signals), scaling=True, strict=True)
        except Exception as e:
            raise StestError(f"DBC encode of {message!r} with {dict(signals)!r} failed: {e}") from e
        return CanFrame(msg.frame_id, data, ext=msg.is_extended_frame)

    def decode(self, frame: CanFrame) -> tuple[str, dict] | None:
        """Decode a frame.

        Args:
            frame: The frame to decode.

        Returns:
            ``(message name, {signal: physical value})``, enum signals as their choice
                name; or ``None`` for a remote frame, an identifier no message has, or a
                standard / extended mismatch with the message.

        Raises:
            StestError: The frame matches a message but cannot be decoded (e.g. too short).
        """
        if frame.rtr:
            return None
        try:
            msg = self.db.get_message_by_frame_id(frame.id, force_extended_id=frame.ext)
        except KeyError:
            return None
        if msg.is_extended_frame != frame.ext:
            return None
        try:
            raw = msg.decode(frame.data, decode_choices=True, scaling=True)
        except Exception as e:
            raise StestError(f"DBC decode of {frame} as {msg.name!r} failed: {e}") from e
        return msg.name, {k: (v.name if hasattr(v, "name") and hasattr(v, "value") else v) for k, v in raw.items()}


class DbcDevice(CanDevice):
    """A virtual [`CanDevice`][stest.CanDevice] that speaks a DBC.

    It sends messages by name ([`send_message`][stest.DbcDevice.send_message],
    [`send_periodic`][stest.DbcDevice.send_periodic]) and records the latest decoded value
    of every message it receives in ``received``.

    ```python
    dbc = Dbc("vehicle.dbc")
    ecu = bus.attach(DbcDevice("ecu", dbc))
    ecu.send_periodic("Speed", 10, {"kph": 50})
    ```

    Args:
        name: Device name, unique among the bus's senders.
        dbc: The [`Dbc`][stest.Dbc] used to encode and decode.
        **kw (bool): Passed to [`CanDevice`][stest.CanDevice] (``ack=``).

    Attributes:
        dbc (Dbc): The database.
        received (dict[str, tuple[int, dict]]): Message name -> ``(t_ns, signals)`` of the
            latest frame of that message delivered to the device (EOF time in ns).
    """

    def __init__(self, name: str, dbc: Dbc, **kw):
        super().__init__(name, **kw)
        self.dbc = dbc
        self.received: dict[str, tuple[int, dict]] = {}

    def on_frame(self, lf: LoggedFrame) -> None:
        """Record the decoded frame in ``received`` (frames the DBC does not know are ignored).

        Overriding it in a subclass? Call ``super().on_frame(lf)`` to keep ``received``.

        Args:
            lf: The delivered frame.
        """
        d = self.dbc.decode(lf.frame)
        if d is not None:
            self.received[d[0]] = (lf.t_ns, d[1])

    def send_message(self, message: str, **signals) -> None:
        """Encode a message and request it at the device's current time
        ([`now_ns`][stest.CanDevice.now_ns]).

        Args:
            message: Message name.
            **signals (float | int | str): Physical value of every signal, by name.

        Raises:
            StestError: The message cannot be encoded (see [`Dbc.encode`][stest.Dbc.encode]),
                or the device is not attached.
        """
        self.send(self.dbc.encode(message, signals))

    def send_periodic(
        self, message: str, period_ms: float, signals: dict | Callable[[], dict]
    ) -> None:
        """Send a message periodically on the virtual clock.

        First at now + period, then at exact multiples of the period (see
        [`CanDevice.every`][stest.CanDevice.every]).

        Args:
            message: Message name; an unknown name raises at once.
            period_ms: Period in virtual milliseconds.
            signals: A dict of physical signal values, or a callable returning one,
                evaluated at each send. A bad value raises ``StestError`` at the first
                affected send, ending the simulation like any device callback error.

        Raises:
            StestError: Unknown message, or the device is not attached.
            ValueError: The period rounds to less than 1 ns.
        """
        self.dbc.message(message)  # fail now on an unknown name

        def tick() -> None:
            sig = signals() if callable(signals) else signals
            self.send(self.dbc.encode(message, sig))

        self.every(period_ms, tick)
