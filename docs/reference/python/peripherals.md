# Peripheral helpers

Typed wrappers over the peripheral models' `call` ops and events. Get them from a node:
[`node.timer("TIM7")`][stest.Node.timer], [`node.adc(1)`][stest.Node.adc],
[`node.dac(1)`][stest.Node.dac] and [`node.spi("SPI1")`][stest.Node.spi]. Creating a helper
sends nothing; each query goes to the node between quanta, so values are as of the last
completed quantum. Voltages are in volts, frequencies in Hz, times in virtual ns.

For GPIO, see [`Pin`][stest.Pin]. For models without a helper, use
[`Node.call`][stest.Node.call] and [`Node.post`][stest.Node.post] with the ops documented in
the model's header under `chips/<family>/models/`.

::: stest.Timer

::: stest.TimerChannel

::: stest.Adc

::: stest.Dac

::: stest.DacChannel

::: stest.SpiBus

::: stest.SpiBurst
