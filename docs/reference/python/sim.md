# Sim, Node and Pin

A [`Sim`][stest.Sim] owns the global virtual clock and runs one or more firmware
[`Node`][stest.Node]s in lockstep. A [`Pin`][stest.Pin] is a handle to one GPIO pin of a node.

Time conventions used on this page: arguments named `*_ms` are virtual milliseconds
(floats allowed), `*_us` virtual microseconds and `t_ns` / `*_ns` virtual nanoseconds
(integers). Nothing here depends on wall-clock time except `wall_timeout_s`.

::: stest.Sim

::: stest.Node

::: stest.Pin
