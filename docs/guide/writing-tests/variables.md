# Firmware variables

Tests read and write firmware variables by name, using the debug information stest builds every
firmware with (`-g -O0`). Recipes use the [imports and `firmware` fixture](index.md#how-the-recipes-run)
of the cookbook. They use these globals from the blinky firmware's `vars.c`:

```c
--8<-- "selftest/firmware/blinky/Core/Src/vars.c:7:23"
```

## Read variables

**Goal:** check scalars, enums, structs and arrays.

```python
--8<-- "python/tests/test_recipes.py:read"
```

- [`node.var(expr)`][stest.Node.var] works for variables with a static address: globals,
  file-scope `static` variables and function-scope `static` variables. Locals on the stack are
  not reachable.
- Expressions:
    - `"name"` is a global;
    - `"file.c:name"` is a file-scope `static`, when several files define one with the same
      name;
    - `"func::name"` is a `static` inside `func`;
    - members and indexes chain: `"g_cfg.lamps[2].duty"`.
- Results:
    - scalars are `int`, `float` or `bool`;
    - an enum is an `int` that also compares equal to its enumerator's name;
    - a struct is a read-only mapping;
    - an array is a `list`.
- The value is the one at `sim.now_ns`: the simulation is paused while Python reads.

## Write a variable

**Goal:** change firmware state from the test, for example a configuration value or a fault
flag.

```python
--8<-- "python/tests/test_recipes.py:write"
```

- [`node.set_var(expr, value)`][stest.Node.set_var] writes one scalar: an integer, float, bool or
  enumerator name. Assign struct members one at a time.
- The value is checked against the variable's type before anything is written: a value that
  does not fit raises [`SymbolRangeError`][stest.SymbolRangeError].
- The firmware sees the new value the next time it reads the variable, after the simulation
  resumes.

## Notes

- Variables are available after `sim.start()`.
- A name the debug information does not have raises [`SymbolError`][stest.SymbolError]; an
  ambiguous one (two file-scope `static`s with the same name) lists the candidates.
- A loop that waits for a variable written from Python (`while (!flag) __WFI();`) sees the new
  value at its next wake-up, an interrupt, not at the next quantum. See
  [WFI and WFE](../../concepts.md#wfi-and-wfe).
- An unused global may be removed by the linker. stest's test firmware marks such variables
  `__attribute__((used))`.
- [`node.read_mem(addr, n)`][stest.Node.read_mem] and
  [`node.write_mem(addr, data)`][stest.Node.write_mem] access raw memory by address.
