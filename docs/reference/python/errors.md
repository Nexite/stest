# Errors

```text
Exception
└── StestError
    ├── NodeCrashed        a node crashed or aborted; the simulation is over
    ├── WallClockTimeout   a node stopped answering in real time; the simulation is over
    ├── WaitTimeout        a wait ran out of virtual time; the simulation goes on
    └── SymbolError        a firmware variable could not be resolved, read or written
        └── SymbolRangeError (also a ValueError)
```

All are importable from `stest`. Argument validation (a bad pin name, a negative
duration, ...) raises plain `ValueError` or `TypeError`.

After [`NodeCrashed`][stest.NodeCrashed] or [`WallClockTimeout`][stest.WallClockTimeout] every
node has been killed and reaped, and every later call on the `Sim` raises the same exception
type again. Logs, `sim.event_log` and pin histories stay readable for diagnosis. Under pytest,
the failure report adds each node's stderr, output and the event log (see the
[pytest plugin reference](../pytest.md)).

::: stest.StestError

::: stest.NodeCrashed

::: stest.WallClockTimeout

::: stest.WaitTimeout

::: stest.SymbolError

::: stest.SymbolRangeError
