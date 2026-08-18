from __future__ import annotations

import os
from pathlib import Path
import sys

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.texture_actualize import (  # noqa: E402
    TextureActualizeError,
    actualize_material_document,
    actualize_texture_path,
    assert_texture_tree_stable,
    capture_texture_tree,
    texture_basename_key,
)


def _touch(path: Path, payload: bytes = b"texture") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    return path


def test_exact_relative_path_wins_before_same_basename_candidates(tmp_path):
    _touch(tmp_path / "authored" / "wall_d.tif", b"exact")
    _touch(tmp_path / "moved" / "wall_d.tif", b"duplicate")
    snapshot = capture_texture_tree(tmp_path)

    result = actualize_texture_path("authored/wall_d.tif", snapshot)

    assert result.status == "exact"
    assert result.resolved_path is None
    assert result.code is None


def test_unique_basename_repairs_to_relative_forward_slash_path(tmp_path):
    target = _touch(tmp_path / "textures" / "walls" / "wall_d.tif")
    snapshot = capture_texture_tree(tmp_path)

    result = actualize_texture_path("old/library/wall_d.tif", snapshot)

    assert result.status == "fixed"
    assert result.resolved_path == "textures/walls/wall_d.tif"
    assert result.candidates == ("textures/walls/wall_d.tif",)
    assert target.read_bytes() == b"texture"
    assert not (tmp_path / "old").exists()


def test_basename_key_is_nfc_casefold_and_keeps_full_extension():
    assert texture_basename_key("old/ME\u0301TAL_D.TIF") == \
        texture_basename_key("M\u00e9tal_d.tif")
    assert texture_basename_key("metal_d.tif") != \
        texture_basename_key("metal_d.png")


def test_ambiguous_basename_is_stable_warning_and_never_repairs(tmp_path):
    _touch(tmp_path / "b" / "wall_d.tif")
    _touch(tmp_path / "a" / "WALL_D.TIF")
    snapshot = capture_texture_tree(tmp_path)

    result = actualize_texture_path("gone/wall_d.tif", snapshot)

    assert result.status == "ambiguous"
    assert result.resolved_path is None
    assert result.code == "MH_W_TEXTURE_BASENAME_AMBIGUOUS"
    assert result.candidates == ("a/WALL_D.TIF", "b/wall_d.tif")


def test_missing_basename_has_stable_warning_with_filename(tmp_path):
    snapshot = capture_texture_tree(tmp_path)

    result = actualize_texture_path("gone/missing_n.tif", snapshot)

    assert result.status == "missing"
    assert result.basename == "missing_n.tif"
    assert result.code == "MH_W_TEXTURE_NOT_FOUND"
    assert result.candidates == ()


def test_existing_absolute_external_texture_is_left_unchanged(tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    external = _touch(tmp_path / "library" / "shared_d.tif")
    snapshot = capture_texture_tree(root)

    result = actualize_texture_path(str(external), snapshot)

    assert result.status == "exact"
    assert result.authored_path == str(external)


def test_material_document_fixes_only_unique_stale_slots(tmp_path):
    _touch(tmp_path / "textures" / "unique_d.tif")
    _touch(tmp_path / "one" / "ambiguous_n.tif")
    _touch(tmp_path / "two" / "ambiguous_n.tif")
    document = {
        "schema": "mh.material",
        "schema_version": 1,
        "uid": "11111111-1111-4111-8111-111111111111",
        "name": "wall",
        "shader_class": "rendinst_simple",
        "params": {},
        "textures": {
            "tex0": "old/unique_d.tif",
            "tex1": "old/ambiguous_n.tif",
            "tex2": "old/missing_a.tif",
        },
    }
    snapshot = capture_texture_tree(tmp_path)

    updated, results = actualize_material_document(document, snapshot)

    assert document["textures"]["tex0"] == "old/unique_d.tif"
    assert updated["textures"] == {
        "tex0": "textures/unique_d.tif",
        "tex1": "old/ambiguous_n.tif",
        "tex2": "old/missing_a.tif",
    }
    assert [(slot, result.status) for slot, result in results] == [
        ("tex0", "fixed"),
        ("tex1", "ambiguous"),
        ("tex2", "missing"),
    ]


def test_snapshot_guard_detects_candidate_appearance(tmp_path):
    snapshot = capture_texture_tree(tmp_path)
    _touch(tmp_path / "textures" / "new.tif")

    with pytest.raises(TextureActualizeError, match="file set changed"):
        assert_texture_tree_stable(snapshot)


def test_writer_coordination_files_do_not_invalidate_texture_snapshot(tmp_path):
    snapshot = capture_texture_tree(tmp_path)
    _touch(tmp_path / ".mh_source_root.lock", b"lock")
    _touch(tmp_path / "assets" / ".mh_manifest_writer.lock", b"lock")
    _touch(tmp_path / "assets" / "export_manifest.json.tmp", b"marker")

    assert_texture_tree_stable(snapshot)


@pytest.mark.skipif(
    not hasattr(os, "symlink"), reason="platform has no symlink support")
def test_relative_symlink_escape_is_never_a_texture_candidate(tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    external = _touch(tmp_path / "outside" / "escape.tif")
    link = root / "escape.tif"
    try:
        link.symlink_to(external)
    except OSError:
        pytest.skip("symlinks are unavailable to this test process")

    snapshot = capture_texture_tree(root)
    assert texture_basename_key("escape.tif") not in snapshot.basename_index
    with pytest.raises(TextureActualizeError, match="resolves outside"):
        actualize_texture_path("escape.tif", snapshot)
