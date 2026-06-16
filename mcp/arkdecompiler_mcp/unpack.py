"""HAP / .app unpacking layer.

A HarmonyOS NEXT ``.hap`` is a ZIP archive. The compiled ArkTS bytecode lives
under ``ets/`` (typically ``ets/modules.abc``). A ``.app`` is an outer ZIP that
bundles one or more ``.hap``/``.hsp`` modules plus a ``pack.info``.

This module locates and extracts every ``.abc`` inside such packages and reads
the module metadata (``module.json``) so callers can organise decompiled output
per module. It depends on nothing but the Python standard library, so it can be
developed and tested without the native ``xabc`` binary.
"""

from __future__ import annotations

import json
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


# Extensions we treat as "a package that may contain abc files".
PACKAGE_SUFFIXES = {".hap", ".hsp", ".app"}
# Nested module containers found inside a .app.
MODULE_SUFFIXES = {".hap", ".hsp"}


class UnpackError(Exception):
    """Raised when a package cannot be opened or contains no bytecode."""


@dataclass
class AbcEntry:
    """One extracted .abc file plus where it came from."""

    abc_path: Path          # extracted file on disk
    arcname: str            # path inside the (innermost) archive, e.g. "ets/modules.abc"
    module_name: str        # owning module ("entry", "<root>", or a nested hap name)
    source_package: str     # name of the package this came from


@dataclass
class ModuleInfo:
    """Parsed module.json metadata (best-effort; fields may be missing)."""

    name: str = ""
    type: str = ""          # "entry" | "feature" | "shared" | ...
    package_name: str = ""
    raw: dict = field(default_factory=dict)


@dataclass
class UnpackResult:
    package: Path
    workdir: Path
    abcs: list[AbcEntry] = field(default_factory=list)
    modules: list[ModuleInfo] = field(default_factory=list)

    def primary_abc(self) -> Optional[AbcEntry]:
        """The abc most likely to be the app's main code.

        Prefer an ``entry`` module's modules.abc, then any ``modules.abc``,
        then simply the largest abc.
        """
        if not self.abcs:
            return None
        entry_names = {m.name for m in self.modules if m.type == "entry"}
        ranked = sorted(
            self.abcs,
            key=lambda e: (
                e.module_name in entry_names,
                e.arcname.endswith("modules.abc"),
                e.abc_path.stat().st_size if e.abc_path.exists() else 0,
            ),
            reverse=True,
        )
        return ranked[0]


def is_package(path: Path) -> bool:
    return path.suffix.lower() in PACKAGE_SUFFIXES


def _safe_extract_member(zf: zipfile.ZipFile, member: str, dest_root: Path) -> Path:
    """Extract a single member, guarding against Zip Slip path traversal."""
    dest_root = dest_root.resolve()
    target = (dest_root / member).resolve()
    if not str(target).startswith(str(dest_root) + "/") and target != dest_root:
        raise UnpackError(f"unsafe path in archive: {member!r}")
    target.parent.mkdir(parents=True, exist_ok=True)
    with zf.open(member) as src, open(target, "wb") as out:
        out.write(src.read())
    return target


def _read_module_json(zf: zipfile.ZipFile) -> Optional[ModuleInfo]:
    """Read module.json (compiled) from a hap/hsp zip if present."""
    for cand in ("module.json", "module.json5"):
        if cand in zf.namelist():
            try:
                data = json.loads(zf.read(cand).decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                return None
            mod = data.get("module", {}) if isinstance(data, dict) else {}
            app = data.get("app", {}) if isinstance(data, dict) else {}
            return ModuleInfo(
                name=mod.get("name", ""),
                type=mod.get("type", ""),
                package_name=app.get("bundleName", mod.get("packageName", "")),
                raw=data,
            )
    return None


def _extract_abcs_from_zip(
    zip_bytes_or_path,
    dest: Path,
    module_label: str,
    source_pkg: str,
) -> tuple[list[AbcEntry], Optional[ModuleInfo]]:
    """Pull every *.abc out of one hap/hsp zip into ``dest``."""
    entries: list[AbcEntry] = []
    with zipfile.ZipFile(zip_bytes_or_path) as zf:
        module = _read_module_json(zf)
        label = (module.name if module and module.name else module_label) or module_label
        for name in zf.namelist():
            if name.lower().endswith(".abc"):
                out = _safe_extract_member(zf, name, dest)
                entries.append(
                    AbcEntry(
                        abc_path=out,
                        arcname=name,
                        module_name=label,
                        source_package=source_pkg,
                    )
                )
    return entries, module


def unpack(package: Path, workdir: Path) -> UnpackResult:
    """Extract all abc files from a .hap / .hsp / .app into ``workdir``.

    For a ``.app`` (which nests .hap/.hsp modules), each inner module is
    extracted into its own subdirectory and processed.
    """
    package = Path(package)
    workdir = Path(workdir)
    workdir.mkdir(parents=True, exist_ok=True)

    if not package.exists():
        raise UnpackError(f"package not found: {package}")
    if not zipfile.is_zipfile(package):
        raise UnpackError(f"not a zip-based package (hap/hsp/app): {package}")

    result = UnpackResult(package=package, workdir=workdir)
    suffix = package.suffix.lower()

    if suffix == ".app":
        # Outer container: find nested .hap/.hsp, extract each, recurse one level.
        with zipfile.ZipFile(package) as outer:
            nested = [n for n in outer.namelist()
                      if Path(n).suffix.lower() in MODULE_SUFFIXES]
            if not nested:
                # Some .app pack abc directly; fall back to flat extraction.
                entries, mod = _extract_abcs_from_zip(
                    package, workdir / "_root", "<root>", package.name)
                result.abcs.extend(entries)
                if mod:
                    result.modules.append(mod)
            for nname in nested:
                stem = Path(nname).stem
                sub = workdir / stem
                hap_path = _safe_extract_member(outer, nname, workdir / "_modules")
                entries, mod = _extract_abcs_from_zip(
                    hap_path, sub, stem, f"{package.name}!{nname}")
                result.abcs.extend(entries)
                if mod:
                    result.modules.append(mod)
    else:
        # Single hap/hsp.
        entries, mod = _extract_abcs_from_zip(
            package, workdir, package.stem, package.name)
        result.abcs.extend(entries)
        if mod:
            result.modules.append(mod)

    if not result.abcs:
        raise UnpackError(f"no .abc bytecode found in {package}")

    return result
