"""End-to-end tests using the mock engine backend.

Verifies the pipeline (unpack -> decompile per module -> write output) and the
MCP tool functions, all without the native xabc binary.
"""

import json
import zipfile
from pathlib import Path

import pytest

from arkdecompiler_mcp.engine import Engine, EngineConfig
from arkdecompiler_mcp.pipeline import decompile_hap
from arkdecompiler_mcp import server


def _make_hap(path: Path, module_name="entry", module_type="entry",
              abc_names=("ets/modules.abc",)):
    with zipfile.ZipFile(path, "w") as zf:
        zf.writestr("module.json", json.dumps({
            "app": {"bundleName": "com.example.app"},
            "module": {"name": module_name, "type": module_type},
        }))
        for n in abc_names:
            zf.writestr(n, b"ABC" + n.encode())


@pytest.fixture
def mock_engine():
    return Engine(EngineConfig(backend="mock"))


def test_pipeline_inline(tmp_path, mock_engine):
    hap = tmp_path / "entry.hap"
    _make_hap(hap, abc_names=("ets/modules.abc", "ets/widget.abc"))
    res = decompile_hap(hap, mock_engine, tmp_path / "work")
    assert res.ok
    assert res.abc_count == 2
    assert all(o.ok for o in res.outputs)
    assert any("mock decompile" in o.source for o in res.outputs)


def test_pipeline_writes_files(tmp_path, mock_engine):
    hap = tmp_path / "entry.hap"
    _make_hap(hap)
    out = tmp_path / "out"
    res = decompile_hap(hap, mock_engine, tmp_path / "work",
                        output_dir=out, want_ast=True)
    assert res.written_to
    ts = out / "entry" / "modules.ts"
    assert ts.exists()
    assert (out / "entry" / "modules.ast.json").exists()
    assert "mock decompile" in ts.read_text()


def test_server_tools_mock(tmp_path, monkeypatch):
    # Force the server's module-level engine to mock for deterministic tests.
    monkeypatch.setattr(server, "_engine", Engine(EngineConfig(backend="mock")))

    hap = tmp_path / "entry.hap"
    _make_hap(hap)

    info = server.backend_info()
    assert info["backend"] == "mock"

    insp = server.inspect_package(str(hap))
    assert insp["modules"][0]["name"] == "entry"
    assert insp["primary_abc"] == "ets/modules.abc"

    pkg = server.decompile_package(str(hap))
    assert pkg["ok"]
    assert pkg["module_count"] == 1
    assert pkg["modules"][0]["module"] == "entry"

    # modules_only path
    only = server.decompile_package(str(hap), modules_only=True)
    assert only["ok"]
    assert only["abc"] == "ets/modules.abc"


def test_decompile_abc_rejects_non_abc(monkeypatch):
    monkeypatch.setattr(server, "_engine", Engine(EngineConfig(backend="mock")))
    r = server.decompile_abc("/tmp/whatever.hap")
    assert not r["ok"]
    assert "expected a .abc" in r["error"]


def test_inspect_rejects_non_package():
    r = server.inspect_package("/tmp/foo.abc")
    assert "error" in r
