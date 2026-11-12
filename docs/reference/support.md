# Support matrix

What stest simulates today: the parts, each peripheral model with what it covers and what it
leaves out, and the hosts it runs on. Anything not listed here is not modelled, and firmware that
touches it [aborts](firmware.md#unmodelled-address) instead of getting made-up values.

Each model's header comment is its full specification: registers, behaviour, the events it
emits, the Python ops it answers and every deliberate deviation. The tables link to it.

## Parts

| Part | Family | Core | Notes |
|---|---|---|---|
| STM32L496RG | STM32L4 | Cortex-M4, 4 NVIC priority bits | Part description: [`chips/stm32l4/parts/stm32l496rg.toml`](https://github.com/Nexite/stest/blob/main/chips/stm32l4/parts/stm32l496rg.toml) (addresses, interrupts, RCC enable bits, analog pins). |

Firmware for another STM32L4 part may build against this one if it uses only peripherals and
addresses the L496 has. Adding a part or family: [Adding a chip](../adding-a-chip.md).

## Hosts

| Host | Status |
|---|---|
| macOS arm64 (Apple silicon), native | Supported; tested by CI (macos-15). |
| Linux x86-64, native or Docker | Supported; tested by CI (the Ubuntu 24.04 Docker image, `docker/Dockerfile`). |
| Linux arm64, native or Docker | Supported; not tested by CI. |
| Windows | Through the Docker image or the dev container (`.devcontainer/`); not tested by CI. |
| macOS x86-64 | Not tested. |

The toolchain needs upstream LLVM 18 or newer; see [Install](../install.md).

## Core (every chip)

| Block | Instances | Modelled | Not modelled, deviations | Header |
|---|---|---|---|---|
| NVIC, SCB, SysTick | one | Priorities and preemption (PRIGROUP, BASEPRI, PRIMASK, FAULTMASK), pending and active state, tail-chaining; SysTick from `SystemCoreClock` (or the external reference, HCLK/8 on the L4); `__WFI`, `__WFE`, `__SEV`; SCB ICSR, VTOR, AIRCR (PRIGROUP), SCR, CCR, SHPR, SHCSR, CPACR. | No fault exceptions: a bad pointer crashes the host process (`NodeCrashed` with a signal), and CFSR/HFSR read 0. `NVIC_SystemReset()` (AIRCR.SYSRESETREQ) and NMI abort. `SCR.SLEEPDEEP` is stored: Stop and Standby entry sleep like WFI with every clock running. | [`cortexm.hpp`](https://github.com/Nexite/stest/blob/main/runtime/include/stest/cortexm.hpp) |
| ITM | one | Stimulus ports into `node.log` / `node.output`, TER/TPR/TCR, the CoreSight lock (LAR/LSR), with or without an attached debugger. | No trace port (SWO), timestamps or synchronisation packets: those TCR bits are stored. | [`itm.hpp`](https://github.com/Nexite/stest/blob/main/chips/cortexm/itm.hpp) |
| DWT | one | `CYCCNT` (from virtual time and `SystemCoreClock`), `CTRL.CYCCNTENA`, LAR/LSR. | Comparators (watchpoints), PC sampling and the profiling counters abort or never count. | [`dwt.hpp`](https://github.com/Nexite/stest/blob/main/chips/cortexm/dwt.hpp) |
| CoreDebug | one | `DHCSR.C_DEBUGEN` (read-only to software), `DEMCR.TRCENA`. | Halting, stepping, vector catch and monitor mode abort. | [`coredebug.hpp`](https://github.com/Nexite/stest/blob/main/chips/cortexm/coredebug.hpp) |

## STM32L4 peripherals

| Peripheral | Instances | Modelled | Not modelled, deviations | Python | Header |
|---|---|---|---|---|---|
| RCC | RCC | Every register with its reset value; the clock tree (MSI, HSI16, HSE, PLL, PLLSAI1/2, AHB/APB prescalers) computed live from the registers; peripheral clock gating (a gated peripheral aborts while its enable bit is clear). | Oscillators and PLLs are ready at once. HSE is 8 MHz (a part parameter). | | [`rcc.hpp`](https://github.com/Nexite/stest/blob/main/chips/stm32l4/models/rcc.hpp) |
| PWR | PWR | CR1-CR4, SR1/SR2, SCR, pull-up/down control registers. | Voltage scaling is instant (`VOSF` reads 0). Low-power mode selection is stored only. | | [`pwr.hpp`](https://github.com/Nexite/stest/blob/main/chips/stm32l4/models/pwr.hpp) |
| FLASH | FLASH | `ACR` (latency, caches, prefetch), the `KEYR` unlock sequence, option bytes at factory defaults. | Programming and erase abort. Latency changes are instant. | | [`flash.hpp`](https://github.com/Nexite/stest/blob/main/chips/stm32l4/models/flash.hpp) |
| GPIO | GPIOA-GPIOI | Modes, pulls, open drain, `ODR`/`BSRR`/`BRR`, `LCKR` locking, alternate-function registers; external drive and analog voltages from the test; a pin event per level change. HAL mode (`MODELS GPIO=hal`). | Alternate functions are stored, not connected to peripherals (an SPI's pins do not toggle). No `ASCR` on the L496. | [`Pin`][stest.Pin] | [`gpio.hpp`](https://github.com/Nexite/stest/blob/main/chips/stm32l4/models/gpio.hpp) |
| SYSCFG | SYSCFG | `EXTICR1-4` (which port drives each EXTI line); other registers stored. | Memory remap, FPU interrupts and SRAM2 erase have no effect. | | [`syscfg.hpp`](https://github.com/Nexite/stest/blob/main/chips/stm32l4/models/syscfg.hpp) |
| EXTI | EXTI | Lines 0-15 from GPIO edges: rising/falling selection, `PR` (write 1 to clear), masks, software interrupts, the shared vectors (EXTI9_5, EXTI15_10). | Lines 16 and above (internal sources) have no source. Event mode (`EMR`) is stored only. | [`Pin`][stest.Pin] | [`exti.hpp`](https://github.com/Nexite/stest/blob/main/chips/stm32l4/models/exti.hpp) |
| TIM | TIM1-8, TIM15-17 | Basic, general-purpose and advanced timers: up/down/center-aligned counting, prescaler, auto-reload and preload, repetition counter, one-pulse mode, update and compare interrupts, output compare and PWM modes, complementary outputs with dead time; exact in virtual time, also across clock changes. | Input capture, encoder and slave modes, external clock mode 2, DMA, break inputs, channels 5 and 6, OCREF clear, option-register remaps abort. Outputs are not driven onto pins: read the duty from the helper. | [`Timer`][stest.Timer] | [`tim.hpp`](https://github.com/Nexite/stest/blob/main/chips/stm32l4/models/tim.hpp) |
| ADC | ADC1-3 (one block) | Calibration, enable, single conversions of one channel by software start, resolution, alignment, sampling time, overrun, interrupts; synchronous and asynchronous clocks; inputs from pin voltages, VREFINT. | Hardware triggers, DMA, continuous, scan and discontinuous modes, injected conversions, analog watchdogs, oversampling, offsets, differential inputs, dual mode, temperature sensor, VBAT and DAC-internal channels abort. An unset input reads 0 V with a warning ([strict analog](../guide/writing-tests/faults.md#require-every-analog-input) aborts). | [`Adc`][stest.Adc] | [`adc.hpp`](https://github.com/Nexite/stest/blob/main/chips/stm32l4/models/adc.hpp) |
| DAC | DAC1 (2 channels) | Both channels, all data holding register formats, software trigger, output onto PA4 / PA5 (readable by the ADC). | Hardware triggers, wave generation, DMA, calibration and sample-and-hold mode abort. | [`Dac`][stest.Dac] | [`dac.hpp`](https://github.com/Nexite/stest/blob/main/chips/stm32l4/models/dac.hpp) |
| SPI | SPI1-3 | Full-duplex master with software NSS, 4-16 bit frames, the TX/RX FIFOs, timing from the baud-rate prescaler, interrupts; MOSI capture and queued MISO bytes. | Slave mode, hardware NSS, bidirectional and receive-only modes, CRC, TI mode and DMA abort. CPOL, CPHA and bit order are reported, not simulated on a wire. | [`SpiBus`][stest.SpiBus] | [`spi.hpp`](https://github.com/Nexite/stest/blob/main/chips/stm32l4/models/spi.hpp) |
| bxCAN | CAN1, CAN2 | Init, sleep and normal modes, bit timing, TX mailboxes and priorities, filters (CAN2 uses CAN1's banks), RX FIFOs, interrupts, error counters, error passive, bus-off and automatic recovery; frames on simulated buses. | Time-triggered mode, automatic wake-up, loop-back and silent modes, DLC > 8 abort. Strict where hardware ignores a write (BTR outside init, a busy mailbox). More in [CAN, Deviations and limits](../can.md#deviations-and-limits). | [`CanBus`][stest.CanBus] | [`bxcan.hpp`](https://github.com/Nexite/stest/blob/main/chips/stm32l4/models/bxcan.hpp) |

**Not modelled** (any access aborts as `unmodelled`): among others USART/UART and LPUART, I2C,
DMA and DMAMUX, RTC, the watchdogs (IWDG, WWDG), LPTIM, CRC, RNG, USB, QUADSPI, SDMMC,
comparators, op-amps, DFSDM, SAI and the MPU. Memory is the host's: flash and SRAM contents are
the firmware's ordinary variables, and there is no memory map below the peripherals.

## Python access

The Python column names the helper for each model. The helpers wrap the model's ops, which
[`node.call(model, op, ...)`][stest.Node.call] and [`node.post(...)`][stest.Node.post] reach
directly; each header documents its ops. Models without a Python column have no ops: the
firmware drives them, and tests observe their effects. To add a peripheral, see
[Adding a peripheral](../adding-a-peripheral.md).
