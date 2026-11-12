"""The reference pages cover what the code offers, so they cannot silently fall behind it.

Each list is derived from the source, not typed here, except the abort-message table, which
pins each message twice: its text must still be in the runtime source *and* on the page.

- Firmware reference (``docs/reference/firmware.md``): every ``STEST_*`` environment variable
  the runtime, the chip models, the Python package or the CMake toolchain reads; every
  ``stest_add_firmware`` keyword; every user-settable ``STEST_*`` CMake cache variable; the
  ``stest: ...`` messages (strict aborts, warnings, usage errors).
- Support matrix (``docs/reference/support.md``): every model kind in a part TOML and every
  core-generic model header, each linked by its header path.
- Every ``https://github.com/Nexite/stest/blob/main/<path>`` link on these pages names a file
  that exists.
- Shell blocks on the pages written for the cookbook and references follow the Getting
  started rule (``test_docs_commands.py``): a command CI runs, or a visible
  "not tested by CI" opt-out.
"""

from __future__ import annotations

import re
import tomllib
from pathlib import Path

import pytest

from test_docs_commands import OPT_OUT, unmatched

REPO = Path(__file__).resolve().parents[2]
FIRMWARE = REPO / "docs/reference/firmware.md"
SUPPORT = REPO / "docs/reference/support.md"
COOKBOOK = sorted((REPO / "docs/guide/writing-tests").glob("*.md"))
SH_PAGES = [
    "docs/reference/firmware.md",
    "docs/reference/support.md",
    "docs/troubleshooting.md",
    *(str(p.relative_to(REPO)) for p in COOKBOOK),
]


def _token(name: str) -> re.Pattern:
    """``name`` as a whole identifier (``SOURCES`` must not match inside ``HAL_SOURCES``)."""
    return re.compile(rf"(?<![A-Za-z0-9_]){re.escape(name)}(?![A-Za-z0-9_])")


def _sources(*globs: str) -> list[Path]:
    out = []
    for g in globs:
        out += [p for p in REPO.glob(g) if "/tests/" not in str(p.relative_to(REPO)) and p.is_file()]
    return sorted(out)


# ---- environment variables ------------------------------------------------------------------

# Read by stest's own test suite only (python/tests); documented for contributors.
TEST_SUITE_ENV = {"STEST_SELFTEST_BIN", "STEST_PERF_STRICT"}


def env_vars() -> set[str]:
    names: set[str] = set()
    for p in _sources("runtime/**/*.cpp", "runtime/**/*.hpp", "runtime/**/*.h", "chips/**/*.cpp",
                      "chips/**/*.hpp", "chips/**/*.h", "chips/**/*.c"):
        names |= set(re.findall(r'getenv\("(STEST_[A-Z0-9_]+)"\)', p.read_text()))
    for p in _sources("python/stest/**/*.py"):
        if p.name == "protocol.py":  # STEST_MAX_PAYLOAD etc. mirror C macros, not variables
            continue
        names |= set(re.findall(r'["\'](STEST_[A-Z0-9_]+)["\']', p.read_text()))
    for p in _sources("cmake/*.cmake", "CMakeLists.txt"):
        names |= set(re.findall(r"\$ENV\{(STEST_[A-Z0-9_]+)\}", p.read_text()))
    return names


def test_env_var_extraction_finds_the_known_variables():
    found = env_vars()
    assert {"STEST_POLL_SKIP", "STEST_STRICT_ANALOG", "STEST_DEBUGGER", "STEST_CTRL",
            "STEST_PARENT_PID", "STEST_ACCESS_COST_NS", "STEST_LLVM_ROOT"} <= found, found
    tests = "".join(p.read_text() for p in (REPO / "python/tests").glob("*.py"))
    for name in TEST_SUITE_ENV:  # still read by the test suite (else drop it from the page)
        assert f'os.environ.get("{name}")' in tests, name


def test_every_env_var_documented():
    text = FIRMWARE.read_text()
    missing = sorted(n for n in env_vars() | TEST_SUITE_ENV if not _token(n).search(text))
    assert not missing, f"environment variables missing from {FIRMWARE.relative_to(REPO)}: {missing}"


# ---- stest_add_firmware and CMake cache variables --------------------------------------------


def firmware_keywords() -> list[str]:
    text = (REPO / "cmake/stest_add_firmware.cmake").read_text()
    m = re.search(r'cmake_parse_arguments\(PARSE_ARGV \d+ \w+ "([^"]*)" "([^"]*)" "([^"]*)"\)', text)
    assert m, "cmake_parse_arguments call not found in stest_add_firmware.cmake"
    return [k for group in m.groups() for k in group.split(";") if k]


def test_every_stest_add_firmware_option_documented():
    kws = firmware_keywords()
    assert {"CHIP", "SOURCES", "INCLUDE_DIRS", "DEFINES", "HAL_SOURCES", "MODELS"} <= set(kws), kws
    text = FIRMWARE.read_text()
    missing = [k for k in kws if not re.search(rf"`{k}(?![A-Za-z0-9_])", text)]
    assert not missing, f"stest_add_firmware keywords missing from the firmware reference: {missing}"


def cache_vars() -> set[str]:
    names: set[str] = set()
    for p in _sources("cmake/*.cmake", "CMakeLists.txt"):
        t = p.read_text()
        names |= set(re.findall(r"option\((STEST_[A-Z0-9_]+)", t))
        names |= set(re.findall(r'set\((STEST_[A-Z0-9_]+) "[^"]*" CACHE (?:PATH|BOOL|STRING|FILEPATH)', t))
    return names


def test_every_cmake_cache_variable_documented():
    names = cache_vars()
    assert {"STEST_BUILD_TESTS", "STEST_LLVM_ROOT", "STEST_STM32L4_HAL_DIR",
            "STEST_STM32L4_CMSIS_DEVICE_DIR", "STEST_CMSIS_CORE_DIR"} <= names, names
    text = FIRMWARE.read_text()
    missing = sorted(n for n in names if not _token(n).search(text))
    assert not missing, f"CMake cache variables missing from the firmware reference: {missing}"


# ---- support matrix --------------------------------------------------------------------------


def model_kinds() -> set[tuple[str, str]]:
    kinds = set()
    for toml in REPO.glob("chips/*/parts/*.toml"):
        for p in tomllib.loads(toml.read_text()).get("peripheral", []):
            family, kind = p["model"].split(".")
            kinds.add((family, kind))
    return kinds


def test_every_modelled_peripheral_in_support_matrix():
    kinds = model_kinds()
    assert len(kinds) >= 11, kinds  # rcc pwr syscfg exti flash gpio tim adc dac spi bxcan
    text = SUPPORT.read_text()
    missing = sorted(f"{f}.{k}" for f, k in kinds if f"chips/{f}/models/{k}.hpp" not in text)
    assert not missing, f"models missing (linked by header) from the support matrix: {missing}"


def test_core_models_in_support_matrix():
    text = SUPPORT.read_text()
    headers = [p.relative_to(REPO).as_posix() for p in (REPO / "chips/cortexm").glob("*.hpp")]
    headers.append("runtime/include/stest/cortexm.hpp")  # NVIC, SCB, SysTick
    assert len(headers) >= 4
    missing = [h for h in headers if h not in text]
    assert not missing, f"core models missing from the support matrix: {missing}"


@pytest.mark.parametrize(
    "page", ["docs/reference/firmware.md", "docs/reference/support.md", "docs/troubleshooting.md",
             *(str(p.relative_to(REPO)) for p in COOKBOOK)]
)
def test_github_links_point_at_existing_files(page):
    text = (REPO / page).read_text()
    bad = [path for path in re.findall(r"https://github\.com/Nexite/stest/(?:blob|tree)/main/([^)#\s]+)", text)
           if not (REPO / path).exists()]
    assert not bad, f"{page}: links to files that do not exist: {bad}"


# ---- stest: messages -------------------------------------------------------------------------

# (what, source file, text that must still be in that file, regex the page must match).
# The page regexes match concrete examples of the message, as a user sees it.
HEX = r"0x[0-9a-f]{8}"
ACCESS = rf"(?:read|write)\d+ {HEX}"
MESSAGES = [
    ("unmodelled address", "runtime/src/mmio.cpp", '"unmodelled %s%u 0x%08x"',
     rf"(?m)stest: unmodelled {ACCESS}$"),
    ("unimplemented register", "runtime/src/mmio.cpp", '"unmodelled %s%u 0x%08x (%s)"',
     rf"stest: unmodelled {ACCESS} \([A-Z][A-Z0-9_]*\)"),
    ("write to a read-only register", "runtime/src/mmio.cpp", '"write to read-only register 0x%08x (write%u, %s)"',
     rf"stest: write to read-only register {HEX} \(write\d+, [A-Z][A-Z0-9_]*\)"),
    ("clock disabled", "runtime/src/bus.cpp", '" while its clock is disabled ("',
     rf"stest: access to [A-Z][A-Z0-9_]* while its clock is disabled \(RCC [A-Z0-9]+ bit \d+\) \({ACCESS}\)"),
    ("feature not modelled", "runtime/include/stest/peripheral.hpp", '": " + f + " not modelled"',
     rf"stest: [A-Z][A-Z0-9_]*: [^\n]+ not modelled \({ACCESS}(?: = {HEX})?\)"),
    ("model exception", "runtime/src/mmio.cpp", '"exception in model at 0x%08x: %s"',
     rf"stest: exception in model at {HEX}: "),
    ("clock event exception", "runtime/src/mmio.cpp", '"exception in clock event: %s"',
     r"stest: exception in clock event: "),
    ("firmware main returned", "runtime/src/control_channel.cpp", '"firmware main returned %d at t=%" PRIu64 " ns"',
     r"stest: firmware main returned -?\d+ at t=\d+ ns"),
    ("inbox message in the past", "runtime/src/control_channel.cpp", '" ns is in the past (now %" PRIu64 " ns; op %s)"',
     r"stest: inbox message for [A-Z][A-Z0-9_]* at t=\d+ ns is in the past \(now \d+ ns; op \w+\)"),
    ("inbox message, unknown model", "runtime/src/control_channel.cpp", '" ns (op %s): unknown model"',
     r"stest: inbox message for \w+ at t=\d+ ns \(op \w+\): unknown model"),
    ("inbox message, model refused", "runtime/src/control_channel.cpp", '" ns (op %s): %s"',
     r"stest: inbox message for [A-Z][A-Z0-9_]* at t=\d+ ns \(op \w+\): unknown op: \w+"),
    ("breakpoint", "chips/cmsis_host/cmsis_compiler.h", '"__BKPT(%d) reached"',
     r"stest: __BKPT\(\d+\) reached"),
    ("WFI with nothing to wake it", "runtime/src/cortexm.cpp", '": nothing can wake the core (no pending exception, "',
     r"nothing can wake the core \(no pending exception, no clock event, no quantum end\)"),
    ("SysTick without a clock", "runtime/src/cortexm.cpp", '"SysTick clock is 0 Hz (SystemCoreClock not set?)"',
     r"SysTick clock is 0 Hz \(SystemCoreClock not set\?\)"),
    ("hal-mode direct access", "runtime/src/bus.cpp",
     '"stest: warning: firmware accessed %s %s directly while %s is in hal mode\\n"',
     r"stest: warning: firmware accessed GPIO[A-I] [A-Z]+ directly while GPIO is in hal mode"),
    ("unset analog input", "chips/stm32l4/models/adc.cpp", '"stest: warning: %s%s; reading 0 V\\n"',
     r"stest: warning: ADC\d channel \d+ converted with no input set \(pin P[A-I]\d+\); reading 0 V"),
    ("locked ITM", "chips/cortexm/itm.cpp", '"stest: warning: ITM %s write ignored: the ITM is locked (write 0xC5ACCE55 to "',
     r"stest: warning: ITM (?:TER|TPR|TCR) write ignored: the ITM is locked \(write 0xC5ACCE55 to ITM->LAR first, as a debugger does\)"),
    ("unterminated ITM line", "runtime/src/control_channel.cpp", '"stest: ITM unterminated %s\\n"',
     r'stest: ITM unterminated \{"port":\d+,"text":'),
    ("unterminated stdout line", "runtime/src/control_channel.cpp", '"stest: stdout unterminated %s\\n"',
     r'stest: stdout unterminated \{"text":'),
    ("bad STEST_POLL_SKIP", "runtime/src/node_main.cpp", '"stest: bad STEST_POLL_SKIP \'%s\' (expected an integer >= 0)\\n"',
     r"stest: bad STEST_POLL_SKIP '[^']*' \(expected an integer >= 0\)"),
    ("standalone usage", "runtime/src/node_main.cpp", '"stest: %s\\nusage: %s --run-ms N [--quantum-us Q]\\n"',
     r"usage: \S+ --run-ms N \[--quantum-us Q\]"),
    ("CAN controller not wired", "python/stest/sim.py", 'requested a transmission but is not wired to a bus: ',
     r"node '\w+' CAN\d requested a transmission but is not wired to a bus: Sim\.node\('\w+', \.\.\., can=\{'CAN\d': bus\}\)"),
    ("CAN bit rate mismatch", "python/stest/sim.py", 'match bus {bus.name!r} ({bus.bitrate}) (check the CAN bit timing: Prescaler, BS1, BS2, PCLK1)',
     r"node '\w+' CAN\d entered normal mode at virtual t=[\d.]+ ms: bit rate \d+ does not match bus '\w+' \(\d+\)"),
]


@pytest.mark.parametrize("what,src,literal,page_re", MESSAGES, ids=[m[0] for m in MESSAGES])
def test_every_abort_message_documented(what, src, literal, page_re):
    source = (REPO / src).read_text()
    assert literal in source, (
        f"{what}: {literal} is no longer in {src}; update the firmware reference and this table"
    )
    assert re.search(page_re, FIRMWARE.read_text()), (
        f"{what}: no example matching {page_re!r} in {FIRMWARE.relative_to(REPO)}"
    )


# ---- shell blocks on the new pages ------------------------------------------------------------


@pytest.mark.parametrize("page", SH_PAGES)
def test_sh_blocks_are_ci_commands_or_labelled(page):
    assert not unmatched(page), "commands not run by CI:\n" + "\n".join(unmatched(page))
    text = (REPO / page).read_text()
    if OPT_OUT in text:
        assert "not tested by CI" in text.replace(OPT_OUT, ""), f"{page} opts blocks out without saying so"
