"""EXTI: a GPIO edge on PC13 reaches HAL_GPIO_EXTI_Callback (selftest firmware `exti`)."""

from __future__ import annotations

from stest import Sim


def test_button_edge_triggers_callback(exti_exe):
    with Sim() as s:
        n = s.node("n", exti_exe)
        s.start()
        s.advance(ms=2)
        led = n.pin("PA5")
        btn = n.pin("PC13")
        assert led.level == 0 and n.var("exti_count") == 0
        btn.set_high()
        s.advance(ms=1)
        assert led.level == 1 and n.var("exti_count") == 1
        btn.set_low()  # falling edge: only RTSR is armed
        s.advance(ms=1)
        assert led.level == 1 and n.var("exti_count") == 1
        btn.set_high()
        s.advance(ms=1)
        assert led.level == 0 and n.var("exti_count") == 2
        s.advance(ms=5)  # no further toggles without edges
        assert n.var("exti_count") == 2
        # exactly one callback per rising edge, edge timestamps in the node event log
        assert [lvl for _, lvl in led.history] == [1, 0]


def test_exti_is_deterministic(exti_exe):
    def run():
        with Sim() as s:
            n = s.node("n", exti_exe)
            s.start()
            s.advance(ms=2)
            n.pin("PC13").set_high()
            s.advance(ms=1)
            n.pin("PC13").set_low()
            n.pin("PC13").set_high()
            s.advance(ms=3)
            return [(t, nd, str(j)) for t, nd, j in s.event_log]

    assert run() == run()
