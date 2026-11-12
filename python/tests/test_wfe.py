"""WFE and the event register: ST's HAL_PWR_EnterSLEEPMode(..., PWR_SLEEPENTRY_WFE) runs
`__SEV(); __WFE(); __WFE();` (SEV sets the event register, the first WFE consumes it, the
second sleeps until a wake-up). With SysTick suspended, one EXTI interrupt must wake it."""

from __future__ import annotations

from stest import Sim


def test_hal_sleep_wfe_wakes_on_one_exti(exti_wfe_exe):
    with Sim() as s:
        n = s.node("n", exti_wfe_exe)
        s.start()
        s.advance(ms=5)
        assert n.var("sleep_wakes") == 0  # asleep: SysTick suspended, nothing pending
        btn = n.pin("PC13")
        btn.set_high()  # one rising edge: one EXTI interrupt
        s.advance(ms=1)
        assert n.var("exti_count") == 1
        assert n.var("sleep_wakes") == 1  # woke once and went back to sleep
        s.advance(ms=5)
        assert n.var("sleep_wakes") == 1
        btn.set_low()
        btn.set_high()
        s.advance(ms=1)
        assert n.var("exti_count") == 2
        assert n.var("sleep_wakes") == 2
