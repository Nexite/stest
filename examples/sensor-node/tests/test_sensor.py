"""The sensor board on its own: button, LED and the SensorReading frames it sends."""

from __future__ import annotations

# --8<-- [start:imports]
from stest import CanDevice

BITRATE = 500_000  # must match the firmware's CAN bit timing exactly
# --8<-- [end:imports]


# --8<-- [start:start]
def start_sensor(sim, firmware):
    """Start the sensor board on a CAN bus with one listener.

    A CAN frame needs an acknowledgement: without another node on the bus, the sensor would
    retry its first frame forever. A plain CanDevice acknowledges every frame."""
    bus = sim.can_bus("can1", BITRATE)
    bus.attach(CanDevice("listener"))
    sensor = sim.node("sensor", firmware("sensor"), can={"CAN1": bus})
    sim.start()
    sensor.pin("PC13").set_high()  # button released (the input also has a pull-up)
    sim.advance(ms=10)  # let main() finish its initialisation
    return sensor, bus
# --8<-- [end:start]


# --8<-- [start:button]
def press(sim, button):
    """One button press: PC13 low (the falling edge fires EXTI), then high again."""
    button.set_low()
    sim.advance(ms=5)
    button.set_high()
    sim.advance(ms=5)


def test_button_toggles_led(sim, firmware):
    sensor, _ = start_sensor(sim, firmware)
    led, button = sensor.pin("PA5"), sensor.pin("PC13")
    initial = led.level
    changes = len(led.history)

    press(sim, button)
    assert led.level == 1 - initial
    assert len(led.history) == changes + 1  # exactly one toggle per press

    press(sim, button)
    assert led.level == initial
# --8<-- [end:button]


def test_reading_sent_on_can(sim, firmware, dbc):
    sensor, bus = start_sensor(sim, firmware)
    sensor.pin("PC0").set_voltage(1.65)
    sim.advance(ms=350)

    _, signals = bus.wait_for_message(dbc, "SensorReading", timeout_ms=200)
    assert abs(signals["millivolts"] - 1650) <= 2

    times = [t for t, name, _ in bus.messages(dbc) if name == "SensorReading"]
    assert len(times) >= 3
    # one frame every 100 ms (t is the end of frame; allow a few bit times of jitter)
    assert all(abs((b - a) - 100_000_000) < 50_000 for a, b in zip(times, times[1:]))
    assert sensor.var("g_sample_count") >= len(times)


def test_button_count_in_frame(sim, firmware, dbc):
    sensor, bus = start_sensor(sim, firmware)
    for _ in range(3):
        press(sim, sensor.pin("PC13"))

    # the presses end at 40 ms, before the first frame at 100 ms
    _, signals = bus.wait_for_message(dbc, "SensorReading", timeout_ms=200)
    assert signals["button_count"] == 3
