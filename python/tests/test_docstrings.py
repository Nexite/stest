"""The public Python API is documented: the site's API reference (mkdocstrings, Google style)
is generated from these docstrings, so a missing one is a hole in the published docs.

Public API: ``stest.__all__`` and the callables in ``stest.can.__all__`` (constants such as
``MIN_FRAME_BITS`` cannot carry a docstring; the reference page describes them).

Rules (every violation is listed in one failure message):

* every public class and function has its own docstring (a dataclass's generated
  ``CanFrame(id: int, ...)`` signature does not count);
* every public method and property defined on a public class (in its own ``__dict__``, so
  inherited members are checked on the class that defines them) has a docstring;
* a public function or method taking parameters other than ``self`` has a Google-style
  ``Args:`` section that names each of them (``*args`` / ``**kwargs`` as written);
* a class users construct themselves documents its constructor parameters in an ``Args:``
  section of the class docstring (mkdocstrings shows the class docstring, not ``__init__``'s);
* a dataclass documents its fields in an ``Attributes:`` section.
"""

from __future__ import annotations

import dataclasses
import inspect
import re

import stest
import stest.can

# Classes the user instantiates directly (the others come from factories: sim.node(),
# node.pin(), sim.can_bus(), ...), so their constructor parameters are public API.
CONSTRUCTED = {"Sim", "CanFrame", "CanDevice", "Dbc", "DbcDevice"}


def _public_objects():
    seen = {}
    for mod in (stest, stest.can):
        for name in mod.__all__:
            obj = getattr(mod, name)
            if inspect.isclass(obj) or inspect.isfunction(obj):
                seen.setdefault(name, obj)
    return seen


def _section(doc: str, title: str) -> str | None:
    """The body of a Google-style section (``Args:``), or None if absent."""
    m = re.search(rf"^([ \t]*){title}:[ \t]*$", doc, re.M)
    if not m:
        return None
    indent = len(m.group(1))
    body = []
    for line in doc[m.end():].splitlines()[1:]:
        if line.strip() and len(line) - len(line.lstrip()) <= indent:
            break
        body.append(line)
    return "\n".join(body)


def _params(fn) -> list[str]:
    out = []
    for p in inspect.signature(fn).parameters.values():
        if p.name in ("self", "cls"):
            continue
        prefix = {p.VAR_POSITIONAL: "*", p.VAR_KEYWORD: "**"}.get(p.kind, "")
        out.append(prefix + p.name)
    return out


def _check_args(where: str, doc: str, params: list[str], offenders: list[str]) -> None:
    if not params:
        return
    args = _section(doc, "Args")
    if args is None:
        offenders.append(f"{where}: no Args: section (parameters: {', '.join(params)})")
        return
    for p in params:
        if not re.search(rf"^\s*{re.escape(p)}\s*(\(.*?\))?\s*:", args, re.M):
            offenders.append(f"{where}: Args: does not document {p!r}")


def _own_doc(obj) -> str:
    doc = obj.__dict__.get("__doc__") if inspect.isclass(obj) else obj.__doc__
    doc = inspect.cleandoc(doc or "")
    if inspect.isclass(obj) and dataclasses.is_dataclass(obj) and doc.startswith(f"{obj.__name__}("):
        return ""  # the signature dataclasses generates when there is no docstring
    return doc


def find_offenders() -> list[str]:
    offenders: list[str] = []
    for name, obj in _public_objects().items():
        doc = _own_doc(obj)
        if not doc:
            offenders.append(f"{name}: no docstring")
        if inspect.isfunction(obj):
            _check_args(name, doc, _params(obj), offenders)
            continue
        if name in CONSTRUCTED:
            _check_args(f"{name} (constructor)", doc, _params(obj.__init__), offenders)
        if dataclasses.is_dataclass(obj) and _section(doc, "Attributes") is None:
            offenders.append(f"{name}: dataclass without an Attributes: section")
        for attr, member in vars(obj).items():
            if attr.startswith("_"):
                continue
            where = f"{name}.{attr}"
            if isinstance(member, property):
                if not inspect.cleandoc(member.__doc__ or ""):
                    offenders.append(f"{where}: no docstring")
            elif inspect.isfunction(member):
                mdoc = inspect.cleandoc(member.__doc__ or "")
                if not mdoc:
                    offenders.append(f"{where}: no docstring")
                _check_args(where, mdoc, _params(member), offenders)
    return offenders


def test_public_api_is_documented():
    offenders = find_offenders()
    assert not offenders, f"{len(offenders)} public API documentation problems:\n" + "\n".join(offenders)


def test_rules_catch_an_undocumented_method():
    class Probe:
        """Probe."""

        def documented(self, x):
            """Does it.

            Args:
                x: the input.
            """

        def missing_arg(self, x, y):
            """Does it.

            Args:
                x: the input.
            """

        def bare(self):
            pass

    out: list[str] = []
    _check_args("documented", Probe.documented.__doc__, _params(Probe.documented), out)
    _check_args("missing_arg", Probe.missing_arg.__doc__, _params(Probe.missing_arg), out)
    assert out == ["missing_arg: Args: does not document 'y'"]
    assert Probe.bare.__doc__ is None
