"""`import stest` stays light: cantools / python-can (LGPL-3.0) are the optional `dbc` extra,
imported only when a Dbc is loaded."""

import subprocess
import sys
from pathlib import Path

DBC = Path(__file__).parent / "data" / "test.dbc"


def run_py(code: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, timeout=60)


def test_import_stest_does_not_import_cantools_or_python_can():
    r = run_py(
        "import sys, stest\n"
        "from stest import Dbc, DbcDevice, Sim\n"
        "bad = [m for m in ('cantools', 'can') if m in sys.modules]\n"
        "assert not bad, bad\n"
    )
    assert r.returncode == 0, r.stderr


def test_dbc_loads_cantools_on_first_use():
    r = run_py(
        "import sys\n"
        "from stest import Dbc\n"
        f"Dbc({str(DBC)!r})\n"
        "assert 'cantools' in sys.modules\n"
    )
    assert r.returncode == 0, r.stderr


def test_missing_cantools_names_the_extra():
    r = run_py(
        "import sys\n"
        "sys.modules['cantools'] = None  # as if not installed\n"
        "from stest import Dbc, DbcDevice, StestError\n"
        "try:\n"
        f"    Dbc({str(DBC)!r})\n"
        "except StestError as e:\n"
        "    print(e)\n"
        "else:\n"
        "    raise SystemExit('no StestError')\n"
    )
    assert r.returncode == 0, r.stderr
    assert "stest[dbc]" in r.stdout
    assert "cantools" in r.stdout
