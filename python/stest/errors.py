"""Exceptions raised by the stest Python harness.

Every exception stest raises on purpose derives from [`StestError`][stest.StestError], except
argument-validation errors, which are plain ``ValueError`` / ``TypeError``.
"""

from __future__ import annotations


class StestError(Exception):
    """Base class of every stest error; also raised directly for recoverable failures.

    Raised directly for a request the node rejected (unknown model or op, bad arguments,
    out-of-range memory, a pin without an analog function) and for harness misuse (calling
    ``sim.start()`` twice, creating nodes after it, a duplicate name, a CAN wiring
    mistake, ...). After a plain ``StestError`` from a node request the node keeps running.
    Two cases end the simulation like [`NodeCrashed`][stest.NodeCrashed] (every node killed,
    the Sim unusable): a ``can=`` controller name that is not a CAN controller of the chip
    (at ``sim.start()``), and a [`CanDevice`][stest.CanDevice] callback that raised (the
    original exception is chained).
    """


class NodeCrashed(StestError):
    """A node process ended the simulation.

    Causes: a host crash (signal), a strict-mode abort (an ``stest: ...`` message naming the
    address, width, direction and peripheral; exit status 70), firmware ``main`` returning,
    an unexpected exit, or a CAN configuration error (a controller transmitting without
    being wired to a bus, or entering normal mode at a bit rate other than its bus's).

    The message names the node, the virtual time and the exit status, and ends with the
    node's last stderr lines. Every node has been killed and reaped before it is raised; the
    aborting quantum's events and output are already in ``sim.event_log``, ``node.output``
    and pin histories. The [`Sim`][stest.Sim] is unusable afterwards.
    """


class WaitTimeout(StestError):
    """A wait did not succeed within its virtual-time budget.

    Raised by [`Sim.wait_for`][stest.Sim.wait_for] and every helper built on it
    (``Pin.wait_high`` / ``wait_low``, ``Node.wait_for_log``, ``SpiBus.wait_for_bytes``,
    ``CanBus.wait_for_frame`` / ``wait_for_message``). The simulation stays usable, with
    ``sim.now_ns`` at the end of the budget.
    """


class WallClockTimeout(StestError):
    """A node did not answer within ``wall_timeout_s`` of real time.

    Typically firmware spinning without any MMIO access, which poll fast-forward cannot
    skip. The message names the node, its last reported virtual time and what the harness
    was waiting for. Every node has been killed and reaped; the [`Sim`][stest.Sim] is
    unusable afterwards.
    """


class SymbolError(StestError):
    """A firmware variable could not be resolved, read or written.

    Raised by [`Node.var`][stest.Node.var] and [`Node.set_var`][stest.Node.set_var]: no
    DWARF debug info, an unknown or ambiguous name, a bad expression, an unsupported type,
    or a value of the wrong type.
    """


class SymbolRangeError(SymbolError, ValueError):
    """A value written with [`Node.set_var`][stest.Node.set_var] does not fit the variable's
    type (also a ``ValueError``). Nothing is written."""
