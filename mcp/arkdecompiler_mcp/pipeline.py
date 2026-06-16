"""High-level decompilation pipeline: HAP/app -> per-module ArkTS.

Combines :mod:`unpack` (extract abc from packages) with :mod:`engine`
(run xabc per abc) and organises the output by module.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

from .engine import Engine, DecompileResult
from .unpack import unpack, UnpackResult, AbcEntry


@dataclass
class ModuleOutput:
    module_name: str
    abc_arcname: str
    ok: bool
    source: str = ""
    error: str = ""
    ast_available: bool = False


@dataclass
class HapDecompileResult:
    package: str
    module_count: int
    abc_count: int
    outputs: list[ModuleOutput] = field(default_factory=list)
    written_to: Optional[str] = None

    @property
    def ok(self) -> bool:
        return any(o.ok for o in self.outputs)


def decompile_hap(
    package: Path,
    engine: Engine,
    workdir: Path,
    output_dir: Optional[Path] = None,
    want_ast: bool = False,
) -> HapDecompileResult:
    """Unpack a hap/app, decompile every abc, optionally write .ts to disk.

    Output layout when ``output_dir`` is given::

        output_dir/<module_name>/<abc-stem>.ts
        output_dir/<module_name>/<abc-stem>.ast.json   (if want_ast)
    """
    unpacked: UnpackResult = unpack(Path(package), Path(workdir))
    result = HapDecompileResult(
        package=str(package),
        module_count=len(unpacked.modules),
        abc_count=len(unpacked.abcs),
    )

    for entry in unpacked.abcs:
        dr = engine.decompile_abc(entry.abc_path, want_ast=want_ast)
        out = ModuleOutput(
            module_name=entry.module_name,
            abc_arcname=entry.arcname,
            ok=dr.ok,
            source=dr.source,
            error=dr.error,
            ast_available=dr.ast is not None,
        )
        result.outputs.append(out)

        if output_dir and dr.ok:
            _write_output(Path(output_dir), entry, dr, want_ast)

    if output_dir:
        result.written_to = str(Path(output_dir).resolve())
    return result


def _write_output(output_dir: Path, entry: AbcEntry, dr: DecompileResult,
                  want_ast: bool) -> None:
    import json
    mod_dir = output_dir / _safe(entry.module_name)
    mod_dir.mkdir(parents=True, exist_ok=True)
    stem = Path(entry.arcname).stem or "modules"
    (mod_dir / f"{stem}.ts").write_text(dr.source)
    if want_ast and dr.ast is not None:
        (mod_dir / f"{stem}.ast.json").write_text(
            json.dumps(dr.ast, indent=2, ensure_ascii=False))


def _safe(name: str) -> str:
    """Sanitise a module name for use as a directory component."""
    keep = "-_."
    cleaned = "".join(c if c.isalnum() or c in keep else "_" for c in name)
    return cleaned.strip("_") or "module"
