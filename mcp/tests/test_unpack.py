"""Tests for the HAP/app unpacking layer.

These build synthetic zip-based packages in-memory so they run without any
native binary or real HarmonyOS sample.
"""

import io
import json
import zipfile
from pathlib import Path

import pytest

from arkdecompiler_mcp.unpack import unpack, UnpackError, is_package


def _make_hap(path: Path, *, module_name="entry", module_type="entry",
              abc_names=("ets/modules.abc",), with_module_json=True,
              extra=()):
    with zipfile.ZipFile(path, "w") as zf:
        if with_module_json:
            zf.writestr("module.json", json.dumps({
                "app": {"bundleName": "com.example.app"},
                "module": {"name": module_name, "type": module_type},
            }))
        for n in abc_names:
            zf.writestr(n, b"FAKE_ABC_BYTES_" + n.encode())
        for n in extra:
            zf.writestr(n, b"x")


def _make_app(path: Path, hap_specs):
    """hap_specs: list of (inner_hap_name, module_name, module_type)."""
    with zipfile.ZipFile(path, "w") as outer:
        outer.writestr("pack.info", "{}")
        for inner_name, mod_name, mod_type in hap_specs:
            buf = io.BytesIO()
            _make_hap(Path("/dev/stdout"), module_name=mod_name,
                      module_type=mod_type)  # placeholder, rebuild below
            # build inner hap into the buffer
            buf = io.BytesIO()
            with zipfile.ZipFile(buf, "w") as inner:
                inner.writestr("module.json", json.dumps({
                    "app": {"bundleName": "com.example.app"},
                    "module": {"name": mod_name, "type": mod_type},
                }))
                inner.writestr("ets/modules.abc", b"ABC_" + mod_name.encode())
            outer.writestr(inner_name, buf.getvalue())


def test_is_package():
    assert is_package(Path("a.hap"))
    assert is_package(Path("a.HAP"))
    assert is_package(Path("a.app"))
    assert is_package(Path("a.hsp"))
    assert not is_package(Path("a.abc"))
    assert not is_package(Path("a.txt"))


def test_unpack_simple_hap(tmp_path):
    hap = tmp_path / "entry.hap"
    _make_hap(hap)
    res = unpack(hap, tmp_path / "work")
    assert len(res.abcs) == 1
    e = res.abcs[0]
    assert e.arcname == "ets/modules.abc"
    assert e.module_name == "entry"
    assert e.abc_path.exists()
    assert e.abc_path.read_bytes().startswith(b"FAKE_ABC_BYTES_")
    assert res.modules[0].type == "entry"
    assert res.modules[0].package_name == "com.example.app"


def test_primary_abc_prefers_entry(tmp_path):
    hap = tmp_path / "entry.hap"
    _make_hap(hap, abc_names=("ets/modules.abc", "ets/widget.abc"))
    res = unpack(hap, tmp_path / "work")
    assert res.primary_abc().arcname == "ets/modules.abc"


def test_unpack_hap_without_module_json(tmp_path):
    hap = tmp_path / "lib.hap"
    _make_hap(hap, with_module_json=False)
    res = unpack(hap, tmp_path / "work")
    assert len(res.abcs) == 1
    # falls back to package stem as module label
    assert res.abcs[0].module_name == "lib"


def test_unpack_app_with_nested_haps(tmp_path):
    app = tmp_path / "demo.app"
    _make_app(app, [
        ("entry.hap", "entry", "entry"),
        ("feature.hap", "feature", "feature"),
    ])
    res = unpack(app, tmp_path / "work")
    assert len(res.abcs) == 2
    names = {e.module_name for e in res.abcs}
    assert names == {"entry", "feature"}
    # primary should be the entry module
    assert res.primary_abc().module_name == "entry"


def test_no_abc_raises(tmp_path):
    hap = tmp_path / "empty.hap"
    with zipfile.ZipFile(hap, "w") as zf:
        zf.writestr("module.json", "{}")
    with pytest.raises(UnpackError, match="no .abc"):
        unpack(hap, tmp_path / "work")


def test_not_a_zip_raises(tmp_path):
    bogus = tmp_path / "fake.hap"
    bogus.write_bytes(b"not a zip")
    with pytest.raises(UnpackError, match="not a zip"):
        unpack(bogus, tmp_path / "work")


def test_zip_slip_blocked(tmp_path):
    hap = tmp_path / "evil.hap"
    with zipfile.ZipFile(hap, "w") as zf:
        zf.writestr("../escape.abc", b"evil")
    with pytest.raises(UnpackError, match="unsafe path"):
        unpack(hap, tmp_path / "work")
