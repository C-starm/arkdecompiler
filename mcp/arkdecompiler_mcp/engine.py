"""Decompiler engine wrapper.

Wraps the native ``xabc`` decompiler (and ``ark_disasm``) behind a clean Python
API. Because ``xabc`` has several sharp edges, the wrapper isolates each run:

* output AST filename is hard-coded to ``arkdemo.ast`` in xabc; the source ts
  name is ``argv[2]``. To avoid collisions across modules / concurrency, every
  invocation runs in its own temp working directory.
* ``xabc`` writes per-function IR into ``./logs/``; that directory must exist.
* it needs ``LD_LIBRARY_PATH`` pointing at runtime_core + zlib.

Two backends are supported:

* ``local``  – invoke a natively-built xabc binary directly.
* ``docker`` – run xabc inside a built image (env-isolated; the default once the
  Phase-0 image exists).

A ``mock`` backend is provided so the MCP server and pipeline can be exercised
end-to-end before the native binary is built.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


class EngineError(Exception):
    pass


@dataclass
class EngineConfig:
    backend: str = "mock"               # "local" | "docker" | "mock"
    # local backend
    xabc_path: str = ""                 # path to xabc binary
    ark_disasm_path: str = ""           # path to ark_disasm binary
    ld_library_path: str = ""           # extra LD_LIBRARY_PATH for local backend
    # docker backend
    docker_image: str = "arkdecompiler:latest"
    docker_xabc: str = "/root/harmonyos/arkdecompiler/out/arkcompiler/common/xabc"
    docker_ld: str = ("/root/harmonyos/arkdecompiler/out/arkcompiler/runtime_core:"
                      "/root/harmonyos/arkdecompiler/out/thirdparty/zlib")
    docker_disasm: str = "/root/harmonyos/out/x64.release/arkcompiler/runtime_core/ark_disasm"
    timeout: int = 300

    @classmethod
    def from_env(cls) -> "EngineConfig":
        return cls(
            backend=os.environ.get("ARKDEC_BACKEND", "mock"),
            xabc_path=os.environ.get("ARKDEC_XABC", ""),
            ark_disasm_path=os.environ.get("ARKDEC_DISASM", ""),
            ld_library_path=os.environ.get("ARKDEC_LD_LIBRARY_PATH", ""),
            docker_image=os.environ.get("ARKDEC_DOCKER_IMAGE", "arkdecompiler:latest"),
            timeout=int(os.environ.get("ARKDEC_TIMEOUT", "300")),
        )


@dataclass
class DecompileResult:
    ok: bool
    source: str = ""                    # decompiled ArkTS source
    ast: Optional[dict] = None          # parsed AST JSON, if available
    stdout: str = ""
    stderr: str = ""
    returncode: int = 0
    abc_name: str = ""
    error: str = ""


# Names xabc writes into its CWD.
_OUT_TS = "arkdemo.ts"
_OUT_AST = "arkdemo.ast"


class Engine:
    def __init__(self, config: Optional[EngineConfig] = None):
        self.cfg = config or EngineConfig.from_env()

    # ---- public API -----------------------------------------------------

    def decompile_abc(self, abc_path: Path, want_ast: bool = True) -> DecompileResult:
        abc_path = Path(abc_path).resolve()
        if not abc_path.exists():
            return DecompileResult(ok=False, abc_name=abc_path.name,
                                   error=f"abc not found: {abc_path}")
        if self.cfg.backend == "mock":
            return self._mock(abc_path)
        if self.cfg.backend == "local":
            return self._run_local(abc_path, want_ast)
        if self.cfg.backend == "docker":
            return self._run_docker(abc_path, want_ast)
        return DecompileResult(ok=False, abc_name=abc_path.name,
                               error=f"unknown backend: {self.cfg.backend}")

    def disassemble(self, abc_path: Path) -> DecompileResult:
        abc_path = Path(abc_path).resolve()
        if not abc_path.exists():
            return DecompileResult(ok=False, abc_name=abc_path.name,
                                   error=f"abc not found: {abc_path}")
        if self.cfg.backend == "mock":
            r = self._mock(abc_path)
            r.source = f"// disasm (mock) of {abc_path.name}\n.function main() {{}}\n"
            return r
        if self.cfg.backend == "local":
            return self._disasm_local(abc_path)
        if self.cfg.backend == "docker":
            return self._disasm_docker(abc_path)
        return DecompileResult(ok=False, error=f"unknown backend: {self.cfg.backend}")

    # ---- mock backend ---------------------------------------------------

    def _mock(self, abc_path: Path) -> DecompileResult:
        size = abc_path.stat().st_size
        return DecompileResult(
            ok=True,
            abc_name=abc_path.name,
            source=(f"// [mock decompile] {abc_path.name} ({size} bytes)\n"
                    f"function main_0() {{\n  return undefined;\n}}\n"),
            ast={"type": "Program", "statements": [],
                 "_mock": True, "_abc": abc_path.name},
            returncode=0,
        )

    # ---- local backend --------------------------------------------------

    def _run_local(self, abc_path: Path, want_ast: bool) -> DecompileResult:
        if not self.cfg.xabc_path or not Path(self.cfg.xabc_path).exists():
            return DecompileResult(ok=False, abc_name=abc_path.name,
                                   error=f"xabc not found at {self.cfg.xabc_path!r}; "
                                         "set ARKDEC_XABC")
        with tempfile.TemporaryDirectory(prefix="arkdec_") as td:
            work = Path(td)
            (work / "logs").mkdir(exist_ok=True)
            local_abc = work / abc_path.name
            shutil.copy2(abc_path, local_abc)
            env = dict(os.environ)
            if self.cfg.ld_library_path:
                env["LD_LIBRARY_PATH"] = (
                    self.cfg.ld_library_path
                    + (":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
                )
            cmd = [self.cfg.xabc_path, abc_path.name, _OUT_TS]
            return self._finish_run(cmd, work, env, abc_path.name, want_ast)

    def _disasm_local(self, abc_path: Path) -> DecompileResult:
        if not self.cfg.ark_disasm_path or not Path(self.cfg.ark_disasm_path).exists():
            return DecompileResult(ok=False, abc_name=abc_path.name,
                                   error="ark_disasm not found; set ARKDEC_DISASM")
        with tempfile.TemporaryDirectory(prefix="arkdis_") as td:
            work = Path(td)
            out_pa = work / "out.pa"
            cmd = [self.cfg.ark_disasm_path, str(abc_path), str(out_pa)]
            proc = self._exec(cmd, work, dict(os.environ))
            src = out_pa.read_text(errors="replace") if out_pa.exists() else ""
            return DecompileResult(ok=proc.returncode == 0 and bool(src),
                                   source=src, stdout=proc.stdout, stderr=proc.stderr,
                                   returncode=proc.returncode, abc_name=abc_path.name,
                                   error="" if src else "no disassembly produced")

    # ---- docker backend -------------------------------------------------

    def _run_docker(self, abc_path: Path, want_ast: bool) -> DecompileResult:
        work = Path(tempfile.mkdtemp(prefix="arkdec_"))
        try:
            (work / "logs").mkdir(exist_ok=True)
            shutil.copy2(abc_path, work / abc_path.name)
            inner = (
                f"cd /w && export LD_LIBRARY_PATH={self.cfg.docker_ld} && "
                f"{self.cfg.docker_xabc} {abc_path.name} {_OUT_TS}"
            )
            cmd = ["docker", "run", "--rm", "-v", f"{work}:/w",
                   self.cfg.docker_image, "bash", "-lc", inner]
            return self._finish_run(cmd, work, dict(os.environ), abc_path.name,
                                    want_ast, cwd_for_exec=None)
        finally:
            shutil.rmtree(work, ignore_errors=True)

    def _disasm_docker(self, abc_path: Path) -> DecompileResult:
        work = Path(tempfile.mkdtemp(prefix="arkdis_"))
        try:
            shutil.copy2(abc_path, work / abc_path.name)
            inner = f"cd /w && {self.cfg.docker_disasm} {abc_path.name} out.pa"
            cmd = ["docker", "run", "--rm", "-v", f"{work}:/w",
                   self.cfg.docker_image, "bash", "-lc", inner]
            proc = self._exec(cmd, None, dict(os.environ))
            out_pa = work / "out.pa"
            src = out_pa.read_text(errors="replace") if out_pa.exists() else ""
            return DecompileResult(ok=proc.returncode == 0 and bool(src),
                                   source=src, stdout=proc.stdout, stderr=proc.stderr,
                                   returncode=proc.returncode, abc_name=abc_path.name,
                                   error="" if src else "no disassembly produced")
        finally:
            shutil.rmtree(work, ignore_errors=True)

    # ---- shared helpers -------------------------------------------------

    def _finish_run(self, cmd, work: Path, env, abc_name, want_ast,
                    cwd_for_exec="__work__") -> DecompileResult:
        cwd = work if cwd_for_exec == "__work__" else cwd_for_exec
        proc = self._exec(cmd, cwd, env)
        ts_file = work / _OUT_TS
        ast_file = work / _OUT_AST
        source = ts_file.read_text(errors="replace") if ts_file.exists() else ""
        ast = None
        if want_ast and ast_file.exists():
            try:
                ast = json.loads(ast_file.read_text(errors="replace"))
            except json.JSONDecodeError:
                ast = None
        ok = proc.returncode == 0 and bool(source)
        return DecompileResult(
            ok=ok, source=source, ast=ast,
            stdout=proc.stdout, stderr=proc.stderr,
            returncode=proc.returncode, abc_name=abc_name,
            error="" if ok else (proc.stderr.strip() or "decompile produced no output"),
        )

    def _exec(self, cmd, cwd, env) -> subprocess.CompletedProcess:
        try:
            return subprocess.run(
                cmd, cwd=cwd, env=env, capture_output=True, text=True,
                timeout=self.cfg.timeout,
            )
        except subprocess.TimeoutExpired as e:
            return subprocess.CompletedProcess(
                cmd, returncode=124, stdout=e.stdout or "",
                stderr=f"timeout after {self.cfg.timeout}s")
        except FileNotFoundError as e:
            return subprocess.CompletedProcess(
                cmd, returncode=127, stdout="", stderr=str(e))
