"""Blender registration and S1 transitional UI gates."""

from pathlib import Path
from types import SimpleNamespace
import sys

import pytest

bpy = pytest.importorskip("bpy")
REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

import mh4blend  # noqa: E402
from mh4blend.ui import ops, panels  # noqa: E402


def test_register_exposes_only_v4_workflow_surfaces():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mh4blend.register()
    try:
        scene_type = bpy.types.Scene
        for name in (
            "mh_fbx_collection", "mh_fbx_directory", "mh_fbx_export_materials",
            "mh_material", "mh_material_directory", "mh_composite_mode",
            "mh_composite_import_path", "mh_composite_export_collection",
            "mh_composite_export_directory",
        ):
            assert hasattr(scene_type, name)
        assert bpy.context.scene.mh_fbx_export_materials is False
        assert {cls.bl_idname for cls in ops.CLASSES} == {
            "mh.export_fbx", "mh.export_material", "mh.export_composite",
            "mh.import_composite",
        }
        assert {cls.bl_category for cls in panels.CLASSES} == {"MH"}
    finally:
        mh4blend.unregister()

    for name in (
        "mh_fbx_collection", "mh_fbx_directory", "mh_fbx_export_materials",
        "mh_material", "mh_material_directory", "mh_composite_mode",
        "mh_composite_import_path", "mh_composite_export_collection",
        "mh_composite_export_directory",
    ):
        assert not hasattr(bpy.types.Scene, name)


def test_pending_material_export_blocks_before_fbx_publish(tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mh4blend.register()
    try:
        scene = bpy.context.scene
        collection = bpy.data.collections.new("asset")
        scene.collection.children.link(collection)
        scene.mh_fbx_collection = collection
        scene.mh_fbx_directory = str(tmp_path)
        scene.mh_fbx_export_materials = True
        monkeypatch.setattr(
            ops.prefs_mod,
            "get_prefs",
            lambda _context: SimpleNamespace(source_root=str(tmp_path)),
        )
        calls = []
        monkeypatch.setattr(
            ops,
            "export_fbx_collection",
            lambda *_args, **_kwargs: calls.append("FBX"),
        )

        with pytest.raises(RuntimeError, match="unavailable until slice S2"):
            bpy.ops.mh.export_fbx()
        assert calls == []
        assert not list(tmp_path.iterdir())
        assert "unavailable until slice S2" in bpy.data.texts[
            ops.LOG_TEXT_NAME].as_string()
    finally:
        mh4blend.unregister()
