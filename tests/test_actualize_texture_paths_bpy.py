"""Blender-hosted disk workflow coverage for manual texture actualization."""

from __future__ import annotations

import json
import os
from pathlib import Path
import sys

import pytest

bpy = pytest.importorskip("bpy")


REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.material_source import (  # noqa: E402
    build_material_document,
)
from mh4blend.core.texture_actualize import TextureActualizeError  # noqa: E402
from mh4blend.scene import actualize_texture_paths as module  # noqa: E402


UID = "11111111-1111-4111-8111-111111111111"


def _write_json(path: Path, document: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(document, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8")


def _fixture(root: Path, textures: dict[str, str]) -> Path:
    owner = root / "materials"
    payload = owner / "wall.material"
    document, _diagnostics = build_material_document(
        uid=UID,
        name="wall",
        shader_class="rendinst_simple",
        params={},
        textures=textures,
        source_root=str(root),
    )
    _write_json(payload, document)
    return payload


def test_manual_actualize_updates_self_contained_material(tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    payload = _fixture(root, {
        "tex0": "old/wall_d.tif",
        "tex1": "old/wall_n.tif",
        "tex2": "old/missing_a.tif",
    })
    (root / "textures" / "walls").mkdir(parents=True)
    (root / "textures" / "walls" / "wall_d.tif").write_bytes(b"d")
    (root / "library_a").mkdir()
    (root / "library_b").mkdir()
    (root / "library_a" / "wall_n.tif").write_bytes(b"n1")
    (root / "library_b" / "wall_n.tif").write_bytes(b"n2")

    report = module.actualize_texture_paths(str(root))

    document = json.loads(payload.read_text(encoding="utf-8"))
    assert document["textures"] == {
        "tex0": "textures/walls/wall_d.tif",
        "tex1": "old/wall_n.tif",
        "tex2": "old/missing_a.tif",
    }
    assert report["materials_scanned"] == 1
    assert report["materials_updated"] == [UID]
    assert [(item["slot"], item["resolved_path"])
            for item in report["fixed"]] == [
                ("tex0", "textures/walls/wall_d.tif")]
    assert report["ambiguous"][0]["code"] == \
        "MH_W_TEXTURE_BASENAME_AMBIGUOUS"
    assert report["ambiguous"][0]["candidates"] == [
        "library_a/wall_n.tif", "library_b/wall_n.tif"]
    assert report["missing"][0]["code"] == "MH_W_TEXTURE_NOT_FOUND"
    assert {item["code"] for item in report["warnings"]} == {
        "MH_W_TEXTURE_BASENAME_AMBIGUOUS", "MH_W_TEXTURE_NOT_FOUND"}
    assert not list(root.rglob("*.material.tmp"))
    assert not (root / "old").exists()


def test_texture_tree_race_blocks_before_payload_mutation(
        tmp_path, monkeypatch):
    root = tmp_path / "source"
    root.mkdir()
    payload = _fixture(root, {
        "tex0": "old/wall_d.tif",
    })
    candidate = root / "textures" / "wall_d.tif"
    candidate.parent.mkdir()
    candidate.write_bytes(b"d")
    stable_payload = payload.read_bytes()
    real_assert = module.assert_texture_tree_stable
    calls = 0

    def race_before_write(snapshot):
        nonlocal calls
        calls += 1
        if calls == 2:
            extra = root / "appeared" / "wall_d.tif"
            extra.parent.mkdir()
            extra.write_bytes(b"new")
        return real_assert(snapshot)

    monkeypatch.setattr(module, "assert_texture_tree_stable", race_before_write)
    with pytest.raises(TextureActualizeError, match="file set changed"):
        module.actualize_texture_paths(str(root))

    assert payload.read_bytes() == stable_payload
    assert not list(root.rglob("*.material.tmp"))


def test_texture_tree_race_inside_material_lock_preserves_payload(
        tmp_path, monkeypatch):
    root = tmp_path / "source"
    root.mkdir()
    payload = _fixture(root, {"tex0": "old/wall_d.tif"})
    candidate = root / "textures" / "wall_d.tif"
    candidate.parent.mkdir()
    candidate.write_bytes(b"d")
    stable_payload = payload.read_bytes()
    real_write = module.write_material_payload_atomic

    def raced_write(prepared, **kwargs):
        extra = root / "appeared" / "wall_d.tif"
        extra.parent.mkdir()
        extra.write_bytes(b"new")
        return real_write(prepared, **kwargs)

    monkeypatch.setattr(module, "write_material_payload_atomic", raced_write)
    with pytest.raises(TextureActualizeError, match="file set changed"):
        module.actualize_texture_paths(str(root))

    assert payload.read_bytes() == stable_payload


def test_no_material_owners_is_successful_empty_report(tmp_path):
    root = tmp_path / "source"
    root.mkdir()

    report = module.actualize_texture_paths(str(root))

    assert report["ok"] is True
    assert report["materials_scanned"] == 0
    assert report["fixed"] == []
    assert report["ambiguous"] == []
    assert report["missing"] == []
