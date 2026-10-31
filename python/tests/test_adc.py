"""ADC (selftest firmware `adc` and its variants) and DAC1 (`dac`), analog pin voltages and
strict analog inputs."""

from __future__ import annotations

import json

import pytest

from stest import Sim
from stest.errors import NodeCrashed, StestError

UNSET = "ADC1 channel 16 converted with no input set"


def test_adc_reads_voltage(adc_exe):
    with Sim() as s:
        n = s.node("n", adc_exe())
        s.start()
        # Set before the firmware enables the ADC/GPIOB clocks: the outside world drives the
        # pin regardless of RCC gating.
        n.pin("PB1").set_voltage(1.65)
        s.advance(ms=2)
        assert n.var("error_count") == 0
        assert n.var("ready") == 1
        assert abs(n.var("last_raw") - 2048) <= 1
        assert n.pin("PB1").voltage == pytest.approx(1.65)
        # node.adc(n).set: same pin, through the [[analog]] table (PB1 = ADC12_IN16)
        n.adc(2).set(16, 0.5)
        assert n.adc(1).get(16) == pytest.approx(0.5)
        s.advance(ms=2)
        assert n.var("last_raw") == round(0.5 / 3.3 * 4095)
        before = n.var("conv_count")
        s.advance(ms=10)
        assert n.var("conv_count") - before == 10  # one conversion per 1 ms tick
        assert n.var("error_count") == 0


def test_unset_channel_warns_once(adc_exe):
    with Sim() as s:
        n = s.node("n", adc_exe())
        s.start()
        s.wait_for(lambda: n.var("conv_count") >= 10, timeout_ms=50)
        assert n.var("last_raw") == 0
        hits = [line for line in n.stderr_lines() if UNSET in line]
        assert len(hits) == 1, n.stderr_lines()
        assert "warning" in hits[0] and "PB1" in hits[0]


def test_unset_channel_strict_analog_aborts(adc_exe):
    with Sim(strict_analog=True) as s:
        n = s.node("n", adc_exe())
        s.start()
        with pytest.raises(NodeCrashed, match="channel 16 converted with no input set"):
            s.advance(ms=5)
    # with the input set, strict analog is quiet
    with Sim(strict_analog=True) as s:
        n = s.node("n", adc_exe())
        s.start()
        n.pin("PB1").set_voltage(3.3)
        s.advance(ms=3)
        assert n.var("last_raw") == 4095


def test_adc_scan_mode_aborts(adc_exe):
    with Sim() as s:
        s.node("n", adc_exe("scan"))
        s.start()
        with pytest.raises(NodeCrashed, match=r"stest: ADC1: scan mode \(SQR1\.L=1: a sequence of 2 conversions\) not modelled \(write32 0x50040030 = 0x00000001\)"):
            s.advance(ms=5)


def test_adc_async_clock_sysclk(adc_exe):
    with Sim() as s:
        n = s.node("n", adc_exe("async"))
        s.start()
        n.pin("PB1").set_voltage(1.0)
        s.advance(ms=3)
        assert n.var("error_count") == 0
        assert n.var("last_raw") == round(1.0 / 3.3 * 4095)


def test_adc_async_no_clock_aborts(adc_exe):
    with Sim() as s:
        s.node("n", adc_exe("async_noclk"))
        s.start()
        with pytest.raises(NodeCrashed, match="RCC CCIPR ADCSEL selects none"):
            s.advance(ms=5)


def test_dac_output(dac_exe):
    with Sim() as s:
        n = s.node("n", dac_exe)
        s.start()
        with pytest.raises(StestError, match="PA4 has no analog voltage"):
            _ = n.pin("PA4").voltage
        s.advance(ms=1)
        assert n.var("started") == 1 and n.var("error_count") == 0
        ch = n.dac(1).channel(1)
        assert ch.enabled and ch.dor == 2048
        assert ch.volts == pytest.approx(1.650, abs=0.001)
        assert n.pin("PA4").voltage == ch.volts
        dac_events = [json.loads(js) for _, _, js in s.event_log if json.loads(js)["kind"] == "dac"]
        assert dac_events == [{"kind": "dac", "periph": "DAC1", "channel": 1, "volts": ch.volts}]
        with pytest.raises(StestError, match="DAC1 channel 2 is disabled"):
            _ = n.dac(1).channel(2).volts
        # an external voltage wins over the DAC output; releasing it shows the DAC again
        n.pin("PA4").set_voltage(0.1)
        assert n.pin("PA4").voltage == pytest.approx(0.1)
        n.pin("PA4").release_voltage()
        assert n.pin("PA4").voltage == ch.volts


def test_analog_helper_errors(adc_exe):
    with Sim() as s:
        n = s.node("n", adc_exe())
        s.start()
        with pytest.raises(StestError, match="ADC3 channel 16 has no pin"):
            n.adc(3).set(16, 1.0)
        with pytest.raises(StestError, match="channel out of range"):
            n.adc(1).set(19, 1.0)
        assert n.adc(1).get(5) is None  # PA0: nothing applied
        with pytest.raises(StestError, match="has no DAC1|unknown model"):
            n.dac(2).state()


def test_adc_is_deterministic(adc_exe):
    def run(quantum_us: int):
        with Sim(quantum_us=quantum_us) as s:
            n = s.node("n", adc_exe())
            s.start()
            n.pin("PB1").set_voltage(2.0)
            s.advance(ms=20)
            return list(s.event_log), n.var("last_raw"), n.var("conv_count")

    a = run(1000)
    assert a == run(1000)
    assert a == run(250)


def test_adc_speed_two_virtual_seconds(adc_exe):
    """Speed budget: <= 0.4 s wall per virtual second with a conversion every 1 ms."""
    import time

    with Sim() as s:
        n = s.node("n", adc_exe())
        s.start()
        n.pin("PB1").set_voltage(1.0)
        t = time.perf_counter()
        s.advance(ms=2000)
        wall = time.perf_counter() - t
        assert n.var("conv_count") >= 1999
    assert wall <= 0.8, f"2 virtual s took {wall:.3f} s wall (budget 0.8 s)"
