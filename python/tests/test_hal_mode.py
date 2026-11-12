"""HAL mode (`stest_add_firmware(... MODELS GPIO=hal)`): ST's stm32l4xx_hal_gpio.c is replaced
by stest's shim, which drives the GPIO model directly. Tests see the same pins, registers,
interrupts and event times as in register mode."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

from stest import Sim

_REPO = Path(__file__).resolve().parents[2]
_GEN = _REPO / "tools" / "gen_chip.py"
_TOML = _REPO / "chips" / "stm32l4" / "parts" / "stm32l496rg.toml"

WARNING = "stest: warning: firmware accessed GPIOA BSRR directly while GPIO is in hal mode"


def _blinky_log(exe) -> list[tuple[int, str, str]]:
    with Sim() as s:
        n = s.node("n", exe)
        s.start()
        s.advance(ms=2100)
        assert [lvl for _, lvl in n.pin("PA5").history] == [1, 0, 1, 0]
        assert not [ln for ln in n.stderr_lines() if "warning" in ln], n.stderr_lines()
        return list(s.event_log)


def test_blinky_hal_mode_matches_register_mode(blinky_exe, blinky_gpio_hal_exe):
    reg = _blinky_log(blinky_exe)
    hal = _blinky_log(blinky_gpio_hal_exe)
    assert len(reg) == 4
    assert hal == reg  # identical times (to the ns), nodes and payloads


def _exti_run(exe) -> tuple[list, int]:
    with Sim() as s:
        n = s.node("n", exe)
        s.start()
        s.advance(ms=2)
        btn = n.pin("PC13")
        for level in (1, 0, 1, 0, 1):
            btn.set_high() if level else btn.set_low()
            s.advance(ms=1)
        assert not [ln for ln in n.stderr_lines() if "warning" in ln], n.stderr_lines()
        return list(s.event_log), n.var("exti_count")


def test_exti_hal_mode(exti_exe, exti_gpio_hal_exe):
    # The shim configures PC13's EXTI line (SYSCFG EXTICR4, EXTI RTSR1/IMR1) through the
    # register models and its IRQ handler clears EXTI PR1: one callback per rising edge.
    reg_log, reg_count = _exti_run(exti_exe)
    hal_log, hal_count = _exti_run(exti_gpio_hal_exe)
    assert hal_count == reg_count == 3
    assert len(reg_log) == 5 + 3  # 5 PC13 drives + 3 LED toggles
    assert hal_log == reg_log  # same LED toggles at the same virtual times


def test_hal_mode_direct_access_warns_once(hal_direct_gpio_hal_exe):
    with Sim() as s:
        n = s.node("n", hal_direct_gpio_hal_exe)
        s.start()
        s.advance(ms=55)
        led = n.pin("PA5")
        # The direct BSRR writes still drove the pin (set, then reset), then the HAL toggles.
        levels = [lvl for _, lvl in led.history]
        assert levels[:2] == [1, 0] and len(levels) >= 7
        assert n.var("toggle_count") >= 5  # the run continued
        s.wait_for(lambda: any("warning" in ln for ln in n.stderr_lines()), timeout_ms=10)
        hits = [ln for ln in n.stderr_lines() if re.search(r"warning: firmware accessed", ln)]
        assert hits == [WARNING], n.stderr_lines()


def test_gen_chip_hal_marks_type_hal_owned(tmp_path):
    types = subprocess.run([sys.executable, _GEN, "--types", _TOML], capture_output=True, text=True, check=True)
    assert types.stdout.strip().split(";") == [
        "ADC", "DAC", "EXTI", "FLASH", "GPIO", "PWR", "RCC", "SPI", "SYSCFG", "TIM",
    ]
    out = tmp_path / "chip.cpp"
    subprocess.run([sys.executable, _GEN, _TOML, out, "--hal", "GPIO"], check=True)
    owned = re.findall(r'bus\.set_hal_owned\(&m_(\w+), "GPIO"\);', out.read_text())
    assert owned == [f"GPIO{p}" for p in "ABCDEFGHI"]
    subprocess.run([sys.executable, _GEN, _TOML, out], check=True)
    assert "set_hal_owned" not in out.read_text()
    bad = subprocess.run([sys.executable, _GEN, _TOML, out, "--hal", "FOO"], capture_output=True, text=True)
    assert bad.returncode == 1 and "no peripheral of type FOO" in bad.stderr
