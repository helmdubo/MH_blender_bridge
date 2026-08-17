"""Blender integration tests for standalone collection FBX export."""

import importlib
import json
import sys
import warnings
from pathlib import Path

import pytest

bpy = pytest.importorskip("bpy")

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.scene.export_fbx import export_fbx_collection  # noqa: E402
export_fbx_module = importlib.import_module(  # noqa: E402
    "mh4blend.scene.export_fbx")


class _FbxTestTextures(bpy.types.PropertyGroup):
    tex0: bpy.props.StringProperty(default="", subtype="FILE_PATH")
    tex1: bpy.props.StringProperty(default="", subtype="FILE_PATH")
    tex2: bpy.props.StringProperty(default="", subtype="FILE_PATH")


class _FbxTestOptional(bpy.types.PropertyGroup):
    pass


class _FbxTestDagormat(bpy.types.PropertyGroup):
    shader_class: bpy.props.StringProperty(default="")
    sides: bpy.props.IntProperty(default=0, min=0, max=2)
    optional: bpy.props.PointerProperty(type=_FbxTestOptional)
    textures: bpy.props.PointerProperty(type=_FbxTestTextures)


@pytest.fixture(scope="module", autouse=True)
def dagormat_rna():
    if hasattr(bpy.types.Material, "dagormat"):
        pytest.skip("test dagormat RNA conflicts with an already enabled addon")
    classes = (_FbxTestTextures, _FbxTestOptional, _FbxTestDagormat)
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.Material.dagormat = bpy.props.PointerProperty(type=_FbxTestDagormat)
    yield
    del bpy.types.Material.dagormat
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)


def _mesh_object(name, collection):
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata(
        [(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    return obj


def _build_joined_collection():
    scene = bpy.context.scene
    selected = bpy.data.collections.new("SelectedResource")
    child = bpy.data.collections.new("SelectedChild")
    sibling = bpy.data.collections.new("UnrelatedSibling")
    scene.collection.children.link(selected)
    selected.children.link(child)
    scene.collection.children.link(sibling)
    direct_obj = _mesh_object("DirectMesh", selected)
    child_obj = _mesh_object("ChildMesh", child)
    sibling_obj = _mesh_object("SiblingMesh", sibling)
    return selected, direct_obj, child_obj, sibling_obj


def test_exports_selected_collection_joined_and_restores_host_state(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected, direct_obj, child_obj, sibling_obj = _build_joined_collection()
    direct_obj.location = (1.25, -2.0, 3.5)
    child_obj.location = (-4.0, 5.0, 0.25)
    direct_matrix = direct_obj.matrix_basis.copy()
    child_matrix = child_obj.matrix_basis.copy()
    mesh_vertices = [vertex.co.copy() for vertex in direct_obj.data.vertices]
    units = (
        bpy.context.scene.unit_settings.system,
        bpy.context.scene.unit_settings.scale_length,
        bpy.context.scene.unit_settings.length_unit,
    )

    direct_obj.select_set(False)
    child_obj.select_set(False)
    sibling_obj.select_set(True)
    bpy.context.view_layer.objects.active = sibling_obj

    report = export_fbx_collection(selected, tmp_path)

    assert report["ok"]
    assert report["objects_exported"] == 2
    exported = Path(report["filepath"])
    assert exported.parent == tmp_path
    assert exported.name.endswith(".mesh.fbx")
    assert exported.exists()
    assert Path(report["manifest_path"]).exists()
    assert report["manifest_written"]
    assert report["resource_entry"]["source"] == exported.name
    assert sibling_obj.select_get()
    assert not direct_obj.select_get()
    assert not child_obj.select_get()
    assert bpy.context.view_layer.objects.active == sibling_obj
    assert direct_obj.matrix_basis == direct_matrix
    assert child_obj.matrix_basis == child_matrix
    assert [vertex.co for vertex in direct_obj.data.vertices] == mesh_vertices
    assert (
        bpy.context.scene.unit_settings.system,
        bpy.context.scene.unit_settings.scale_length,
        bpy.context.scene.unit_settings.length_unit,
    ) == units

    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=str(exported))
    imported_names = {obj.name for obj in bpy.context.scene.objects
                      if obj.type == "MESH"}
    assert "DirectMesh" in imported_names
    assert "ChildMesh" in imported_names
    assert "SiblingMesh" not in imported_names


def test_reports_direct_texture_paths_and_never_copies_textures(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    blend_dir = tmp_path / "authoring"
    blend_dir.mkdir()
    bpy.ops.wm.save_as_mainfile(filepath=str(blend_dir / "asset.blend"))
    selected = bpy.data.collections.new("MaterialResource")
    bpy.context.scene.collection.children.link(selected)
    obj = _mesh_object("MaterialMesh", selected)
    material = bpy.data.materials.new("AuthoredMaterial")
    material.dagormat.shader_class = "rendinst_simple"
    absolute_path = str(tmp_path / "source" / "metal_d.tif")
    relative_authored_path = "shared/textures/metal_n.tif"
    blender_relative_path = "//textures/metal_mask.tif"
    material.dagormat.textures.tex0 = absolute_path
    material.dagormat.textures.tex1 = relative_authored_path
    # Blender 4.5 warns when ``//`` is assigned to a FILE_PATH RNA property,
    # but dag4blend still stores the authored value and the bridge must resolve
    # it. Keep the compatibility assertion without polluting the test run.
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        material.dagormat.textures.tex2 = blender_relative_path
    obj.data.materials.append(material)

    output_dir = tmp_path / "fbx"
    report = export_fbx_collection(selected, output_dir)

    assert report["material_entries"][0]["textures"] == {
        "tex0": absolute_path,
        "tex1": relative_authored_path,
        "tex2": bpy.path.abspath(blender_relative_path),
    }
    manifest = json.loads(
        Path(report["manifest_path"]).read_text(encoding="utf-8"))
    assert manifest["materials"][0]["textures"] == \
        report["material_entries"][0]["textures"]
    assert {path.name for path in output_dir.iterdir()} == {
        Path(report["filepath"]).name,
        "export_manifest.json",
    }
    assert not (output_dir / "metal_d.tif").exists()
    assert not (output_dir / "metal_n.tif").exists()
    assert not (output_dir / "metal_mask.tif").exists()


def test_dry_run_prepares_manifest_entries_without_creating_output(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("DryResource")
    bpy.context.scene.collection.children.link(selected)
    _mesh_object("DryMesh", selected)
    output_dir = tmp_path / "not-created"

    report = export_fbx_collection(selected, output_dir, dry_run=True)

    assert report["ok"]
    assert not report["written"]
    assert not report["manifest_written"]
    assert report["objects_exported"] == 1
    assert report["resource_entry"]["source"].endswith(".mesh.fbx")
    assert not output_dir.exists()


def test_optional_registry_warning_is_reported_without_blocking(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("RegistryResource")
    bpy.context.scene.collection.children.link(selected)
    obj = _mesh_object("RegistryMesh", selected)
    material = bpy.data.materials.new("UnknownShader")
    material.dagormat.shader_class = "custom_unknown"
    obj.data.materials.append(material)
    registry = tmp_path / "registry.json"
    registry.write_text(json.dumps({
        "schema": "mh.registry",
        "schema_version": 1,
        "shader_classes": ["rendinst_simple"],
    }), encoding="utf-8")

    report = export_fbx_collection(
        selected, tmp_path / "output", dry_run=True,
        registry_path=str(registry))

    assert report["ok"]
    assert [row["code"] for row in report["validation"]["warnings"]] == [
        "MH_W_UNKNOWN_SHADER_CLASS"]


def test_failed_payload_keeps_pending_manifest_for_fail_closed_readers(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("InterruptedResource")
    bpy.context.scene.collection.children.link(selected)
    _mesh_object("InterruptedMesh", selected)

    def fail_export(_filepath):
        raise RuntimeError("simulated FBX failure")

    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx", fail_export)
    with pytest.raises(RuntimeError, match="simulated FBX failure"):
        export_fbx_collection(selected, tmp_path)

    assert (tmp_path / "export_manifest.json.tmp").exists()
    assert not (tmp_path / "export_manifest.json").exists()
    assert not any(path.name.endswith(".mesh.fbx") for path in tmp_path.iterdir())
    assert not any(path.name.endswith(".mesh.fbx.tmp") for path in tmp_path.iterdir())
