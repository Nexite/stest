#!/usr/bin/env python3
"""Generate the C++ chip glue for one stest chip part (TOML).

    gen_chip.py <part.toml> <out.cpp> [--hal TYPE[,TYPE...]]   write the generated C++ file
    gen_chip.py --info <part.toml>        print "name;family;device_define" (for CMake)
    gen_chip.py --types <part.toml>       print the peripheral types, ";"-separated (for CMake)

The generated file defines, for the part:
  * a weak default for every core exception and external IRQ handler, e.g.
        __attribute__((weak)) void TIM7_IRQHandler(void){ stest_rt_abort("unhandled IRQ TIM7 (55)"); }
    It must be linked as an object (never from an archive) so that the firmware's
    strong definitions win on both ELF and Mach-O.
  * the IRQ table (index = IRQn + 16) referring to those symbols,
  * stest_chip_init(ModelContext&): model instances (constructed and reset), ctx.add,
    set_clock_tree([clock_tree] peripheral), connect() on each, then Bus::map + set_gate
    (peripherals with rcc_enable) + register_model, core clock getter (CMSIS
    SystemCoreClock) and the SysTick external-reference divider,
  * `extern const stest::ChipDesc stest_chip` consumed by node_main.cpp.

Peripheral types: a model "<family>.<kind>" has type KIND upper-cased (stm32l4.gpio -> GPIO);
they are what stest_add_firmware's `MODELS <TYPE>=register|hal` names. `--hal GPIO` marks
every peripheral of that type hal-owned in the generated init (Bus::set_hal_owned: firmware
MMIO to it warns once per register). This is how a node learns its hal-mode types: they are
baked into the per-firmware generated glue by stest_add_firmware (which validates TYPE
against `--types` and the family's family.cmake).

Model "<family>.<kind>" maps to class stest::<family>::<Kind> declared in
chips/<family>/models/<kind>.hpp, constructed as (name, params-json, ModelContext&).

Clock gating: [clock_tree] names the clock-tree model (`peripheral`, implements
stest::ClockTree) and the RCC enable registers by name (`enable_regs = { AHB2ENR = 0x4C,
... }`, offsets in the RCC block). A [[peripheral]] with
`rcc_enable = { reg = "AHB2ENR", bit = 0 }` is gated on that bit: any access while it is
clear aborts "access to <NAME> while its clock is disabled (RCC AHB2ENR bit 0) (write32 0x48000414)".

Analog pins: top-level `[[analog]]` entries (`pin = "PB1", adc = [1, 2], channel = 16`)
map package pins to ADC channels. They are validated (pin "P<port><0-15>" with a
GPIO<port> peripheral, ADC numbers 1..9 without repeats, channel 1..18, each (adc, channel)
and each pin listed once) and passed to every `<family>.adc` model as `params.analog`.

Python >= 3.11, standard library only (tomllib).
"""
from __future__ import annotations

import json
import re
import sys
import tomllib
from pathlib import Path

IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
PIN = re.compile(r"^P([A-Z])(\d{1,2})$")

# Core exceptions: (exception number, CMSIS handler name, IRQn).
CORE_EXCEPTIONS = [
    (2, "NMI_Handler", -14),
    (3, "HardFault_Handler", -13),
    (4, "MemManage_Handler", -12),
    (5, "BusFault_Handler", -11),
    (6, "UsageFault_Handler", -10),
    (11, "SVC_Handler", -5),
    (12, "DebugMon_Handler", -4),
    (14, "PendSV_Handler", -2),
    (15, "SysTick_Handler", -1),
]


class ChipError(Exception):
    pass


def load(path: Path) -> dict:
    with open(path, "rb") as f:
        data = tomllib.load(f)
    chip = data.get("chip")
    if not isinstance(chip, dict):
        raise ChipError(f"{path}: missing [chip] table")
    for key in ("name", "family", "core", "nvic_prio_bits", "device_define"):
        if key not in chip:
            raise ChipError(f"{path}: [chip] missing '{key}'")
    if chip["core"] != "cortex-m4":
        raise ChipError(f"{path}: unsupported core '{chip['core']}'")
    if not IDENT.match(chip["family"]):
        raise ChipError(f"{path}: bad family '{chip['family']}'")
    periphs = data.get("peripheral", [])
    names = set()
    for p in periphs:
        for key in ("name", "model", "base", "size"):
            if key not in p:
                raise ChipError(f"{path}: peripheral missing '{key}': {p}")
        if not IDENT.match(p["name"]) or p["name"] in names:
            raise ChipError(f"{path}: bad or duplicate peripheral name '{p['name']}'")
        names.add(p["name"])
        fam, _, kind = p["model"].partition(".")
        if not IDENT.match(fam) or not IDENT.match(kind):
            raise ChipError(f"{path}: bad model '{p['model']}' (want <family>.<kind>)")
    tree = data.get("clock_tree")
    regs = {}
    if tree is not None:
        if not isinstance(tree, dict) or tree.get("peripheral") not in names:
            raise ChipError(f"{path}: [clock_tree] peripheral must name a [[peripheral]]")
        regs = tree.get("enable_regs", {})
        if not isinstance(regs, dict) or not all(
            IDENT.match(k) and isinstance(v, int) and 0 <= v < 0x400 and v % 4 == 0 for k, v in regs.items()
        ):
            raise ChipError(f"{path}: [clock_tree] enable_regs must map NAME = word offset")
    for p in periphs:
        gate = p.get("rcc_enable")
        if gate is None:
            continue
        if tree is None:
            raise ChipError(f"{path}: {p['name']}: rcc_enable needs a [clock_tree] table")
        if p["name"] == tree["peripheral"]:
            raise ChipError(f"{path}: {p['name']}: the clock-tree peripheral cannot be gated")
        if (
            not isinstance(gate, dict)
            or set(gate) != {"reg", "bit"}
            or gate["reg"] not in regs
            or not isinstance(gate["bit"], int)
            or not 0 <= gate["bit"] < 32
        ):
            raise ChipError(
                f"{path}: {p['name']}: bad rcc_enable {gate!r} "
                f"(want {{ reg = <one of {sorted(regs)}>, bit = 0..31 }})"
            )
    analog = data.get("analog", [])
    if not isinstance(analog, list):
        raise ChipError(f"{path}: analog must be an array of tables ([[analog]])")
    pins, chans = set(), set()
    for a in analog:
        m = PIN.match(a.get("pin", "")) if isinstance(a, dict) else None
        adcs = a.get("adc") if isinstance(a, dict) else None
        ch = a.get("channel") if isinstance(a, dict) else None
        if (
            not m
            or set(a) != {"pin", "adc", "channel"}
            or not 0 <= int(m.group(2)) <= 15
            or not isinstance(adcs, list)
            or not adcs
            or len(set(adcs)) != len(adcs)
            or not all(isinstance(n, int) and not isinstance(n, bool) and 1 <= n <= 9 for n in adcs)
            or not isinstance(ch, int)
            or isinstance(ch, bool)
            or not 1 <= ch <= 18
        ):
            raise ChipError(
                f"{path}: bad [[analog]] entry {a!r} (want pin = \"PB1\", adc = [1, 2], channel = 1..18)"
            )
        if f"GPIO{m.group(1)}" not in names:
            raise ChipError(f"{path}: [[analog]] pin {a['pin']}: no GPIO{m.group(1)} peripheral")
        if a["pin"] in pins:
            raise ChipError(f"{path}: [[analog]] pin {a['pin']} listed twice")
        pins.add(a["pin"])
        for n in adcs:
            if (n, ch) in chans:
                raise ChipError(f"{path}: [[analog]] ADC{n} channel {ch} listed twice")
            chans.add((n, ch))
    irqs = data.get("irq", {})
    seen = {}
    for name, num in irqs.items():
        if not IDENT.match(name) or not isinstance(num, int) or not 0 <= num < 240:
            raise ChipError(f"{path}: bad irq entry {name} = {num!r}")
        if num in seen:
            raise ChipError(f"{path}: IRQ {num} used by {seen[num]} and {name}")
        seen[num] = name
    return data


def model_class(model: str) -> tuple[str, str]:
    fam, _, kind = model.partition(".")
    cls = "".join(part.capitalize() for part in kind.split("_"))
    return f"{fam}/models/{kind}.hpp", f"stest::{fam}::{cls}"


def periph_type(p: dict) -> str:
    return p["model"].partition(".")[2].upper()


def types(data: dict) -> list[str]:
    return sorted({periph_type(p) for p in data.get("peripheral", [])})


def generate(data: dict, source_name: str, hal: tuple[str, ...] = ()) -> str:
    chip = data["chip"]
    periphs = data.get("peripheral", [])
    for t in hal:
        if t not in types(data):
            raise ChipError(f"--hal {t}: no peripheral of type {t} (types: {' '.join(types(data))})")
    irqs = sorted(data.get("irq", {}).items(), key=lambda kv: kv[1])
    n_ext = (max(n for _, n in irqs) + 1) if irqs else 0
    n_table = 16 + n_ext
    system_init = chip.get("system_init")
    divider = chip.get("systick_external_divider")

    out: list[str] = []
    w = out.append
    w(f"// Generated by tools/gen_chip.py from {source_name}. Do not edit.")
    w(f"// Chip {chip['name']} ({chip['family']}, {chip['core']}).")
    w("#include <cstddef>")
    w("#include <cstdint>")
    w("#include <nlohmann/json.hpp>")
    w('#include "stest/cortexm.hpp"')
    w('#include "stest/model_context.hpp"')
    w('#include "stest/mmio.h"')
    w('#include "stest/node.hpp"')
    for header in sorted({model_class(p["model"])[0] for p in periphs}):
        w(f'#include "{header}"')
    w("")
    w('extern "C" {')
    w("extern uint32_t SystemCoreClock;  // CMSIS system_<family>.c")
    if system_init:
        w(f"void {system_init}(void);")
    w("")
    w("// Weak defaults: firmware definitions (strong) replace these at link time.")
    for exc, handler, irqn in CORE_EXCEPTIONS:
        short = handler[: -len("_Handler")]
        w(f'__attribute__((weak)) void {handler}(void){{ stest_rt_abort("unhandled exception {short} ({irqn})"); }}')
    for name, num in irqs:
        w(f'__attribute__((weak)) void {name}_IRQHandler(void){{ stest_rt_abort("unhandled IRQ {name} ({num})"); }}')
    w('}  // extern "C"')
    w("")
    w("namespace {")
    w(f"const stest::IrqHandler kIrqTable[{n_table}] = {{")
    table = ["nullptr"] * n_table
    for exc, handler, _ in CORE_EXCEPTIONS:
        table[exc] = handler
    for name, num in irqs:
        table[16 + num] = f"{name}_IRQHandler"
    for i, entry in enumerate(table):
        w(f"    /* {i - 16:4d} */ {entry},")
    w("};")
    w("uint32_t core_clock() { return SystemCoreClock; }")
    if system_init:
        w(f"void chip_startup() {{ {system_init}(); }}")
    w("}  // namespace")
    w("")
    tree = data.get("clock_tree")
    w("void stest_chip_init(stest::ModelContext& ctx) {")
    w("  stest::Bus& bus = ctx.bus();")
    w("  stest::Cortexm& core = ctx.core();")
    for p in periphs:
        _, cls = model_class(p["model"])
        params = dict(p.get("params", {}))
        if p["model"].partition(".")[2] == "adc":
            params["analog"] = data.get("analog", [])
        params = json.dumps(params, sort_keys=True, separators=(",", ":"))
        var = f"m_{p['name']}"
        w(f'  static {cls} {var}("{p["name"]}", nlohmann::json::parse(R"json({params})json"), ctx);')
        w(f"  {var}.reset();")
    for p in periphs:
        w(f"  ctx.add(&m_{p['name']});")
    if tree:
        w(f"  ctx.set_clock_tree(&m_{tree['peripheral']});")
    for p in periphs:
        w(f"  m_{p['name']}.connect(ctx);")
    for p in periphs:
        var = f"m_{p['name']}"
        w(f"  bus.map(0x{p['base']:08X}u, 0x{p['size']:X}u, &{var});")
        gate = p.get("rcc_enable")
        if gate:
            off = tree["enable_regs"][gate["reg"]]
            w(
                f"  bus.set_gate(&{var}, ctx.clocks().enable_word(0x{off:02X}u), 1u << {gate['bit']}, "
                f'"{tree["peripheral"]} {gate["reg"]} bit {gate["bit"]}");'
            )
        if periph_type(p) in hal:
            w(f'  bus.set_hal_owned(&{var}, "{periph_type(p)}");')
        w(f"  stest::register_model(&{var});")
    w("  core.set_core_clock_getter(&core_clock);")
    if divider:
        w(f"  core.set_systick_external_divider({int(divider)});")
    w("}")
    w("")
    startup = "&chip_startup" if system_init else "nullptr"
    w(
        f'extern const stest::ChipDesc stest_chip = {{"{chip["name"]}", {int(chip["nvic_prio_bits"])}u, '
        f"kIrqTable, {n_table}, &stest_chip_init, {startup}}};"
    )
    return "\n".join(out) + "\n"


def main(argv: list[str]) -> int:
    try:
        if len(argv) == 3 and argv[1] == "--info":
            chip = load(Path(argv[2]))["chip"]
            print(f"{chip['name']};{chip['family']};{chip['device_define']}")
            return 0
        if len(argv) == 3 and argv[1] == "--types":
            print(";".join(types(load(Path(argv[2])))))
            return 0
        hal: tuple[str, ...] = ()
        if len(argv) == 5 and argv[3] == "--hal":
            hal = tuple(t for t in argv[4].split(",") if t)
            argv = argv[:3]
        if len(argv) != 3 or argv[1].startswith("--"):
            print(__doc__, file=sys.stderr)
            return 2
        src, dst = Path(argv[1]), Path(argv[2])
        text = generate(load(src), src.name, hal)
        if not dst.exists() or dst.read_text() != text:
            dst.write_text(text)
        return 0
    except (ChipError, tomllib.TOMLDecodeError, OSError) as e:
        print(f"gen_chip: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
