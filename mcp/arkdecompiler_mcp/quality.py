"""Decompilation quality assessment.

Panda .abc retains a lot of ground-truth symbols (source-file paths, class/
interface names, imported module specifiers, method names). We can therefore
score a decompiled output by how many of those symbols survive into the
reconstructed ArkTS — a cheap, objective fidelity metric that needs no original
source.

This is a heuristic baseline, not a semantic equivalence check: a high score
means symbols/structure are preserved; it does not prove behavioural fidelity.
"""

from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path


# Symbol extraction patterns over the raw `strings` dump of an .abc.
_RE_ETS = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_/]+\.ets")
# Class/interface names appear two ways across abc variants: the `#Name#`
# annotation form (seen in some apps) and the Panda `L...../Name;` type
# descriptor form (the general case). Capture the trailing identifier of both.
_RE_CLASS_HASH = re.compile(r"#([A-Z][A-Za-z0-9_]+)#")
_RE_CLASS_DESC = re.compile(r"L[A-Za-z0-9_/@.]*?([A-Z][A-Za-z0-9_]+);")
# Imports include framework (@ohos/@bundle/@app) and package (@package/@native/
# @normalized) module specifiers used by ohpm/Flutter-ported apps.
_RE_IMPORT = re.compile(r"@(?:ohos|bundle|app|package|native|normalized):[A-Za-z0-9_./@+=-]+")


@dataclass
class Baseline:
    """Ground-truth symbols mined from an .abc file."""

    source_files: set[str] = field(default_factory=set)   # *.ets paths
    type_names: set[str] = field(default_factory=set)      # class/interface/struct names
    imports: set[str] = field(default_factory=set)         # @ohos:/@bundle: specifiers

    def total(self) -> int:
        return len(self.source_files) + len(self.type_names) + len(self.imports)


@dataclass
class QualityReport:
    abc: str
    baseline: Baseline
    found_types: set[str] = field(default_factory=set)
    found_imports: set[str] = field(default_factory=set)
    found_source_refs: set[str] = field(default_factory=set)
    decompiled_chars: int = 0
    decompiled_lines: int = 0
    # crude structural signals in the output
    n_functions: int = 0
    n_classes: int = 0
    n_placeholders: int = 0   # e.g. v254-style temp register names left over

    def type_recall(self) -> float:
        return _ratio(len(self.found_types), len(self.baseline.type_names))

    def import_recall(self) -> float:
        return _ratio(len(self.found_imports), len(self.baseline.imports))

    def source_recall(self) -> float:
        return _ratio(len(self.found_source_refs), len(self.baseline.source_files))

    def overall(self) -> float:
        """Weighted blend of the recalls (type names matter most)."""
        return round(
            0.5 * self.type_recall()
            + 0.3 * self.import_recall()
            + 0.2 * self.source_recall(),
            3,
        )

    def summary(self) -> dict:
        return {
            "abc": self.abc,
            "baseline_counts": {
                "type_names": len(self.baseline.type_names),
                "imports": len(self.baseline.imports),
                "source_files": len(self.baseline.source_files),
            },
            "recall": {
                "type_names": self.type_recall(),
                "imports": self.import_recall(),
                "source_files": self.source_recall(),
                "overall": self.overall(),
            },
            "output": {
                "chars": self.decompiled_chars,
                "lines": self.decompiled_lines,
                "functions": self.n_functions,
                "classes": self.n_classes,
                "leftover_temp_regs": self.n_placeholders,
            },
            "missing_types_sample": sorted(
                self.baseline.type_names - self.found_types)[:20],
        }


def _ratio(a: int, b: int) -> float:
    return round(a / b, 3) if b else 0.0


def _strings(data: bytes, min_len: int = 4) -> list[str]:
    """Pure-python `strings` so we don't depend on binutils."""
    out, cur = [], bytearray()
    for byte in data:
        if 32 <= byte < 127:
            cur.append(byte)
        else:
            if len(cur) >= min_len:
                out.append(cur.decode("ascii"))
            cur.clear()
    if len(cur) >= min_len:
        out.append(cur.decode("ascii"))
    return out


def extract_baseline(abc_path: Path) -> Baseline:
    data = Path(abc_path).read_bytes()
    blob = "\n".join(_strings(data))
    b = Baseline()
    b.source_files = {m.lstrip("_") for m in _RE_ETS.findall(blob)}
    # Common framework/runtime type names that aren't app classes — exclude so
    # recall reflects app-specific reconstruction, not stdlib noise.
    _NOISE = {"Object", "Array", "String", "Number", "Boolean", "Function",
              "Promise", "Map", "Set", "Error", "Date", "RegExp", "Symbol",
              "JSON", "Math", "Reflect", "Proxy"}
    types = set(_RE_CLASS_HASH.findall(blob)) | set(_RE_CLASS_DESC.findall(blob))
    b.type_names = {t for t in types if t not in _NOISE and len(t) > 2}
    b.imports = set(_RE_IMPORT.findall(blob))
    return b


def assess(abc_path: Path, decompiled_source: str,
           baseline: Baseline | None = None) -> QualityReport:
    abc_path = Path(abc_path)
    baseline = baseline or extract_baseline(abc_path)
    src = decompiled_source
    rep = QualityReport(abc=abc_path.name, baseline=baseline)

    rep.decompiled_chars = len(src)
    rep.decompiled_lines = src.count("\n") + 1 if src else 0

    # Symbol recall: does each ground-truth symbol appear in the output text?
    rep.found_types = {t for t in baseline.type_names if re.search(rf"\b{re.escape(t)}\b", src)}
    rep.found_imports = {i for i in baseline.imports if i in src}
    # source-file recall: match by basename stem (paths rarely survive verbatim)
    stems = {Path(p).stem for p in baseline.source_files}
    rep.found_source_refs = {s for s in stems if re.search(rf"\b{re.escape(s)}\b", src)}

    # Structural signals.
    rep.n_functions = len(re.findall(r"\bfunction\b", src))
    rep.n_classes = len(re.findall(r"\bclass\b", src))
    rep.n_placeholders = len(re.findall(r"\bv\d{2,}\b", src))  # v0, v254, ...
    return rep
