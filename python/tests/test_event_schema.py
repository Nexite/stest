"""Event schema: every model event names the peripheral instance that emitted it."""

from __future__ import annotations

import json

from stest import Sim


def _events(exe, ms: float, setup=None) -> list[dict]:
    with Sim() as s:
        n = s.node("n", exe)
        s.start()
        if setup:
            setup(n)
        s.advance(ms=ms)
        return [json.loads(js) for _, _, js in s.event_log]


def test_every_event_carries_periph(blinky_exe, dac_exe, spi_exe, itm_exe):
    evs = _events(blinky_exe, 1100) + _events(dac_exe, 2) + _events(spi_exe(), 15) + _events(itm_exe, 250)
    kinds = {e["kind"] for e in evs}
    assert {"gpio", "dac", "spi", "itm"} <= kinds
    for e in evs:
        assert isinstance(e.get("periph"), str) and e["periph"], e
        if e["kind"] == "gpio":
            assert e["periph"] == "GPIO" + e["port"]
        elif e["kind"] == "dac":
            assert e["periph"] == "DAC1"
        elif e["kind"] == "spi":
            assert e["periph"] == "SPI1" and "bus" not in e
        elif e["kind"] == "itm":
            assert e["periph"] == "ITM"
