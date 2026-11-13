# sensor-node: an example stest project

Two STM32L496RG boards, written as CubeMX projects and tested together in stest:

- **sensor** (`firmware/sensor`): the button on PC13 toggles the LED on PA5. Every 100 ms the
  board reads PC0 (ADC1 channel 1) and sends `SensorReading` (id `0x120`) on CAN1 at 500 kbit/s.
- **display** (`firmware/display`): receives `SensorReading` and turns its LED on PA5 on while
  the reading is above 2000 mV.

The CAN messages are described in `can/sensor-node.dbc`. The tests are in `tests/`.

This directory is laid out like your own test repo would be, with stest as a submodule at
`STEST_DIR`. Here `STEST_DIR` defaults to the checkout two levels up (`../..`).

## Build and test

You need the stest prerequisites (upstream LLVM >= 18, CMake >= 3.23, Ninja, Python >= 3.11, uv).

```sh
cd examples/sensor-node
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
uv run pytest tests -q
```

The tests look for the executables in `build/`; set `SENSOR_NODE_BIN` to use another directory.
