"""The shell commands the Install, Quickstart and Getting started pages show are commands CI runs.

Every command line in a fenced ``sh`` block of the checked pages must appear verbatim (after
collapsing whitespace) as a line of a CI workflow (``.github/workflows/ci.yml`` or
``docs.yml``, the only sources CI executes). ``cd <dir>`` must match a ``working-directory``,
a ``cd`` or a ``-w /w/<dir>`` of a workflow. A block may opt out with an HTML comment
``<!-- not tested by CI -->`` on the line(s) just before it; the page must then also say
"not tested by CI" in visible text.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PAGES = ["docs/install.md", "docs/quickstart.md", "docs/guide/getting-started.md"]
WORKFLOWS = [".github/workflows/ci.yml", ".github/workflows/docs.yml"]
OPT_OUT = "<!-- not tested by CI -->"

_FENCE = re.compile(r"^(?P<indent>[ \t]*)(?P<fence>`{3,}|~{3,})(?P<info>[^\n]*)$")


def _norm(s: str) -> str:
    return " ".join(s.split())


def sh_blocks(text: str) -> list[tuple[int, list[str], bool]]:
    """(first line number, command lines, opted out) for each fenced sh/bash/shell block."""
    lines = text.splitlines()
    out: list[tuple[int, list[str], bool]] = []
    i = 0
    while i < len(lines):
        m = _FENCE.match(lines[i])
        if not m:
            i += 1
            continue
        fence, info = m["fence"], m["info"].strip().split()
        lang = info[0] if info else ""
        start = i
        body: list[str] = []
        i += 1
        while i < len(lines) and not lines[i].strip().startswith(fence):
            body.append(lines[i])
            i += 1
        i += 1
        if lang not in ("sh", "bash", "shell", "console"):
            continue
        # the nearest non-blank line before the fence
        j = start - 1
        while j >= 0 and not lines[j].strip():
            j -= 1
        opted_out = j >= 0 and lines[j].strip() == OPT_OUT
        cmds = []
        for raw in body:
            s = raw.strip()
            if not s or s.startswith("#"):
                continue
            s = re.sub(r"\s+#\s.*$", "", s)  # trailing comment
            if s.startswith("$ "):
                s = s[2:]
            cmds.append(_norm(s))
        out.append((start + 1, cmds, opted_out))
    return out


def ci_lines() -> set[str]:
    lines: set[str] = set()
    for wf in WORKFLOWS:
        for raw in (REPO / wf).read_text().splitlines():
            s = raw.strip()
            if s.startswith("run: "):
                s = s[len("run: "):]
            lines.add(_norm(s))
    return lines


def ci_dirs() -> set[str]:
    dirs: set[str] = set()
    for wf in WORKFLOWS:
        text = (REPO / wf).read_text()
        dirs.update(re.findall(r"working-directory:\s*(\S+)", text))
        dirs.update(re.findall(r"-w /w/(\S+)", text))
        dirs.update(re.findall(r"(?:^|[;&|]\s*|\s)cd (\S+)", text, re.M))
    return {d.strip("'\"").rstrip("/") for d in dirs}


def unmatched(page: str) -> list[str]:
    text = (REPO / page).read_text()
    lines, dirs = ci_lines(), ci_dirs()
    bad = []
    for lineno, cmds, opted_out in sh_blocks(text):
        if opted_out:
            continue
        for c in cmds:
            if c.startswith("cd "):
                if c[3:].strip().rstrip("/") not in dirs:
                    bad.append(f"{page}:{lineno}: {c}  (no matching working-directory/cd in CI)")
            elif c not in lines:
                bad.append(f"{page}:{lineno}: {c}")
    return bad


def test_extractor_finds_blocks_and_opt_outs():
    sample = (
        "x\n\n```sh\n# comment\ncmake --build build   # trailing\n\n```\n\n"
        f"{OPT_OUT}\n```sh\ngit submodule add x\n```\n"
        "```python\nprint(1)\n```\n"
    )
    blocks = sh_blocks(sample)
    assert [(c, o) for _, c, o in blocks] == [(["cmake --build build"], False), (["git submodule add x"], True)]


def test_getting_started_commands_match_ci():
    bad = [b for page in PAGES for b in unmatched(page)]
    assert not bad, "commands not run by CI:\n" + "\n".join(bad)


def test_pages_have_commands():
    # a stub page would pass the parity test vacuously
    for page in PAGES:
        blocks = sh_blocks((REPO / page).read_text())
        assert any(cmds for _, cmds, opted_out in blocks if not opted_out), f"{page} has no CI-checked sh block"


def test_opt_outs_are_visible():
    for page in PAGES:
        text = (REPO / page).read_text()
        if OPT_OUT in text:
            visible = text.replace(OPT_OUT, "")
            assert "not tested by CI" in visible, f"{page} opts blocks out of CI parity without saying so"
