# Adding a chip

stest organises chips as **family → part** (design spec section 4.2):

- A **family** (`chips/stm32l4/`) owns the peripheral models, the ST sources (HAL, CMSIS device) and the family's unit tests.
- A **part** (`chips/stm32l4/parts/stm32l496rg.toml`) is data. It lists which model instances exist, where they are mapped, their parameters, clock gates and IRQ numbers.

Adding a part to an existing family should be mostly a new TOML file. Adding a family is more work, and for a new Cortex-M core (STM32U3 / Cortex-M33 is the next planned family, Plan 4) parts of the runtime are still open work (section 3.5).

To write the models themselves, see `docs/adding-a-peripheral.md`.

## 1. How a part becomes an executable

`stest_add_firmware(<name> CHIP STM32L496RG ...)` (`cmake/stest_add_firmware.cmake`) runs these steps:

1. It lower-cases the part and globs `chips/*/parts/stm32l496rg.toml`. Exactly one match is required.
2. It runs `tools/gen_chip.py --info <toml>`, which prints `name;family;device_define` (e.g. `STM32L496RG;stm32l4;STM32L496xx`).
3. It calls `stest_st_family_dirs(<family> ...)` and `stest_cmsis_core_include(...)` (`cmake/StestFetchST.cmake`) to get the HAL, the CMSIS device files and the staged CMSIS-Core headers.
4. It compiles the firmware plus `<family>xx_hal*.c` and `Source/Templates/system_<family>xx.c` with the MMIO pass, `-D<device_define> -DUSE_HAL_DRIVER`, and `chips/cmsis_host` first on the include path.
5. It generates `<name>_chip.cpp` from the TOML (`tools/gen_chip.py <toml> <out.cpp>`). The output contains:
   - weak `*_IRQHandler` defaults that abort with `unhandled IRQ <NAME> (<n>)`
   - the IRQ table
   - `stest_chip_init()`, which constructs, wires, maps and gates the models
   - `stest_chip`
6. It links all of this against `stest_chip_<family>`. The generator re-runs when the TOML or `gen_chip.py` changes.

## 2. Adding a part to an existing family

### 2.1 Write `chips/<family>/parts/<part>.toml`

The file name is the lower-case part name. Use `chips/stm32l4/parts/stm32l496rg.toml` as the template. Every number in it comes from **ST's CMSIS device header for the part** (e.g. `stm32l496xx.h` in `cmsis_device_l4`, fetched into `<build>/_deps/` by `StestFetchST.cmake`) or from the reference manual. Never write them from memory.

**`[chip]`**, validated by `load()` in `tools/gen_chip.py`:

| Key | Required | Source / meaning |
|---|---|---|
| `name` | yes | Part name, e.g. `"STM32L496RG"` (`stest_chip.name`). |
| `family` | yes | Directory under `chips/`, e.g. `"stm32l4"`. It selects `stest_chip_<family>` and the ST sources. |
| `core` | yes | Only `"cortex-m4"` is accepted today (section 3.5). |
| `nvic_prio_bits` | yes | `__NVIC_PRIO_BITS` in the device header. |
| `device_define` | yes | The part's define as `stm32l4xx.h` expects it, e.g. `"STM32L496xx"`. |
| `system_init` | no | Function run on the firmware thread before `main` (`"SystemInit"`). |
| `systick_external_divider` | no | SysTick CLKSOURCE=0 divider (8 on L4: HCLK/8, see the RM's RCC/SysTick section). |

**`[clock_tree]`**:
- `peripheral` names the `[[peripheral]]` whose model implements `stest::ClockTree` (the RCC).
- `enable_regs` maps the RCC enable registers that `rcc_enable` may name to their word offsets in the RCC block (e.g. `AHB2ENR = 0x4C`, from `RCC_TypeDef`).

**`[[peripheral]]`**, one per instance, in init order:

| Key | Meaning |
|---|---|
| `name` | Instance name, matching the CMSIS instance macro (`GPIOA`, `TIM7`). Must be unique. It is the name Python uses in `node.call()` and `node.timer()`. |
| `model` | `"<family>.<kind>"` → class `stest::<family>::<Kind>` in `chips/<family>/models/<kind>.hpp`. |
| `base`, `size` | `<NAME>_BASE` from the device header, and the block size (usually `0x400`). Must lie in the MMIO windows `[0x40000000, 0x60000000)` or `[0xE0000000, 0xE0100000)` (`runtime/include/stest/mmio_windows.hpp`), and must not overlap another mapping. `Bus::map` throws otherwise. |
| `params` | Model-specific inline table, passed to the constructor as JSON. Each model's header documents its schema (e.g. `tim.hpp`: `kind`, `width`, `bus`, `irq`, `opt`; `gpio.hpp`: `port`, `*_reset`). |
| `rcc_enable` | `{ reg = "<one of enable_regs>", bit = n }` from the RM's RCC xxxENR register (the `RCC_xxxENR_<PPP>EN` bit in the header). Any firmware access while the bit is clear aborts. Omit it for always-clocked blocks (RCC, FLASH, EXTI). The clock-tree peripheral itself cannot be gated. |

**`[[analog]]`** (optional, top level, one entry per analog-capable pin): tells every `<family>.adc` model which pin feeds which ADC channel.

```toml
[[analog]]
pin = "PB1"      # P<port><0-15>; GPIO<port> must be a [[peripheral]]
adc = [1, 2]     # ADC instances that can convert this pin (non-empty, no repeats)
channel = 16     # 1..18; the same channel number on each listed ADC
```

`tools/gen_chip.py` validates the table (pin format and existing GPIO port, non-empty ADC list, channel range, each pin and each `(adc, channel)` listed once) and injects it into the params of every `<family>.adc` peripheral as `params.analog`. The pin-to-channel resolution happens in the ADC model, not in Python. Take the entries from the **datasheet's pin-definitions table** (the `ADCx_INy` alternate functions; L496: DS11585, "STM32L496xx pin definitions") for the exact package, and cross-check against ST's CubeMX MCU database XML for the part. Never write them from memory: a pin that is missing from the table is a channel with no pin on that package, which the model reports as an error. Python then uses `node.pin("PB1").set_voltage(v)` / `node.adc(1).set(16, v)`, and `Sim(strict_analog=True)` makes unset inputs abort (`docs/adding-a-peripheral.md` section 8.1). See `chips/stm32l4/parts/stm32l496rg.toml` for the L496 table.

**Core-generic blocks** (ITM, DWT, DHCSR/DEMCR) are not in the TOML: `stest::Cortexm` maps them for every part (`chips/cortexm/`, `docs/adding-a-peripheral.md` section 8.3).

Two things to watch in `params`:
- **IRQ numbers** go in `params` as the model expects them (TIM: `irq = { update = 54 }`; EXTI: an array of 16). Use the `<NAME>_IRQn` values from the device header. Shared vectors (`TIM1_UP_TIM16 = 25`) appear in more than one instance's params, and the model ORs them.
- **Reset values that differ per instance** go in `params` too, e.g. GPIOA/GPIOB debug-pin reset values in `moder_reset`/`pupdr_reset` (from the RM).

**`[irq]`**: `<NAME> = <IRQn>` for every external interrupt in the device header's `IRQn_Type`. The handler symbol is `<NAME>_IRQHandler`, as in ST's startup file. Leave reserved numbers out (L496: 79). They become `nullptr` table entries. Each IRQn must be in 0..239 and must not repeat. The table size is `16 + (highest IRQn + 1)`. Core exceptions (NMI..SysTick) are implicit.

Only list peripherals that have a model. Accesses to anything else abort as "unmodelled", which is the intended strict behaviour. For an unmodelled block that firmware merely touches, add a model; do not leave it out silently.

### 2.2 Validate the TOML

```sh
python3 tools/gen_chip.py --info chips/<family>/parts/<part>.toml        # name;family;device_define
python3 tools/gen_chip.py chips/<family>/parts/<part>.toml /tmp/gen.cpp  # full generation to a scratch file
```

Schema errors print `gen_chip: <file>: ...` and exit 1: a missing key, a bad `model` string, an unknown `rcc_enable` register, a duplicate IRQ, or an unsupported core. Read the generated file and check the following:
- the IRQ table (`/* n */ NAME_IRQHandler,` lines)
- one `bus.map` per peripheral
- `bus.set_gate(..., "RCC <REG> bit <n>")` for gated ones

Model `params` are only checked when the node starts. A bad value throws from the model constructor. Build and run a firmware to catch those (section 2.4).

### 2.3 Add a generator check

Copy the `chip_gen_stm32l496rg` test in `chips/<family>/CMakeLists.txt`. It runs `chips/stm32l4/tests/check_gen.cmake`, which asserts the following exact lines in the generated output:
- several weak-handler lines
- the `stest_chip` descriptor (table size)
- the SysTick divider
- a `bus.map`
- several `bus.set_gate` lines
- the number of weak IRQ handlers
- that RCC/FLASH are *not* gated

`check_gen.cmake` hard-codes the L496 expectations, so give the new part its own check script (or parameterise the existing one), taking the expected values from the device header. Add the test to `set_tests_properties(... TIMEOUT 30)`.

### 2.4 Add a self-test firmware

Add `selftest/firmware/<name>/` and a `stest_add_firmware(<name> CHIP <PART> ...)` in `selftest/CMakeLists.txt`. Start from blinky (`selftest/firmware/blinky/Core`: `main.c`, `stm32l4xx_it.c`, `stm32l4xx_hal_msp.c`, `Inc/stm32l4xx_hal_conf.h`). The part's clock config, GPIO, SysTick and a peripheral or two through ST's HAL are enough to prove the description. Add a pytest test with an `<name>_exe` fixture in `python/tests/conftest.py` (see `python/tests/test_exti.py`). Run the full suite natively and in Docker (commands in `docs/adding-a-peripheral.md` section 9).

## 3. Adding a new family

### 3.1 Layout

```
chips/<family>/
  CMakeLists.txt         library stest_chip_<family> + unit tests + generator check
  models/<kind>.{hpp,cpp}  namespace stest::<family>
  parts/<part>.toml
  tests/test_<kind>.cpp, tests/check_gen*.cmake
```

`chips/<family>/CMakeLists.txt` (see `chips/stm32l4/CMakeLists.txt`):

```cmake
add_library(stest_chip_<family> STATIC models/rcc.cpp models/gpio.cpp ...)
target_include_directories(stest_chip_<family> PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(stest_chip_<family> PUBLIC stest_runtime)
```

- The library name must be `stest_chip_<family>`, because `stest_add_firmware` links exactly that.
- The include dir must be `chips/`, because the generator emits `#include "<family>/models/<kind>.hpp"`.
- Add `add_subdirectory(<family>)` to `chips/CMakeLists.txt`.

### 3.2 ST sources: `cmake/StestFetchST.cmake`

1. **Pin the sources.** Add the family's HAL driver and CMSIS device repos to `_STEST_ST_PINS` as `repo|commit|sha256`, where the hash is the SHA-256 of `https://github.com/STMicroelectronics/<repo>/archive/<commit>.tar.gz`. Pin a specific commit, never a branch.
2. **Add override cache variables** following the L4 ones: `STEST_<FAMILY>_HAL_DIR` and `STEST_<FAMILY>_CMSIS_DEVICE_DIR`. Document them in the file's header comment and in `AGENTS.md`.
3. **Add a family branch to `stest_st_family_dirs`.** Today it fails with `no ST sources known for family '<family>'` for anything but `stm32l4`. The branch must return a HAL dir with `Inc/` and `Src/`, and a device dir with `Include/` and `Source/Templates/`. Keep the sanity checks (`Inc/<family>xx_hal.h`, `Include/<family>xx.h`).

`stest_add_firmware` assumes ST's naming. HAL sources are globbed as `Src/<family>xx_hal*.c` (minus `*_template.c`), and the system file is `Source/Templates/system_<family>xx.c`. If a family breaks the pattern, extend the function instead of renaming files. **Never patch ST sources** (`AGENTS.md`).

### 3.3 Models: reuse or write

Models live per family (`stest::<family>::...`). Registers that look the same often differ in reset values, bit positions or features between families. Per-family models are the default.

- **Reuse** is fine when the IP block really is identical: same `<PPP>_TypeDef`, same bit definitions, same RM behaviour. Diff the device headers and RM sections before reusing. Share code through a common base or helper, not by pointing one family's TOML `model` at another family's class (the generator derives the header path and namespace from the `model` prefix).
- **The RCC model must implement `stest::ClockTree`** (`struct ClockTree` in `runtime/include/stest/model_context.hpp`) if any peripheral uses `rcc_enable` or reads clocks. The interface currently has L4-shaped members (`pllsai1r_hz`, `pllsai2r_hz`, `adcsel`). A family with a different tree will need the interface generalised. That is a runtime change, so review it with the existing L4 users (`Tim`, the gating code generated by `gen_chip.py`).
- Write the minimum to boot ST's HAL: RCC (with ideal, instantly-ready oscillators, as in `chips/stm32l4/models/rcc.hpp`), PWR, FLASH (latency), GPIO. Then add peripherals as firmware needs them, following `docs/adding-a-peripheral.md`.

### 3.4 CMSIS-Core host shim

`chips/cmsis_host/cmsis_compiler.h` replaces CMSIS's `cmsis_compiler.h`/`cmsis_gcc.h` for every family:
- barriers become compiler barriers
- `__WFI` and PRIMASK/BASEPRI/FAULTMASK call the runtime
- the bit intrinsics are portable C
- exclusives always succeed

`stest_cmsis_core_include()` stages the CMSIS-Core headers *without* the toolchain headers, so that `core_cmX.h`'s `#include "cmsis_compiler.h"` finds the shim. Reuse both unchanged for any Cortex-M4 family.

### 3.5 A different Cortex-M core (e.g. STM32U3, Cortex-M33): open work

None of the following exists yet. These are the known places that assume Cortex-M4:

- **`tools/gen_chip.py`** rejects any `core` other than `"cortex-m4"`, and limits IRQn to 0..239.
- **`stest_cmsis_core_include()`** looks for `core_cm4.h` to locate the CMSIS-Core include dir. The M33 device header includes `core_cm33.h`.
- **`chips/cmsis_host/cmsis_compiler.h`** does not provide ARMv8-M intrinsics: TrustZone, MSPLIM/PSPLIM, LDA/STL (see its header comment). These would need host implementations, or explicit aborts if firmware uses them.
- **`stest::Cortexm`** (`runtime/include/stest/cortexm.hpp`, `runtime/src/cortexm.cpp`) models an M4:
  - CPUID reads `0x410FC241` (Cortex-M4 r0p1)
  - the NVIC is sized for 240 external IRQs
  - the SCB block covers CPUID..CPACR

  An M33 needs its own identification, any SCB/NVIC differences the HAL touches, and a decision on how to treat the security extension. The likely shape is a `core` value mapped to core options in the generated `ChipDesc`, but that design is not settled.
- **Address windows** (`mmio_windows.hpp`) cover `0x40000000-0x5FFFFFFF`, which includes the secure peripheral aliases at `0x5xxxxxxx`. Decide whether the secure alias of a block maps to the same model.

Record decisions about these in the relevant plan (`docs/superpowers/plans/`) before implementing them.

### 3.6 Python

Add typed helpers for the family's peripherals to `python/stest/peripherals.py` only where an existing helper does not fit (today: `node.timer()`, `node.adc()`, `node.dac()`, `node.spi()`). Helpers talk to models through `call()` ops, so they are family-neutral as long as the ops and state schemas match.

## 4. Checklists

**New part in an existing family**

- [ ] `chips/<family>/parts/<part>.toml`: `[chip]`, `[clock_tree]`, one `[[peripheral]]` per modelled instance (base and size from the device header, `rcc_enable` bits, `params` incl. IRQ numbers and per-instance reset values), `[irq]` from `IRQn_Type`.
- [ ] `[[analog]]` entries (if the part has an ADC) taken from the datasheet pin table and cross-checked with the CubeMX XML.
- [ ] `python3 tools/gen_chip.py --info` and a scratch generation succeed. The generated IRQ table, maps and gates have been reviewed.
- [ ] A generator check test (`check_gen`-style) in `chips/<family>/CMakeLists.txt`, with `TIMEOUT 30`.
- [ ] A self-test firmware (`selftest/firmware/<name>/`, `stest_add_firmware(... CHIP <PART> ...)`) and a pytest e2e test.
- [ ] The full suite passes natively and in Docker.

**New family**

- [ ] All of the above, plus:
- [ ] `chips/<family>/CMakeLists.txt` with `stest_chip_<family>` (include dir `chips/`), unit-test executables and `chip_<family>_*` CTests. `add_subdirectory` in `chips/CMakeLists.txt`.
- [ ] `cmake/StestFetchST.cmake`: pinned `repo|commit|sha256` entries, `STEST_<FAMILY>_*_DIR` override variables, a branch in `stest_st_family_dirs`.
- [ ] Models in `chips/<family>/models/`: at least RCC (implementing `ClockTree`), PWR, FLASH and GPIO, each with doctest tests.
- [ ] If the core is not Cortex-M4, the open items in section 3.5 have been designed (plan) and implemented.
- [ ] `AGENTS.md` repo map and setup notes, `README.md` if user-visible, and these docs updated.
