"""Blender registration contract for the no-bundle source UX."""

import os
import sys

import pytest

bpy = pytest.importorskip("bpy")


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "addon"))

import mh4blend  # noqa: E402
from mh4blend.ui import ops, panels  # noqa: E402


def test_register_exposes_only_standalone_workflow_properties_and_panels():
    mh4blend.register()
    try:
        scene_type = bpy.types.Scene
        assert hasattr(scene_type, "mh_fbx_collection")
        assert hasattr(scene_type, "mh_fbx_directory")
        assert hasattr(scene_type, "mh_fbx_export_materials")
        assert bpy.context.scene.mh_fbx_export_materials is True
        assert hasattr(scene_type, "mh_material")
        assert hasattr(scene_type, "mh_material_directory")
        assert hasattr(scene_type, "mh_composite_mode")
        assert hasattr(scene_type, "mh_composite_import_path")
        assert hasattr(scene_type, "mh_composite_export_collection")
        assert hasattr(scene_type, "mh_composite_export_directory")
        assert not hasattr(scene_type, "mh_bundle_subdir")

        assert {cls.bl_idname for cls in ops.CLASSES} == {
            "mh.export_fbx", "mh.export_material",
            "mh.export_composite", "mh.import_composite"}
        assert {cls.bl_category for cls in panels.CLASSES} == {"MH"}
        assert all("bundle" not in cls.bl_idname for cls in ops.CLASSES)
    finally:
        mh4blend.unregister()

    for name in (
        "mh_fbx_collection", "mh_fbx_directory", "mh_fbx_export_materials",
        "mh_composite_mode",
        "mh_composite_import_path", "mh_composite_export_collection",
        "mh_composite_export_directory",
        "mh_material", "mh_material_directory",
    ):
        assert not hasattr(bpy.types.Scene, name)
