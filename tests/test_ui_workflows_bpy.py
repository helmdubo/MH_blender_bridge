"""Blender registration contract for the no-bundle source UX."""

import os
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

bpy = pytest.importorskip("bpy")


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "addon"))

import mh4blend  # noqa: E402
from mh4blend.ui import ops, panels  # noqa: E402
from mh4blend.scene.export_fbx import export_fbx_collection  # noqa: E402


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
            "mh.resolve_name_collision",
            "mh.export_fbx", "mh.export_material",
            "mh.actualize_texture_paths",
            "mh.export_composite", "mh.import_composite",
            "mh.migrate_sources_v2"}
        assert {cls.bl_category for cls in panels.CLASSES} == {"MH"}
        assert all("bundle" not in cls.bl_idname for cls in ops.CLASSES)
        assert "one combined FBX" in ops.MH_OT_export_fbx.bl_description
        fbx_collection_description = scene_type.bl_rna.properties[
            "mh_fbx_collection"].description
        assert "one combined FBX" in fbx_collection_description
        assert "separate FBX" not in fbx_collection_description
    finally:
        mh4blend.unregister()

    for name in (
        "mh_fbx_collection", "mh_fbx_directory", "mh_fbx_export_materials",
        "mh_composite_mode",
        "mh_composite_import_path", "mh_composite_export_collection",
        "mh_composite_export_directory",
        "mh_material", "mh_material_directory",
        "mh_collision_kind", "mh_collision_message",
    ):
        assert not hasattr(bpy.types.Scene, name)


def test_fbx_operator_keeps_geometry_and_continues_after_material_failure(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mh4blend.register()
    try:
        scene = bpy.context.scene
        collection = bpy.data.collections.new("Asset")
        scene.collection.children.link(collection)
        good = bpy.data.materials.new("Good")
        good["mh_uid"] = "11111111-1111-4111-8111-111111111111"
        bad = bpy.data.materials.new("Bad")
        bad["mh_uid"] = "22222222-2222-4222-8222-222222222222"
        scene.mh_fbx_collection = collection
        scene.mh_fbx_directory = str(tmp_path)
        scene.mh_fbx_export_materials = True
        monkeypatch.setattr(
            ops.prefs_mod, "get_prefs",
            lambda _context: SimpleNamespace(
                source_root=str(tmp_path), texture_policy="strict"))

        fbx_path = tmp_path / "asset.mesh.fbx"

        def fake_export(*_args, **_kwargs):
            fbx_path.write_bytes(b"published")
            return {
                "ok": True, "filepath": str(fbx_path), "materials": [bad, good],
                "validation": {"errors": [], "warnings": []},
            }

        def fake_prepare(material, *_args, **_kwargs):
            if material is bad:
                raise ValueError(
                    "MH_E_TEXTURE_OUTSIDE_ROOT: bad external texture")
            return SimpleNamespace(
                payload_path=str(tmp_path / "good.material"))

        def fake_write(prepared, **_kwargs):
            Path(prepared.payload_path).write_text("ok", encoding="utf-8")
            return True

        monkeypatch.setattr(ops, "export_fbx_collection", fake_export)
        monkeypatch.setattr(ops, "prepare_blender_material_export", fake_prepare)
        monkeypatch.setattr(ops, "write_prepared_material", fake_write)

        assert bpy.ops.mh.export_fbx() == {"FINISHED"}
        assert fbx_path.read_bytes() == b"published"
        assert (tmp_path / "good.material").is_file()
        logged = bpy.data.texts[ops.LOG_TEXT_NAME].as_string()
        assert bad["mh_uid"] in logged
        assert "Export materials…" in logged
    finally:
        mh4blend.unregister()


def test_migration_operator_uses_project_root_and_logs_receipt(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mh4blend.register()
    try:
        monkeypatch.setattr(
            ops.prefs_mod, "get_prefs",
            lambda _context: SimpleNamespace(
                source_root=str(tmp_path), texture_policy="transitional"))
        calls = []

        def fake_migrate(source_root, *, dry_run=False):
            calls.append((source_root, dry_run))
            return {
                "ok": True, "blocked": False, "dry_run": dry_run,
                "migrated": [{"uid": "11111111-1111-4111-8111-111111111111"}],
                "renamed": [], "failed": [], "manifests_removed": [],
            }

        monkeypatch.setattr(ops, "migrate_sources_v2", fake_migrate)
        assert bpy.ops.mh.migrate_sources_v2(dry_run=False) == {"FINISHED"}
        assert calls == [(str(tmp_path.resolve()), False)]
        logged = bpy.data.texts[ops.LOG_TEXT_NAME].as_string()
        assert '"operation": "migrate_sources_v2"' in logged
    finally:
        mh4blend.unregister()


def test_collision_rename_preserves_lods_authoring_hierarchy(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mh4blend.register()
    try:
        scene = bpy.context.scene
        root = bpy.data.collections.new("Garage.lods")
        scene.collection.children.link(root)
        for level in (0, 1):
            child = bpy.data.collections.new(f"Garage.lod{level:02d}")
            root.children.link(child)
            mesh = bpy.data.meshes.new(f"LOD{level}Mesh")
            mesh.from_pydata(
                [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)],
                [], [(0, 1, 2)])
            child.objects.link(bpy.data.objects.new(f"Body{level}", mesh))
        scene.mh_fbx_collection = root
        scene.mh_collision_kind = "FBX"

        assert bpy.ops.mh.resolve_name_collision(
            action="RENAME", new_name="Renamed Garage") == {"FINISHED"}
        assert root.name == "Garage.lods"
        assert root["name"] == "Renamed Garage"
        report = export_fbx_collection(
            root, tmp_path, source_root=tmp_path, dry_run=True)
        assert report["passport"]["name"] == "Renamed Garage"
        assert report["passport"]["lod_levels"] == [0, 1]
    finally:
        mh4blend.unregister()
