"""tools/gen_chip.py: the [[analog]] pin table is validated and injected into ADC params."""

from __future__ import annotations

import importlib.util
import json
import re
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("gen_chip", _REPO / "tools" / "gen_chip.py")
gen_chip = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gen_chip)

BASE = """
[chip]
name = "X"
family = "stm32l4"
core = "cortex-m4"
nvic_prio_bits = 4
device_define = "X"

[[peripheral]]
name = "GPIOB"
model = "stm32l4.gpio"
base = 0x48000400
size = 0x400

[[peripheral]]
name = "ADC"
model = "stm32l4.adc"
base = 0x50040000
size = 0x400
params = { vref = 3.3 }
"""


def gen(tmp_path: Path, extra: str) -> str:
    p = tmp_path / "part.toml"
    p.write_text(BASE + extra)
    return gen_chip.generate(gen_chip.load(p), p.name)


def test_analog_table_injected(tmp_path):
    out = gen(tmp_path, '[[analog]]\npin = "PB1"\nadc = [1, 2]\nchannel = 16\n')
    m = re.search(r'm_ADC\("ADC", nlohmann::json::parse\(R"json\((.*?)\)json"\)', out)
    assert m, out
    assert json.loads(m.group(1)) == {"vref": 3.3, "analog": [{"pin": "PB1", "adc": [1, 2], "channel": 16}]}


@pytest.mark.parametrize(
    "entry, msg",
    [
        ('pin = "PB16"\nadc = [1]\nchannel = 1', "bad \\[\\[analog\\]\\] entry"),
        ('pin = "PB1"\nadc = []\nchannel = 1', "bad \\[\\[analog\\]\\] entry"),
        ('pin = "PB1"\nadc = [1, 1]\nchannel = 1', "bad \\[\\[analog\\]\\] entry"),
        ('pin = "PB1"\nadc = [1]\nchannel = 19', "bad \\[\\[analog\\]\\] entry"),
        ('pin = "PB1"\nadc = [1]\nchannel = 1\nextra = 2', "bad \\[\\[analog\\]\\] entry"),
        ('pin = "PC1"\nadc = [1]\nchannel = 1', "no GPIOC peripheral"),
    ],
)
def test_analog_validation(tmp_path, entry, msg):
    with pytest.raises(gen_chip.ChipError, match=msg):
        gen(tmp_path, f"[[analog]]\n{entry}\n")


def test_analog_duplicates(tmp_path):
    with pytest.raises(gen_chip.ChipError, match="pin PB1 listed twice"):
        gen(tmp_path, '[[analog]]\npin = "PB1"\nadc = [1]\nchannel = 1\n[[analog]]\npin = "PB1"\nadc = [2]\nchannel = 2\n')
    with pytest.raises(gen_chip.ChipError, match="ADC1 channel 1 listed twice"):
        gen(tmp_path, '[[analog]]\npin = "PB1"\nadc = [1]\nchannel = 1\n[[analog]]\npin = "PB0"\nadc = [1]\nchannel = 1\n')
