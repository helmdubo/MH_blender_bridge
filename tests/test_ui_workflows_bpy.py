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
            "mh_fbx_collection", "mh_fbx_directory", "mh_fbx_import_path",
            "mh_fbx_export_materials",
            "mh_material", "mh_material_directory", "mh_composite_mode",
            "mh_composite_import_path", "mh_composite_export_collection",
            "mh_composite_export_directory", "mh_dagor_composite_import_path",
            "mh_dag4blend_composite_collection",
        ):
            assert hasattr(scene_type, name)
        assert hasattr(bpy.types.Material, "mh4blend")
        assert hasattr(bpy.types.Object, "mh4blend")
        assert bpy.context.scene.mh_fbx_export_materials is False
        assert {cls.bl_idname for cls in ops.CLASSES} == {
            "mh.export_fbx", "mh.import_mesh_fbx", "mh.export_material",
            "mh.export_composite",
            "mh.import_composite", "mh.material_texture_add",
            "mh.material_texture_remove", "mh.material_param_add",
            "mh.material_param_remove", "mh.copy_all_textures_to_project",
            "mh.remap_all_textures_to_project",
            "mh.import_dagor_composite",
            "mh.convert_dag4blend_composite",
        }
        assert {cls.bl_category for cls in panels.CLASSES} == {"MH"}
    finally:
        mh4blend.unregister()

    for name in (
        "mh_fbx_collection", "mh_fbx_directory", "mh_fbx_import_path",
        "mh_fbx_export_materials",
        "mh_material", "mh_material_directory", "mh_composite_mode",
        "mh_composite_import_path", "mh_composite_export_collection",
        "mh_composite_export_directory", "mh_dagor_composite_import_path",
        "mh_dag4blend_composite_collection",
    ):
        assert not hasattr(bpy.types.Scene, name)
    assert not hasattr(bpy.types.Object, "mh4blend")


def test_material_export_option_is_forwarded_to_fbx_workflow(tmp_path, monkeypatch):
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
            lambda *_args, **kwargs: (
                calls.append(kwargs),
                {"filepath": str(tmp_path / "asset.mesh.fbx")},
            )[1],
        )

        assert bpy.ops.mh.export_fbx() == {"FINISHED"}
        assert len(calls) == 1
        assert calls[0]["export_materials"] is True
    finally:
        mh4blend.unregister()


def test_misc_texture_operators_share_project_root_and_log_reports(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mh4blend.register()
    try:
        monkeypatch.setattr(
            ops.prefs_mod,
            "get_prefs",
            lambda _context: SimpleNamespace(source_root=str(tmp_path)),
        )
        calls = []
        monkeypatch.setattr(
            ops,
            "copy_all_dagor_textures_to_project",
            lambda **kwargs: (
                calls.append(("copy", kwargs)),
                {"ok": True, "copied": 3, "skipped": 1},
            )[1],
        )
        monkeypatch.setattr(
            ops,
            "remap_all_dagor_textures_to_project",
            lambda **kwargs: (
                calls.append(("remap", kwargs)),
                {"ok": True, "remapped": 4},
            )[1],
        )

        assert bpy.ops.mh.copy_all_textures_to_project() == {"FINISHED"}
        assert bpy.ops.mh.remap_all_textures_to_project() == {"FINISHED"}
        assert calls == [
            ("copy", {"source_root": str(tmp_path)}),
            ("remap", {"source_root": str(tmp_path)}),
        ]
        log = bpy.data.texts[ops.LOG_TEXT_NAME].as_string()
        assert '"operation": "copy_all_textures_to_project"' in log
        assert '"operation": "remap_all_textures_to_project"' in log
    finally:
        mh4blend.unregister()


def test_mesh_import_operator_forwards_exact_source_path(tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mh4blend.register()
    try:
        source = tmp_path / "vehicle.mesh.fbx"
        source.write_bytes(b"test seam")
        bpy.context.scene.mh_fbx_import_path = str(source)
        monkeypatch.setattr(
            ops.prefs_mod,
            "get_prefs",
            lambda _context: SimpleNamespace(source_root=str(tmp_path)),
        )
        calls = []
        monkeypatch.setattr(
            ops,
            "import_mesh_fbx",
            lambda path, **kwargs: (
                calls.append((path, kwargs)),
                {"ok": True, "resource_name": "vehicle"},
            )[1],
        )
        assert bpy.ops.mh.import_mesh_fbx() == {"FINISHED"}
        assert calls == [(str(source), {"source_root": str(tmp_path)})]
    finally:
        mh4blend.unregister()


def test_composite_import_operator_reports_unresolved_placement_warning(
        tmp_path, monkeypatch, capsys):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mh4blend.register()
    try:
        source = tmp_path / "unresolved.composite"
        source.write_bytes(b'{\n  "nodes": []\n}\n')
        bpy.context.scene.mh_composite_import_path = str(source)
        monkeypatch.setattr(
            ops.prefs_mod,
            "get_prefs",
            lambda _context: SimpleNamespace(source_root=str(tmp_path)),
        )
        monkeypatch.setattr(
            ops,
            "import_composite_file",
            lambda *_args, **_kwargs: {
                "ok": True,
                "warnings": [{
                    "code": "MH_W_UNRESOLVED_PLACEMENT",
                    "message": "mesh resource 'missing' is unavailable",
                }],
            },
        )
        assert bpy.ops.mh.import_composite() == {"FINISHED"}
        output = capsys.readouterr().out
        assert ("Warning: MH_W_UNRESOLVED_PLACEMENT: mesh resource "
                "'missing' is unavailable") in output
    finally:
        mh4blend.unregister()


def test_material_collection_operators_use_deterministic_valid_defaults():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mh4blend.register()
    try:
        material = bpy.data.materials.new("wall")
        bpy.context.scene.mh_material = material
        settings = material.mh4blend
        settings.mode = "CLASS"

        assert bpy.ops.mh.material_texture_add() == {"FINISHED"}
        assert bpy.ops.mh.material_texture_add() == {"FINISHED"}
        assert [row.slot for row in settings.textures] == [0, 1]
        assert bpy.ops.mh.material_texture_remove(index=0) == {"FINISHED"}
        assert [row.slot for row in settings.textures] == [1]
        assert bpy.ops.mh.material_texture_add() == {"FINISHED"}
        assert [row.slot for row in settings.textures] == [1, 0]

        assert bpy.ops.mh.material_param_add() == {"FINISHED"}
        assert bpy.ops.mh.material_param_add() == {"FINISHED"}
        assert [row.name for row in settings.params] == ["param", "param_1"]
        assert all(row.kind == "SCALAR" for row in settings.params)
        assert bpy.ops.mh.material_param_remove(index=0) == {"FINISHED"}
        assert [row.name for row in settings.params] == ["param_1"]
    finally:
        mh4blend.unregister()


def test_texture_add_is_fail_closed_when_all_slots_are_used_or_library_mode():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mh4blend.register()
    try:
        material = bpy.data.materials.new("wall")
        bpy.context.scene.mh_material = material
        settings = material.mh4blend
        settings.mode = "CLASS"
        for _index in range(16):
            assert bpy.ops.mh.material_texture_add() == {"FINISHED"}
        assert [row.slot for row in settings.textures] == list(range(16))
        with pytest.raises(RuntimeError, match="all texture slots"):
            bpy.ops.mh.material_texture_add()
        assert len(settings.textures) == 16

        settings.mode = "LIBRARY"
        with pytest.raises(RuntimeError, match="cannot have overrides"):
            bpy.ops.mh.material_param_add()
        assert len(settings.params) == 0
    finally:
        mh4blend.unregister()


def test_composite_picker_does_not_accept_uppercase_extension(tmp_path):
    path = tmp_path / "asset.COMPOSITE"
    path.write_text("{}", encoding="utf-8")
    with pytest.raises(ValueError, match="MH_E_NONCANONICAL_RESOURCE_NAME"):
        ops._filepath(str(path))


def test_dagor_conversion_operators_forward_explicit_sources(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mh4blend.register()
    try:
        source = tmp_path / "vehicle.composit.blk"
        source.write_text('className:t="composit"\n', encoding="utf-8")
        scene = bpy.context.scene
        scene.mh_dagor_composite_import_path = str(source)
        collection = bpy.data.collections.new("dag4blend_source")
        scene.mh_dag4blend_composite_collection = collection
        monkeypatch.setattr(
            ops.prefs_mod,
            "get_prefs",
            lambda _context: SimpleNamespace(source_root=str(tmp_path)),
        )
        calls = []
        monkeypatch.setattr(
            ops,
            "import_dagor_composite_file",
            lambda path, **kwargs: (
                calls.append(("file", path, kwargs)),
                {"ok": True},
            )[1],
        )
        monkeypatch.setattr(
            ops,
            "import_dag4blend_composite_collection",
            lambda value: (
                calls.append(("scene", value)),
                {"ok": True},
            )[1],
        )

        assert bpy.ops.mh.import_dagor_composite() == {"FINISHED"}
        assert bpy.ops.mh.convert_dag4blend_composite() == {"FINISHED"}
        assert calls == [
            ("file", str(source), {"source_root": str(tmp_path)}),
            ("scene", collection),
        ]
    finally:
        mh4blend.unregister()
