"""Integration test: §9 hashing over real golden-scene data (requires the
pip bpy module; skipped where Blender is unavailable)."""

import sys
from pathlib import Path

import pytest

bpy = pytest.importorskip("bpy")

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))
sys.path.insert(0, str(REPO_ROOT / "tools"))

from golden_uids import UIDS  # noqa: E402
from mh4blend.scene.mesh_extract import collection_content_hash  # noqa: E402

GOLDEN = REPO_ROOT / "golden" / "golden.blend"
MUTATIONS = REPO_ROOT / "golden" / "mutations"

pytestmark = pytest.mark.skipif(
    not GOLDEN.exists(), reason="golden.blend not generated (run tools/run_all.py)")


def open_scene(path):
    bpy.ops.wm.open_mainfile(filepath=str(path))
    bpy.context.window.scene = bpy.data.scenes["GEOMETRY"]


def hash_of(col_uid):
    collection = next(
        c for c in bpy.data.collections if c.get("mh_uid") == col_uid)
    return collection_content_hash(collection)


def test_hash_is_stable_across_reload():
    open_scene(GOLDEN)
    first = hash_of(UIDS["col/wall_a"])
    open_scene(GOLDEN)
    assert hash_of(UIDS["col/wall_a"]) == first
    assert first.startswith("xxh3:") and len(first) == 21


def test_edit_geometry_changes_hash_others_do_not():
    open_scene(GOLDEN)
    base = {key: hash_of(UIDS[f"col/{key}"])
            for key in ("wall_a", "wall_b", "window_a", "decal_leak")}

    open_scene(MUTATIONS / "edit_geometry.blend")
    assert hash_of(UIDS["col/wall_a"]) != base["wall_a"], \
        "moved vertex must change the §9 hash"
    assert hash_of(UIDS["col/wall_b"]) == base["wall_b"]

    open_scene(MUTATIONS / "rename_collection.blend")
    assert hash_of(UIDS["col/wall_a"]) == base["wall_a"], \
        "rename must NOT change the §9 hash (anti-requirement §9.4)"

    open_scene(MUTATIONS / "rename_object.blend")
    assert hash_of(UIDS["col/window_a"]) == base["window_a"]


def test_all_golden_resources_hash_distinctly():
    open_scene(GOLDEN)
    hashes = [hash_of(UIDS[f"col/{key}"])
              for key in ("wall_a", "wall_b", "window_a", "decal_leak")]
    assert len(set(hashes)) == len(hashes)
