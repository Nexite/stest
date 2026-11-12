# Pins

A [`Pin`][stest.Pin] is a handle to one GPIO pin of a node: `node.pin("PA5")`. Pins are named
as on the chip. Recipes use the [imports and `firmware` fixture](index.md#how-the-recipes-run)
of the cookbook.

## Press a button, check the LED

**Goal:** drive an input, let the firmware react (here through an EXTI interrupt), and read an
output.

```python
--8<-- "python/tests/test_recipes.py:button"
```

- `set_high()` / `set_low()` drive the pin from outside and win over the firmware's own output
  until `release()`. They take effect at the current virtual time, between quanta.
- An edge on a pin whose EXTI line the firmware armed sets the EXTI pending bit and raises the
  interrupt, exactly as ST's `HAL_GPIO_Init(... GPIO_MODE_IT_RISING ...)` configured it. Your
  `EXTIx_IRQHandler` and `HAL_GPIO_EXTI_Callback` run as on the chip.
- `level` is what the firmware reads from `IDR`: the external drive if any, else the output
  (`ODR`) for an output pin, else the pull (1 with a pull-up, 0 otherwise).
- Let `main()` configure the pins first (`sim.advance(ms=2)` here). An edge before the EXTI
  line is armed is not an interrupt, as on hardware.

## Check output timing

**Goal:** assert *when* an output changed, not only its current level.

```python
--8<-- "python/tests/test_recipes.py:history"
```

- `history` lists every change as `(t_ns, level)` at the exact virtual time it happened,
  whether the firmware or the test caused it.
- `pin.wait_high(timeout_ms=...)` / `wait_low(...)` advance until the level is reached. Like
  every wait they stop at a quantum boundary; read the exact time from `history`.

## Exact edge times

**Goal:** apply an input at a precise virtual time, independent of the quantum.

```python
--8<-- "python/tests/test_recipes.py:drive-at"
```

- `drive_at(level, t_ns)` queues the change; it takes effect at exactly `t_ns` during the next
  quantum that reaches it. `None` releases the pin. Calls apply in order.
- `t_ns` must not be in the node's past: that aborts the node (an
  [inbox message in the past](../../reference/firmware.md#inbox-messages)).
- Inputs made with `set_high()` / `set_low()` land on a quantum boundary instead, so their time
  depends on the quantum. Use `drive_at` when timing matters or when you compare runs with
  different quanta.

## Analog voltages

Pins with an analog function (ADC inputs and DAC outputs) also carry a voltage:
`pin.set_voltage(volts)` and `pin.voltage`. See [Peripherals](peripherals.md#feed-an-adc-input).

## Notes

- `node.pin("PB11")` creates a handle without checking the pin. Requests to a port the chip lacks
  raise [`StestError`][stest.StestError].
- Every pin change is also an event in `sim.event_log`:
  `{"kind":"gpio","periph":"GPIOA","port":"A","pin":5,"level":1}`
  ([Firmware output](output.md#read-the-event-log)).
- In [HAL mode](../../concepts.md#register-mode-and-hal-mode) (`MODELS GPIO=hal`), pins,
  EXTI, event times and interrupts behave the same.
