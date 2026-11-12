# stest.can

Simulated classic CAN: buses, Python virtual devices, frames, DBC support and log formats.
The classes are also exported from `stest` (`from stest import CanBus, CanFrame`); the
helper functions and constants live in `stest.can`. The [CAN guide](../../can.md) explains the
bus engine, error handling and the bxCAN model.

DBC support needs the optional `dbc` extra (`pip install 'stest[dbc]'`), which installs
cantools; `import stest` never loads it.

::: stest.CanBus

::: stest.CanDevice

::: stest.CanFrame

::: stest.LoggedFrame

::: stest.Dbc

::: stest.DbcDevice

## Functions and constants

| Constant | Value | Meaning |
|---|---|---|
| `stest.can.MIN_FRAME_BITS` | 44 | Conservative floor of any frame's length from SOF to EOF in bits. A 0-byte standard data frame would be 44 bits without stuff bits, but every real frame has at least one, so the true minimum is 45. [`lookahead_ns`][stest.can.lookahead_ns] is built on it. |
| `stest.can.IFS_BITS` | 3 | The intermission between frames in bits; not part of [`CanFrame.bit_length`][stest.CanFrame.bit_length]. |

::: stest.can.lookahead_ns
    options:
      heading_level: 3

::: stest.can.crc15
    options:
      heading_level: 3

::: stest.can.format_candump
    options:
      heading_level: 3

::: stest.can.format_asc
    options:
      heading_level: 3
