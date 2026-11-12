# Peripherals

Each modelled peripheral is driven by your firmware through its registers, as on the chip.
From the test you observe and feed it through typed helpers: [`node.timer(...)`][stest.Node.timer],
[`node.adc(n)`][stest.Node.adc], [`node.dac(n)`][stest.Node.dac] and
[`node.spi(...)`][stest.Node.spi]. What each model covers is in the
[support matrix](../../reference/support.md). Recipes use the
[imports and `firmware` fixture](index.md#how-the-recipes-run) of the cookbook.

## Check a timer's rate and a PWM duty

**Goal:** verify a timer's configuration, and the duty of an output it drives.

```python
--8<-- "python/tests/test_recipes.py:timer"
```

- [`Timer.state()`][stest.Timer.state] reports what the model computed from the registers and
  the clock tree: `running`, `cnt`, `psc`, `arr`, `clk_hz`, `update_hz`, the counting mode and,
  per channel, mode, polarity and duty.
- For hardware PWM (a timer channel in PWM mode), read the configured duty directly:
  `node.timer("TIM2").channel(1).duty`. It is the duty the output *would* have, also while the
  channel's output is disabled (check `.enabled`).
- A pin toggled from a timer interrupt (as here) is a GPIO output: measure it from
  `Pin.history`.
- Timers are exact in virtual time: a 20 kHz update runs 20 000 times per virtual second.

## Feed an ADC input

**Goal:** apply a voltage to an analog input and check what the firmware converts.

```python
--8<-- "python/tests/test_recipes.py:adc"
```

- [`pin.set_voltage(volts)`][stest.Pin.set_voltage] applies a voltage to a pin with an analog
  function. The ADC samples it at the end of each conversion's sampling phase. The code is
  `round(clamp(v, 0, VREF+) / VREF+ * (2^bits - 1))`, with VREF+ = 3.3 V.
- [`node.adc(1).set(channel, volts)`][stest.Adc.set] does the same by ADC channel, through the
  part's analog pin table (channel 16 of ADC1 and ADC2 is PB1 on the STM32L496RG).
- Only analog-capable pins accept a voltage (ADC inputs and DAC outputs). Any other pin raises
  [`StestError`][stest.StestError] naming the valid ones.
- An input you never set reads 0 V, with one warning per channel on the node's stderr. Make
  that an abort with [strict analog](faults.md#require-every-analog-input).
- The voltage can be set before the firmware enables the ADC or GPIO clocks: the outside world
  drives the pin.

## Read a DAC output

**Goal:** check the voltage the firmware outputs.

```python
--8<-- "python/tests/test_recipes.py:dac"
```

- [`DacChannel.volts`][stest.DacChannel.volts] is `DOR / 4095 * VREF+` while the channel is
  enabled. A disabled channel raises `StestError`.
- The output pin (PA4 for channel 1, PA5 for channel 2) carries the voltage, so an ADC channel
  on the same pin converts it. A voltage set from the test on that pin wins over the DAC until
  `release_voltage()`.
- Each output change is an event: `{"kind":"dac","periph":"DAC1","channel":1,"volts":1.65}`.

## Capture SPI traffic and answer it

**Goal:** check what the firmware sends on SPI, and give it data to receive.

```python
--8<-- "python/tests/test_recipes.py:spi"
```

- [`SpiBus.bursts`][stest.SpiBus.bursts] lists the traffic as bursts: frames sent back to back,
  each with `mosi`, `miso`, `t_start_ns` and `t_end_ns`. A burst ends when the SPI's busy flag
  drops, so `HAL_SPI_TransmitReceive` (which alternates sending and receiving) produces several
  bursts per transaction. Group them by the chip-select pin's history, as here.
- [`queue_miso(data)`][stest.SpiBus.queue_miso] queues the bytes the device answers with. When
  the queue is empty, the firmware reads `0xFF`.
- [`wait_for_bytes(n, timeout_ms)`][stest.SpiBus.wait_for_bytes] advances until `n` bytes have
  been sent.
- Only master mode with software NSS is modelled.

## Below the helpers

The helpers wrap each model's ops. [`node.call(model, op, ...)`][stest.Node.call] calls an op
directly, and [`node.post(...)`][stest.Node.post] applies an input at an exact time; the ops are
documented in the model's header, linked from the [support matrix](../../reference/support.md).
CAN has its own page: [CAN](../../can.md).
