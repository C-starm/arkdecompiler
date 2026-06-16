"""ArkDecompiler MCP server.

Exposes HarmonyOS NEXT decompilation as MCP tools so an MCP client (Claude
Desktop, Claude Code, etc.) can decompile .abc / .hap / .app to ArkTS.

Backend selection is via environment (see :class:`engine.EngineConfig`):

    ARKDEC_BACKEND   local | docker | mock   (default: mock)
    ARKDEC_XABC      path to xabc (local backend)
    ARKDEC_DISASM    path to ark_disasm
    ARKDEC_LD_LIBRARY_PATH   runtime_core:zlib paths (local backend)
    ARKDEC_DOCKER_IMAGE      docker image tag (docker backend)

Run with:  python -m arkdecompiler_mcp.server
"""

from __future__ import annotations

import tempfile
from pathlib import Path
from typing import Optional

from mcp.server.fastmcp import FastMCP

from .engine import Engine, EngineConfig
from .pipeline import decompile_hap
from .quality import assess, extract_baseline
from .unpack import unpack, is_package, UnpackError

mcp = FastMCP("arkdecompiler")
_engine = Engine(EngineConfig.from_env())

# Cap inline source returned to the model so we don't blow the context window.
MAX_INLINE_CHARS = 60_000


def _truncate(text: str) -> tuple[str, bool]:
    if len(text) <= MAX_INLINE_CHARS:
        return text, False
    return text[:MAX_INLINE_CHARS] + "\n// ...[truncated]...\n", True


@mcp.tool()
def backend_info() -> dict:
    """Report the active decompiler backend and whether it looks usable.

    Useful as a first call to confirm xabc is wired up before decompiling.
    """
    cfg = _engine.cfg
    info = {"backend": cfg.backend, "timeout_s": cfg.timeout}
    if cfg.backend == "local":
        info["xabc_path"] = cfg.xabc_path
        info["xabc_exists"] = bool(cfg.xabc_path and Path(cfg.xabc_path).exists())
        info["disasm_exists"] = bool(cfg.ark_disasm_path and Path(cfg.ark_disasm_path).exists())
    elif cfg.backend == "docker":
        info["docker_image"] = cfg.docker_image
    elif cfg.backend == "mock":
        info["note"] = "MOCK backend: returns placeholder output. Set ARKDEC_BACKEND=local|docker for real decompilation."
    return info


@mcp.tool()
def inspect_package(package_path: str) -> dict:
    """Inspect a .hap/.hsp/.app without decompiling.

    Lists the modules and .abc bytecode files inside, so you can see what
    decompile_package would process. Returns module names/types and abc sizes.
    """
    pkg = Path(package_path)
    if not is_package(pkg):
        return {"error": f"not a hap/hsp/app package: {package_path}"}
    with tempfile.TemporaryDirectory(prefix="arkinspect_") as td:
        try:
            res = unpack(pkg, Path(td))
        except UnpackError as e:
            return {"error": str(e)}
        return {
            "package": str(pkg),
            "modules": [
                {"name": m.name, "type": m.type, "bundle": m.package_name}
                for m in res.modules
            ],
            "abcs": [
                {"module": e.module_name, "arcname": e.arcname,
                 "size": e.abc_path.stat().st_size}
                for e in res.abcs
            ],
            "primary_abc": res.primary_abc().arcname if res.primary_abc() else None,
        }


@mcp.tool()
def decompile_abc(abc_path: str, include_ast: bool = False) -> dict:
    """Decompile a single Panda bytecode (.abc) file to ArkTS source.

    Args:
        abc_path: path to a .abc file on the server's filesystem.
        include_ast: also return the reconstructed AST (JSON).
    """
    path = Path(abc_path)
    if path.suffix.lower() != ".abc":
        return {"ok": False, "error": f"expected a .abc file, got {abc_path}"}
    dr = _engine.decompile_abc(path, want_ast=include_ast)
    source, truncated = _truncate(dr.source)
    out = {
        "ok": dr.ok,
        "abc": dr.abc_name,
        "source": source,
        "truncated": truncated,
    }
    if not dr.ok:
        out["error"] = dr.error
        out["stderr"] = dr.stderr[-2000:]
    if include_ast and dr.ast is not None:
        out["ast"] = dr.ast
    return out


@mcp.tool()
def decompile_package(
    package_path: str,
    output_dir: Optional[str] = None,
    include_ast: bool = False,
    modules_only: bool = False,
) -> dict:
    """Decompile an entire HarmonyOS .hap/.hsp/.app to ArkTS, per module.

    Unpacks the package, finds every .abc, and decompiles each. If output_dir
    is given, writes <module>/<name>.ts files there and returns a summary;
    otherwise returns decompiled source inline (truncated if large).

    Args:
        package_path: path to a .hap/.hsp/.app file.
        output_dir: optional directory to write decompiled .ts files into.
        include_ast: also emit reconstructed AST.
        modules_only: only decompile the primary (entry) module's abc.
    """
    pkg = Path(package_path)
    if not is_package(pkg):
        return {"ok": False, "error": f"not a hap/hsp/app package: {package_path}"}

    with tempfile.TemporaryDirectory(prefix="arkpkg_") as td:
        try:
            if modules_only:
                # Decompile just the primary abc for speed.
                res = unpack(pkg, Path(td))
                primary = res.primary_abc()
                if not primary:
                    return {"ok": False, "error": "no abc found"}
                dr = _engine.decompile_abc(primary.abc_path, want_ast=include_ast)
                source, truncated = _truncate(dr.source)
                return {
                    "ok": dr.ok,
                    "package": str(pkg),
                    "module": primary.module_name,
                    "abc": primary.arcname,
                    "source": source,
                    "truncated": truncated,
                    "error": dr.error if not dr.ok else None,
                }

            result = decompile_hap(
                pkg, _engine, Path(td),
                output_dir=Path(output_dir) if output_dir else None,
                want_ast=include_ast,
            )
        except UnpackError as e:
            return {"ok": False, "error": str(e)}

        summary = {
            "ok": result.ok,
            "package": result.package,
            "module_count": result.module_count,
            "abc_count": result.abc_count,
            "written_to": result.written_to,
            "modules": [],
        }
        for o in result.outputs:
            entry = {
                "module": o.module_name,
                "abc": o.abc_arcname,
                "ok": o.ok,
            }
            if not o.ok:
                entry["error"] = o.error
            if not output_dir:
                src, trunc = _truncate(o.source)
                entry["source"] = src
                entry["truncated"] = trunc
            summary["modules"].append(entry)
        return summary


@mcp.tool()
def disassemble_abc(abc_path: str) -> dict:
    """Disassemble a .abc to Panda assembly (.pa) text.

    Lower-level than decompile; works even when full decompilation fails.
    """
    dr = _engine.disassemble(Path(abc_path))
    source, truncated = _truncate(dr.source)
    out = {"ok": dr.ok, "abc": dr.abc_name, "disassembly": source, "truncated": truncated}
    if not dr.ok:
        out["error"] = dr.error
    return out


@mcp.tool()
def assess_quality(abc_path: str) -> dict:
    """Decompile a .abc and score reconstruction fidelity against abc symbols.

    Mines ground-truth symbols (class/interface names, imports, source-file
    names) retained in the .abc, decompiles it, and reports how many survive
    into the output (recall), plus structural signals like leftover temp
    registers (vNNN). A heuristic fidelity metric, not semantic equivalence.
    """
    path = Path(abc_path)
    if path.suffix.lower() != ".abc":
        return {"ok": False, "error": f"expected a .abc file, got {abc_path}"}
    if not path.exists():
        return {"ok": False, "error": f"abc not found: {abc_path}"}
    dr = _engine.decompile_abc(path, want_ast=False)
    if not dr.ok:
        return {"ok": False, "error": dr.error, "stderr": dr.stderr[-2000:]}
    report = assess(path, dr.source)
    out = report.summary()
    out["ok"] = True
    if _engine.cfg.backend == "mock":
        out["warning"] = "mock backend: scores are meaningless until xabc is wired up"
    return out


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
