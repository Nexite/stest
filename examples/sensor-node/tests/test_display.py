"""Both boards on one CAN bus: the display follows the sensor's readings."""

from __future__ import annotations

BITRATE = 500_000


def test_two_boards(sim, firmware):
    bus = sim.can_bus("can1", BITRATE)
    sensor = sim.node("sensor", firmware("sensor"), can={"CAN1": bus})
    display = sim.node("display", firmware("display"), can={"CAN1": bus})
    sim.start()
    led = display.pin("PA5")

    sensor.pin("PC0").set_voltage(2.5)
    led.wait_high(timeout_ms=300)  # virtual time
    assert abs(display.var("g_last_millivolts") - 2500) <= 2

    sensor.pin("PC0").set_voltage(1.0)
    led.wait_low(timeout_ms=300)
    assert abs(display.var("g_last_millivolts") - 1000) <= 2
