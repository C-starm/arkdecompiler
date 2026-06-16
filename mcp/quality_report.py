#!/usr/bin/env python3
"""One-shot decompilation quality report for a .hap / .abc sample.

Usage (once xabc is built):
    ARKDEC_BACKEND=docker python quality_report.py samples/orangeMall.hap
    ARKDEC_BACKEND=local  python quality_report.py samples/orangeMall.modules.abc

Unpacks (if a package), decompiles each abc, scores fidelity against symbols
retained in the bytecode, and prints a Markdown report. Works with the mock
backend too (scores will be ~0; useful only to exercise the plumbing).
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

from arkdecompiler_mcp.engine import Engine, EngineConfig
from arkdecompiler_mcp.quality import assess
from arkdecompiler_mcp.unpack import unpack, is_package


def _abcs_for(target: Path, workdir: Path):
    if is_package(target):
        res = unpack(target, workdir)
        return [(e.module_name, e.abc_path) for e in res.abcs]
    return [(target.stem, target)]


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    target = Path(argv[1])
    if not target.exists():
        print(f"not found: {target}")
        return 1

    engine = Engine(EngineConfig.from_env())
    print(f"# Decompilation quality report\n")
    print(f"- target: `{target}`")
    print(f"- backend: `{engine.cfg.backend}`")
    if engine.cfg.backend == "mock":
        print("\n> ⚠️ mock backend — scores are placeholders until xabc is wired up.\n")

    with tempfile.TemporaryDirectory(prefix="qr_") as td:
        items = _abcs_for(target, Path(td))
        print(f"- abc files: {len(items)}\n")
        for module, abc in items:
            dr = engine.decompile_abc(abc, want_ast=False)
            print(f"## module `{module}` — `{abc.name}`\n")
            if not dr.ok:
                print(f"**DECOMPILE FAILED**: {dr.error}\n")
                if dr.stderr:
                    print("```\n" + dr.stderr[-1500:] + "\n```\n")
                continue
            rep = assess(abc, dr.source)
            s = rep.summary()
            r = s["recall"]
            o = s["output"]
            print(f"| metric | value |")
            print(f"|---|---|")
            print(f"| overall fidelity | **{r['overall']}** |")
            print(f"| type-name recall | {r['type_names']} ({len(rep.found_types)}/{len(rep.baseline.type_names)}) |")
            print(f"| import recall | {r['imports']} ({len(rep.found_imports)}/{len(rep.baseline.imports)}) |")
            print(f"| source-file recall | {r['source_files']} ({len(rep.found_source_refs)}/{len(rep.baseline.source_files)}) |")
            print(f"| output lines | {o['lines']} |")
            print(f"| classes / functions | {o['classes']} / {o['functions']} |")
            print(f"| leftover temp regs (vNNN) | {o['leftover_temp_regs']} |")
            if s["missing_types_sample"]:
                print(f"\nMissing type names (sample): "
                      f"{', '.join(s['missing_types_sample'][:15])}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
