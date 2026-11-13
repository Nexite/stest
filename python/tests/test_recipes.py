"""The Writing tests cookbook (docs/guide/writing-tests/): every recipe there is quoted from this
file (pymdownx.snippets section markers), so each one runs in CI against the self-test firmware.

``firmware("<name>")`` (conftest.py) is the path of a self-test executable in build/selftest, the
same shape as the ``firmware`` fixture of the Getting started guide. Keep each recipe a complete
test: the pages show the code between the markers and nothing else.
"""

from __future__ import annotations

# --8<-- [start:imports]
import json

import pytest

from stest import NodeCrashed, Sim, WaitTimeout, WallClockTimeout
# --8<-- [end:imports]


# ---- index.md: the sim fixture, time, waits ----------------------------------------------------

# --8<-- [start:basics]
def test_advance_and_wait(sim, firmware):
    node = sim.node("app", firmware("blinky"))  # one MCU, run as its own process
    sim.start()  # every node starts at virtual t = 0

    sim.advance(ms=10)  # run 10 ms of virtual time
    assert sim.now_ns == 10_000_000

    # Wait for a condition, checked at every quantum boundary (1 ms by default).
    sim.wait_for(lambda: node.var("uwTick") >= 100, timeout_ms=200)
    assert 100_000_000 <= sim.now_ns <= 101_000_000
# --8<-- [end:basics]


# --8<-- [start:timeout]
def test_wait_timeout(sim, firmware):
    node = sim.node("app", firmware("blinky"))
    sim.start()
    led = node.pin("PA5")

    with pytest.raises(WaitTimeout):
        led.wait_high(timeout_ms=100)  # blinky's first edge is at 500 ms
    assert sim.now_ns == 100_000_000  # the budget was spent; the simulation goes on

    led.wait_high(timeout_ms=500)
    assert led.level == 1
# --8<-- [end:timeout]


# --8<-- [start:no-fixture]
def test_without_the_fixture(firmware):
    with Sim(quantum_us=100) as sim:  # closed (every node stopped) at the end of the block
        node = sim.node("app", firmware("blinky"))
        sim.start()
        node.pin("PA5").wait_high(timeout_ms=600)
# --8<-- [end:no-fixture]


# ---- pins.md -----------------------------------------------------------------------------------

# --8<-- [start:button]
def test_button_interrupt(sim, firmware):
    # exti: PC13 is a rising-edge EXTI input with a pull-down; the callback toggles PA5
    # and counts in exti_count.
    node = sim.node("app", firmware("exti"))
    sim.start()
    sim.advance(ms=2)  # let main() configure the pins
    button, led = node.pin("PC13"), node.pin("PA5")

    button.set_high()  # rising edge -> EXTI interrupt
    sim.advance(ms=1)
    assert led.level == 1 and node.var("exti_count") == 1

    button.set_low()  # falling edge: not armed
    sim.advance(ms=1)
    assert node.var("exti_count") == 1

    button.release()  # stop driving: the pin reads its pull-down again
    assert button.level == 0
# --8<-- [end:button]


# --8<-- [start:history]
def test_led_timing(sim, firmware):
    node = sim.node("app", firmware("blinky"))
    sim.start()
    led = node.pin("PA5")
    sim.advance(ms=2100)

    # (t_ns, level) for every change, at the exact virtual time it happened
    assert [level for _, level in led.history] == [1, 0, 1, 0]
    times = [t for t, _ in led.history]
    periods = [b - a for a, b in zip(times, times[1:])]
    assert periods == [500_000_000] * 3  # exactly 500 ms apart
# --8<-- [end:history]


# --8<-- [start:drive-at]
def test_pulse_at_exact_times(sim, firmware):
    node = sim.node("app", firmware("exti"))
    sim.start()
    sim.advance(ms=2)
    button = node.pin("PC13")

    # Queue a 100 us pulse at t = 2.5 ms, in the middle of the next quantum.
    button.drive_at(1, 2_500_000)
    button.drive_at(0, 2_600_000)
    button.drive_at(None, 2_700_000)  # release
    sim.advance(ms=2)

    assert button.history == [(2_500_000, 1), (2_600_000, 0)]
    (t_led, level), = node.pin("PA5").history
    assert level == 1 and 2_500_000 < t_led < 2_510_000  # the interrupt ran right after the edge
# --8<-- [end:drive-at]


# ---- variables.md (blinky's Core/Src/vars.c) ---------------------------------------------------

# --8<-- [start:read]
def test_read_variables(sim, firmware):
    node = sim.node("app", firmware("blinky"))
    sim.start()

    assert node.var("u8") == 200  # uint8_t u8 = 200;
    assert node.var("s16") == -5  # int16_t s16 = -5;
    assert node.var("f") == 1.5  # float f = 1.5f;

    mode = node.var("mode")  # enum mode { MODE_OFF, MODE_ON } mode = MODE_ON;
    assert mode == "MODE_ON" and mode == 1

    # struct lamp { uint16_t duty; bool on; } lamps[3] = {{10, 1}, {20, 0}, {30, 1}};
    assert node.var("lamps[1].duty") == 20
    assert dict(node.var("lamps[2]")) == {"duty": 30, "on": True}
    assert [lamp["duty"] for lamp in node.var("lamps")] == [10, 20, 30]

    assert node.var("tick_fn::calls") >= 0  # a static local of tick_fn()
# --8<-- [end:read]


# --8<-- [start:write]
def test_write_a_variable(sim, firmware):
    node = sim.node("app", firmware("blinky"))
    sim.start()

    node.set_var("lamps[0].duty", 99)  # scalars only; set struct members one by one
    node.set_var("mode", "MODE_OFF")  # an enum by name (or by value)
    sim.advance(ms=1000)

    assert node.var("seen_duty") == 99  # the firmware copied it in tick_fn()
    assert node.var("mode") == "MODE_OFF"
# --8<-- [end:write]


# ---- output.md ---------------------------------------------------------------------------------

# --8<-- [start:output]
def test_firmware_output(sim, firmware):
    # itm_mixed logs "out <i>" with printf, "itm <i>" on ITM port 0 10 ms later, then
    # "pc <c>!" through stdio, every 100 ms.
    node = sim.node("app", firmware("itm_mixed"))
    sim.start()

    m = node.wait_for_log(r"itm (\d+)", timeout_ms=500)  # ITM or stdout, whichever comes
    assert m.group(1) == "0"
    sim.advance(ms=200)

    assert node.output[:3] == ["out 0", "itm 0", "pc A!"]  # both, in virtual-time order
    assert node.log[:2] == ["itm 0", "itm 1"]  # ITM port 0 only
# --8<-- [end:output]


# --8<-- [start:wait-for-log]
def test_wait_for_successive_lines(sim, firmware):
    node = sim.node("app", firmware("itm"))  # logs "hello <i>" every 100 ms
    sim.start()

    first = node.wait_for_log(r"hello (\d+)", timeout_ms=500)
    second = node.wait_for_log(r"hello (\d+)", timeout_ms=500)  # the next line, not the same
    assert (first.group(1), second.group(1)) == ("0", "1")

    with pytest.raises(WaitTimeout):
        node.wait_for_log(r"goodbye", timeout_ms=150)
# --8<-- [end:wait-for-log]


# --8<-- [start:events]
def test_output_events(sim, firmware):
    node = sim.node("app", firmware("itm"))
    sim.start()
    sim.advance(ms=250)

    lines = [(t, json.loads(e)) for t, name, e in sim.event_log if name == node.name]
    itm = [(t, e["text"]) for t, e in lines if e["kind"] == "itm"]
    assert [text for _, text in itm] == ["hello 0", "hello 1", "hello 2"]
    assert round((itm[1][0] - itm[0][0]) / 1e6) == 100  # 100 ms apart, in virtual time
# --8<-- [end:events]


# ---- peripherals.md ----------------------------------------------------------------------------

# --8<-- [start:timer]
def test_timer_rate_and_software_pwm(sim, firmware):
    # tim: TIM7 at 20 kHz; its update interrupt runs a 25 % software PWM on PA6.
    node = sim.node("app", firmware("tim"))
    sim.start()
    sim.advance(ms=1)

    tim7 = node.timer("TIM7")
    assert tim7.running and tim7.update_hz == pytest.approx(20_000)
    assert tim7.state()["psc"] == 0 and tim7.state()["arr"] == 3999

    t0 = sim.now_ns
    sim.advance(ms=50)
    edges = [(t, lv) for t, lv in node.pin("PA6").history if t >= t0]
    high = sum(b[0] - a[0] for a, b in zip(edges, edges[1:]) if a[1] == 1)
    assert high / (edges[-1][0] - edges[0][0]) == pytest.approx(0.25, abs=0.02)
# --8<-- [end:timer]


# --8<-- [start:adc]
def test_adc_input(sim, firmware):
    # adc: converts ADC1 channel 16 (PB1) every 1 ms into last_raw (12 bit, VREF+ 3.3 V).
    node = sim.node("app", firmware("adc"))
    sim.start()
    node.pin("PB1").set_voltage(1.65)  # any time, also before the firmware enables clocks
    sim.advance(ms=2)
    assert abs(node.var("last_raw") - 2048) <= 1

    node.adc(1).set(16, 0.5)  # the same pin, by ADC channel
    sim.advance(ms=2)
    assert node.var("last_raw") == round(0.5 / 3.3 * 4095)
# --8<-- [end:adc]


# --8<-- [start:dac]
def test_dac_output(sim, firmware):
    # dac: DAC1 channel 1 (PA4) set to 2048 of 4095.
    node = sim.node("app", firmware("dac"))
    sim.start()
    sim.advance(ms=1)

    out = node.dac(1).channel(1)
    assert out.enabled and out.dor == 2048
    assert out.volts == pytest.approx(1.65, abs=0.001)
    assert node.pin("PA4").voltage == out.volts  # the pin carries the output
# --8<-- [end:dac]


# --8<-- [start:spi]
def test_spi_transfer(sim, firmware):
    # spi: SPI1 master; every 10 ms CS (PA4) low, send A5 01 02 03, receive into rx[4], CS high.
    node = sim.node("app", firmware("spi"))
    sim.start()
    spi1 = node.spi("SPI1")
    spi1.queue_miso(bytes([0x10, 0x20, 0x30, 0x40]))  # what the device answers

    sim.advance(ms=15)
    assert b"".join(b.mosi for b in spi1.bursts) == bytes([0xA5, 0x01, 0x02, 0x03])
    assert node.var("rx") == [0x10, 0x20, 0x30, 0x40]

    # Bursts carry virtual times: check they fall inside the chip-select low window.
    cs = node.pin("PA4").history
    assert [level for _, level in cs] == [1, 0, 1]  # idle high, then one transfer
    t_low, t_high = cs[1][0], cs[2][0]
    assert all(t_low <= b.t_start_ns and b.t_end_ns <= t_high for b in spi1.bursts)
# --8<-- [end:spi]


# ---- multi-node.md -----------------------------------------------------------------------------

# --8<-- [start:two-nodes]
def test_two_boards(sim, firmware):
    led_board = sim.node("led", firmware("blinky"))
    button_board = sim.node("button", firmware("exti"))
    sim.start()

    sim.advance(ms=600)  # both nodes run the same 600 ms, quantum by quantum
    assert led_board.now_ns == button_board.now_ns == sim.now_ns
    assert led_board.pin("PA5").level == 1
    assert button_board.var("exti_count") == 0

    # sim.event_log merges both nodes' events by virtual time
    names = {name for _, name, _ in sim.event_log}
    assert names == {"led"}  # only blinky has changed a pin so far
# --8<-- [end:two-nodes]


# --8<-- [start:can]
def test_two_boards_on_can(sim, firmware):
    # can_node1 / can_node2: CAN2 at 500 kbit/s, each sends id 0x100 + its node id every 10 ms.
    bus = sim.can_bus("can", 500_000)
    sim.node("n1", firmware("can_node1"), can={"CAN2": bus})
    sim.node("n2", firmware("can_node2"), can={"CAN2": bus})
    sim.start()

    frame = bus.wait_for_frame(id=0x102, timeout_ms=50)
    assert frame.sender == "n2"
# --8<-- [end:can]


# ---- faults.md ---------------------------------------------------------------------------------

# --8<-- [start:crash]
def test_unmodelled_access_aborts(sim, firmware):
    # faults_unmodelled writes a register no model covers, 10 ms after start.
    sim.node("app", firmware("faults_unmodelled"))
    sim.start()

    with pytest.raises(NodeCrashed, match=r"stest: unmodelled write32 0x40013800") as crash:
        sim.advance(ms=100)
    assert "aborted at virtual t=10.000 ms" in str(crash.value)
# --8<-- [end:crash]


# --8<-- [start:strict-analog]
@pytest.mark.stest(strict_analog=True)
def test_unset_analog_input_aborts(sim, firmware):
    sim.node("app", firmware("adc"))  # converts PB1, which this test never sets
    sim.start()
    with pytest.raises(NodeCrashed, match="ADC1 channel 16 converted with no input set"):
        sim.advance(ms=5)
# --8<-- [end:strict-analog]


# --8<-- [start:no-debugger]
@pytest.mark.stest(debugger_attached=False)
def test_itm_silent_without_debugger(sim, firmware):
    node = sim.node("app", firmware("itm"))  # plain CMSIS ITM_SendChar
    sim.start()
    sim.advance(ms=250)
    assert node.log == []  # bare board: nobody enabled the ITM, output is dropped
    assert node.var("loops") >= 2  # and the firmware did not block on it
# --8<-- [end:no-debugger]


# --8<-- [start:poll-skip]
@pytest.mark.stest(poll_skip=0)
def test_counting_loop_exact(sim, firmware):
    # pollcount counts 1000 iterations over a constant register, timed with SysTick
    # (4 MHz: 250 ns per tick). Every register access costs 25 ns of virtual time.
    node = sim.node("app", firmware("pollcount"))
    sim.start()
    sim.wait_for(lambda: node.var("done") == 1, timeout_ms=2000)
    elapsed_ns = node.var("elapsed_ticks") * 250
    assert 1000 * 25 <= elapsed_ns <= 2 * 1000 * 25  # without poll fast-forward: exact
# --8<-- [end:poll-skip]


# --8<-- [start:hang]
@pytest.mark.stest(wall_timeout_s=2)
def test_hung_firmware(sim, firmware):
    # faults_spin spins without touching a register after 10 ms: virtual time stops.
    sim.node("app", firmware("faults_spin"))
    sim.start()
    with pytest.raises(WallClockTimeout, match=r"node 'app' did not answer"):
        sim.advance(ms=100)
# --8<-- [end:hang]


# ---- pytest.md ---------------------------------------------------------------------------------

# --8<-- [start:marker]
@pytest.mark.stest(quantum_us=100)
def test_with_a_finer_quantum(sim, firmware):
    assert sim.quantum_us == 100
    node = sim.node("app", firmware("blinky"))
    sim.start()
    node.pin("PA5").wait_high(timeout_ms=600)
    assert sim.now_ns <= 500_100_000  # waits now stop within 100 us of the edge
# --8<-- [end:marker]


# --8<-- [start:determinism]
def test_same_log_for_any_quantum(firmware):
    def run(quantum_us):
        with Sim(quantum_us=quantum_us) as sim:
            node = sim.node("app", firmware("exti"))
            sim.start()
            node.pin("PC13").drive_at(1, 3_000_000)  # an exact-time input
            sim.advance(ms=10)
            return sim.event_log

    assert run(1000) == run(1000) == run(250)
# --8<-- [end:determinism]
