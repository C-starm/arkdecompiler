"""Tests for the quality-assessment module (no native binary needed)."""

from pathlib import Path

from arkdecompiler_mcp.quality import Baseline, assess, extract_baseline, _strings


def test_strings_extraction():
    data = b"\x00\x01hello\x00world\xffAB"  # "AB" too short (min_len=4)
    got = _strings(data, min_len=4)
    assert "hello" in got and "world" in got
    assert "AB" not in got


def test_baseline_from_synthetic_abc(tmp_path):
    # craft a blob carrying ground-truth-style symbols
    blob = (b"PANDA\x00\x00\x00"
            b"\x00pages/HomePage.ets\x00"
            b"\x00#HomeModel#\x00#AddressInfo#\x00"
            b"\x00@ohos:router\x00@bundle:com.x.y\x00")
    abc = tmp_path / "t.abc"
    abc.write_bytes(blob)
    b = extract_baseline(abc)
    assert "pages/HomePage.ets" in b.source_files
    assert {"HomeModel", "AddressInfo"} <= b.type_names
    assert "@ohos:router" in b.imports


def test_assess_recall_and_signals():
    base = Baseline(
        source_files={"pages/HomePage.ets", "model/User.ets"},
        type_names={"HomeModel", "AddressInfo", "UserVM"},
        imports={"@ohos:router", "@ohos:window"},
    )
    src = (
        'import router from "@ohos:router";\n'
        'class HomeModel {}\n'
        'class AddressInfo {}\n'
        '// HomePage stuff\n'
        'function f() { let v254 = 1; let v0 = v254; return v0; }\n'
    )
    rep = assess(Path("x.abc"), src, base)
    # 2 of 3 types present (value is rounded to 3 dp)
    assert rep.type_recall() == round(2 / 3, 3)
    # 1 of 2 imports
    assert rep.import_recall() == 0.5
    # HomePage stem referenced
    assert "HomePage" in rep.found_source_refs
    # leftover temp regs detected (v254, v0)
    assert rep.n_placeholders == 2
    assert rep.n_classes == 2
    assert 0 < rep.overall() < 1


def test_real_abc_baseline_if_present():
    abc = Path(__file__).resolve().parents[1] / "samples" / "orangeMall.modules.abc"
    if not abc.exists():
        return  # sample not downloaded in this env; skip silently
    b = extract_baseline(abc)
    assert len(b.type_names) > 30
    assert len(b.source_files) > 30
    assert any(i.startswith("@ohos:") for i in b.imports)
