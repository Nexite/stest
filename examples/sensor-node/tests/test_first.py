"""A minimal first test: one board, GPIO only (a button in, an LED out) and one variable.

This is the shape of a first test for any board. The sensor board also sends a CAN frame every
100 ms, which needs a CAN bus (see test_sensor.py); this test ends at 20 ms, before the first
frame, so it needs no bus. For your own GPIO-only board, drop that concern.
"""


# --8<-- [start:first]
def test_button_press_toggles_led(sim, firmware):
    node = sim.node("app", firmware("sensor"))
    sim.start()
    button, led = node.pin("PC13"), node.pin("PA5")
    button.set_high()  # released
    sim.advance(ms=10)  # virtual time: let main() initialise
    before = led.level

    button.set_low()  # press: the falling edge fires the EXTI interrupt
    sim.advance(ms=5)
    button.set_high()
    sim.advance(ms=5)

    assert led.level == 1 - before
    assert node.var("g_button_count") == 1  # a firmware global, by name
# --8<-- [end:first]
