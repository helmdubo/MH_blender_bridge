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
from mh4blend.scene.export_material import (  # noqa: E402
    prepare_blender_material_export,
    write_prepared_material,
)
from mh4blend.scene.source_manifest import (  # noqa: E402
    abandon_staged_manifest,
    commit_staged_manifest,
    prepare_manifest_update,
    stage_manifest,
)
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

    report = export_fbx_collection(
        selected, tmp_path, source_root=tmp_path)

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
    report = export_fbx_collection(
        selected, output_dir,
        export_materials=True,
        source_root=tmp_path,
    )

    assert report["materials_exported"] == 1
    assert len(report["material_updates"]) == 1
    material_path = Path(report["material_updates"][0]["path"])
    material_payload = json.loads(material_path.read_text(encoding="utf-8"))
    assert material_payload["textures"] == {
        "tex0": "source/metal_d.tif",
        "tex1": "shared/textures/metal_n.tif",
        "tex2": "authoring/textures/metal_mask.tif",
    }
    manifest = json.loads(
        Path(report["manifest_path"]).read_text(encoding="utf-8"))
    material_rows = [
        row for row in manifest["resources"] if row["kind"] == "material"]
    assert material_rows == report["material_entries"]
    assert material_rows[0]["source"] == material_path.name
    assert {path.name for path in output_dir.iterdir()
            if not path.name.startswith(".mh_")} == {
        Path(report["filepath"]).name,
        material_path.name,
        "export_manifest.json",
    }
    assert not (output_dir / "metal_d.tif").exists()
    assert not (output_dir / "metal_n.tif").exists()
    assert not (output_dir / "metal_mask.tif").exists()


def test_export_materials_false_leaves_material_payloads_untouched(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("GeometryOnly")
    bpy.context.scene.collection.children.link(selected)
    obj = _mesh_object("GeometryOnlyMesh", selected)
    material = bpy.data.materials.new("ReferencedMaterial")
    material.dagormat.shader_class = "rendinst_simple"
    obj.data.materials.append(material)

    output_dir = tmp_path / "fbx"
    report = export_fbx_collection(
        selected, output_dir,
        export_materials=False,
        source_root=tmp_path,
    )

    assert report["materials_exported"] == 0
    assert report["material_entries"] == []
    assert report["material_updates"] == []
    assert [warning["code"] for warning in report["validation"]["warnings"]] == [
        "MH_W_MATERIAL_NOT_FOUND"]
    assert report["validation"]["warnings"][0]["subjects"] == [
        material["mh_uid"]]
    assert not list(output_dir.glob("*.material"))
    manifest = json.loads(
        Path(report["manifest_path"]).read_text(encoding="utf-8"))
    assert [row["kind"] for row in manifest["resources"]] == ["static_mesh"]
    assert manifest["resources"][0]["material_slots"][0]["material_uid"] == \
        material["mh_uid"]


def test_export_materials_true_allows_collection_without_materials(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("UnmaterialedGeometry")
    bpy.context.scene.collection.children.link(selected)
    _mesh_object("UnmaterialedMesh", selected)

    output_dir = tmp_path / "fbx"
    report = export_fbx_collection(
        selected, output_dir,
        export_materials=True,
        source_root=tmp_path,
    )

    assert report["ok"]
    assert report["materials"] == []
    assert report["materials_exported"] == 0
    assert report["material_entries"] == []
    assert report["material_updates"] == []
    assert not list(output_dir.glob("*.material"))


def test_material_write_failure_warns_but_keeps_geometry_export(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("GeometrySurvivesMaterialFailure")
    bpy.context.scene.collection.children.link(selected)
    obj = _mesh_object("GeometryMesh", selected)
    material = bpy.data.materials.new("FailingMaterial")
    material.dagormat.shader_class = "rendinst_simple"
    obj.data.materials.append(material)

    def fail_material(*_args, **_kwargs):
        raise OSError("injected material write failure")

    monkeypatch.setattr(
        export_fbx_module, "write_prepared_material", fail_material)
    output_dir = tmp_path / "fbx"
    report = export_fbx_collection(
        selected, output_dir,
        export_materials=True,
        source_root=tmp_path,
    )

    assert report["ok"]
    assert report["written"] and report["manifest_written"]
    assert Path(report["filepath"]).is_file()
    assert report["materials_exported"] == 0
    assert report["material_updates"][0]["ok"] is False
    assert any(warning["code"] == "MH_W_MATERIAL_NOT_FOUND"
               for warning in report["validation"]["warnings"])
    stable = json.loads(
        (output_dir / "export_manifest.json").read_text(encoding="utf-8"))
    assert [row["kind"] for row in stable["resources"]] == ["static_mesh"]
    assert (output_dir / "export_manifest.json.tmp").is_file()


def test_fbx_material_export_updates_existing_owner_in_place(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("MeshWithSharedMaterial")
    bpy.context.scene.collection.children.link(selected)
    obj = _mesh_object("SharedMesh", selected)
    material = bpy.data.materials.new("SharedMaterial")
    material.dagormat.shader_class = "rendinst_simple"
    material.dagormat.optional["roughness"] = 0.25
    obj.data.materials.append(material)

    common = tmp_path / "common"
    common.mkdir()
    prepared = prepare_blender_material_export(
        material, common, source_root=tmp_path)
    manifest = prepare_manifest_update(
        str(common), resources=[prepared.resource_row],
        exporter_version="0.4.0", source_root=str(tmp_path))
    stage_manifest(str(common), manifest)
    assert write_prepared_material(prepared, source_root=tmp_path)
    commit_staged_manifest(str(common))
    original_path = Path(prepared.payload_path)

    material.dagormat.optional["roughness"] = 0.75
    mesh_dir = tmp_path / "meshes"
    report = export_fbx_collection(
        selected, mesh_dir,
        export_materials=True,
        source_root=tmp_path,
    )

    assert Path(report["material_updates"][0]["path"]) == original_path
    assert not list(mesh_dir.glob("*.material"))
    payload = json.loads(original_path.read_text(encoding="utf-8"))
    assert payload["params"]["roughness"] == 0.75
    mesh_manifest = json.loads(
        (mesh_dir / "export_manifest.json").read_text(encoding="utf-8"))
    assert [row["kind"] for row in mesh_manifest["resources"]] == [
        "static_mesh"]


def test_fbx_retry_recovers_material_marker_and_forces_payload_replace(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("RecoveryMesh")
    bpy.context.scene.collection.children.link(selected)
    obj = _mesh_object("RecoveryObject", selected)
    material = bpy.data.materials.new("RecoveryMaterial")
    material.dagormat.shader_class = "rendinst_simple"
    material.dagormat.optional["roughness"] = 0.25
    obj.data.materials.append(material)
    output_dir = tmp_path / "sources"

    initial = export_fbx_collection(
        selected, output_dir,
        export_materials=True,
        source_root=tmp_path,
    )
    material_path = Path(initial["material_updates"][0]["path"])
    manifest_path = Path(initial["material_updates"][0]["manifest_path"])

    material.dagormat.optional["roughness"] = 0.75
    changed = prepare_blender_material_export(
        material,
        output_dir,
        source_root=tmp_path,
        target_payload_path=material_path,
        owning_manifest_path=manifest_path,
        existing_source=material_path.name,
    )
    pending = prepare_manifest_update(
        str(output_dir), resources=[changed.resource_row],
        exporter_version="0.4.0", source_root=str(tmp_path))
    stage_manifest(str(output_dir), pending)
    assert write_prepared_material(changed, source_root=tmp_path)
    abandon_staged_manifest(str(output_dir))
    assert (output_dir / "export_manifest.json.tmp").is_file()

    real_write = write_prepared_material
    force_values = []

    def record_force(prepared, **kwargs):
        force_values.append(kwargs.get("force", False))
        return real_write(prepared, **kwargs)

    monkeypatch.setattr(
        export_fbx_module, "write_prepared_material", record_force)
    recovered = export_fbx_collection(
        selected, output_dir,
        export_materials=True,
        source_root=tmp_path,
    )

    assert recovered["ok"]
    assert force_values == [True]
    assert recovered["material_updates"][0]["ok"] is True
    assert recovered["material_updates"][0]["written"] is True
    assert not (output_dir / "export_manifest.json.tmp").exists()
    payload = json.loads(material_path.read_text(encoding="utf-8"))
    assert payload["params"]["roughness"] == 0.75


def test_dry_run_prepares_manifest_entries_without_creating_output(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("DryResource")
    bpy.context.scene.collection.children.link(selected)
    _mesh_object("DryMesh", selected)
    output_dir = tmp_path / "not-created"

    report = export_fbx_collection(
        selected, output_dir, dry_run=True, source_root=tmp_path)

    assert report["ok"]
    assert not report["written"]
    assert not report["manifest_written"]
    assert report["objects_exported"] == 1
    assert report["resource_entry"]["source"].endswith(".mesh.fbx")
    assert not output_dir.exists()


def test_unchanged_mesh_skips_fbx_payload_but_commits_manifest(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("HashSkipMesh")
    bpy.context.scene.collection.children.link(selected)
    _mesh_object("HashSkipObject", selected)
    initial = export_fbx_collection(
        selected, tmp_path, source_root=tmp_path)
    payload = Path(initial["filepath"])
    original_bytes = payload.read_bytes()
    original_mtime = payload.stat().st_mtime_ns
    calls = []

    def unexpected_fbx(path):
        calls.append(path)
        Path(path).write_bytes(b"unexpected payload rewrite")

    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx", unexpected_fbx)
    repeated = export_fbx_collection(
        selected, tmp_path, source_root=tmp_path)

    assert repeated["written"] is False
    assert repeated["manifest_written"] is True
    assert calls == []
    assert payload.read_bytes() == original_bytes
    assert payload.stat().st_mtime_ns == original_mtime


def test_hash_skip_snapshot_race_blocks_before_marker(tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("HashSkipRaceMesh")
    bpy.context.scene.collection.children.link(selected)
    _mesh_object("HashSkipRaceObject", selected)
    initial = export_fbx_collection(
        selected, tmp_path, source_root=tmp_path)
    payload = Path(initial["filepath"])
    manifest_path = Path(initial["manifest_path"])
    real_stage = export_fbx_module.stage_manifest
    fbx_calls = []

    def foreign_writer_before_locked_guard(directory, document, **kwargs):
        payload.write_bytes(b"foreign writer payload")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        row = next(item for item in manifest["resources"]
                   if item["uid"] == selected["mh_uid"])
        row["content_hash"] = "xxh3:1111111111111111"
        manifest_path.write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        return real_stage(directory, document, **kwargs)

    def unexpected_fbx(path):
        fbx_calls.append(path)

    monkeypatch.setattr(
        export_fbx_module, "stage_manifest",
        foreign_writer_before_locked_guard)
    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx", unexpected_fbx)

    with pytest.raises(ValueError, match="MH_E_INVALID_EXPORT_MANIFEST"):
        export_fbx_collection(selected, tmp_path, source_root=tmp_path)

    assert fbx_calls == []
    assert not (tmp_path / "export_manifest.json.tmp").exists()
    assert payload.read_bytes() == b"foreign writer payload"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    row = next(item for item in manifest["resources"]
               if item["uid"] == selected["mh_uid"])
    assert row["content_hash"] == "xxh3:1111111111111111"


def test_metadata_only_mesh_update_skips_fbx_and_updates_manifest(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("MetadataOnlyMesh")
    bpy.context.scene.collection.children.link(selected)
    _mesh_object("MetadataOnlyObject", selected)
    initial = export_fbx_collection(
        selected, tmp_path, source_root=tmp_path)
    payload = Path(initial["filepath"])
    original_mtime = payload.stat().st_mtime_ns
    original_hash = initial["resource_entry"]["content_hash"]
    selected["mh_p_category"] = "architecture"
    calls = []

    def unexpected_fbx(path):
        calls.append(path)
        Path(path).write_bytes(b"unexpected metadata payload rewrite")

    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx", unexpected_fbx)
    updated = export_fbx_collection(
        selected, tmp_path, source_root=tmp_path)

    assert updated["resource_entry"]["content_hash"] == original_hash
    assert updated["written"] is False
    assert updated["manifest_written"] is True
    assert calls == []
    assert payload.stat().st_mtime_ns == original_mtime
    manifest = json.loads(
        Path(updated["manifest_path"]).read_text(encoding="utf-8"))
    row = next(item for item in manifest["resources"]
               if item["uid"] == selected["mh_uid"])
    assert row["properties"] == {"category": "architecture"}


def test_changed_geometry_rewrites_fbx_payload(tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("ChangedGeometryMesh")
    bpy.context.scene.collection.children.link(selected)
    obj = _mesh_object("ChangedGeometryObject", selected)
    initial = export_fbx_collection(
        selected, tmp_path, source_root=tmp_path)
    payload = Path(initial["filepath"])
    initial_hash = initial["resource_entry"]["content_hash"]
    obj.data.vertices[0].co.x += 0.25
    obj.data.update()
    calls = []

    def write_changed(path):
        calls.append(path)
        Path(path).write_bytes(b"changed geometry payload")

    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx", write_changed)
    updated = export_fbx_collection(
        selected, tmp_path, source_root=tmp_path)

    assert updated["resource_entry"]["content_hash"] != initial_hash
    assert updated["written"] is True
    assert updated["manifest_written"] is True
    assert calls == [str(payload) + ".tmp"]
    assert payload.read_bytes() == b"changed geometry payload"


def test_missing_fbx_payload_is_rebuilt_even_when_hash_matches(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("MissingPayloadMesh")
    bpy.context.scene.collection.children.link(selected)
    _mesh_object("MissingPayloadObject", selected)
    initial = export_fbx_collection(
        selected, tmp_path, source_root=tmp_path)
    payload = Path(initial["filepath"])
    payload.unlink()
    calls = []

    def rebuild(path):
        calls.append(path)
        Path(path).write_bytes(b"rebuilt missing payload")

    monkeypatch.setattr(export_fbx_module, "_export_selected_fbx", rebuild)
    rebuilt = export_fbx_collection(
        selected, tmp_path, source_root=tmp_path)

    assert rebuilt["resource_entry"]["content_hash"] == \
        initial["resource_entry"]["content_hash"]
    assert rebuilt["written"] is True
    assert rebuilt["manifest_written"] is True
    assert calls == [str(payload) + ".tmp"]
    assert payload.read_bytes() == b"rebuilt missing payload"


def test_static_mesh_recovery_forces_payload_rewrite_after_hash_reverts(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("RecoveryHashMesh")
    bpy.context.scene.collection.children.link(selected)
    obj = _mesh_object("RecoveryHashObject", selected)
    initial = export_fbx_collection(
        selected, tmp_path, source_root=tmp_path)
    payload = Path(initial["filepath"])
    original_coordinate = obj.data.vertices[0].co.x
    obj.data.vertices[0].co.x += 0.5
    obj.data.update()

    def fail_fbx(_path):
        raise RuntimeError("injected FBX payload failure")

    monkeypatch.setattr(export_fbx_module, "_export_selected_fbx", fail_fbx)
    with pytest.raises(RuntimeError, match="injected FBX payload failure"):
        export_fbx_collection(selected, tmp_path, source_root=tmp_path)
    marker = tmp_path / "export_manifest.json.tmp"
    assert marker.is_file()

    # Current content once again matches the stable manifest/payload. Recovery
    # must nevertheless replace FBX because the crash point is unknown.
    obj.data.vertices[0].co.x = original_coordinate
    obj.data.update()
    calls = []

    def force_recovery(path):
        calls.append(path)
        Path(path).write_bytes(b"forced recovery payload")

    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx", force_recovery)
    recovered = export_fbx_collection(
        selected, tmp_path, source_root=tmp_path)

    assert recovered["resource_entry"]["content_hash"] == \
        initial["resource_entry"]["content_hash"]
    assert recovered["written"] is True
    assert recovered["manifest_written"] is True
    assert calls == [str(payload) + ".tmp"]
    assert payload.read_bytes() == b"forced recovery payload"
    assert not marker.exists()


def test_mesh_hash_skip_still_updates_touched_materials(tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("MaterialTouchMesh")
    bpy.context.scene.collection.children.link(selected)
    obj = _mesh_object("MaterialTouchObject", selected)
    material = bpy.data.materials.new("TouchedMaterial")
    material.dagormat.shader_class = "rendinst_simple"
    material.dagormat.optional["roughness"] = 0.25
    obj.data.materials.append(material)
    initial = export_fbx_collection(
        selected, tmp_path, export_materials=True, source_root=tmp_path)
    material_path = Path(initial["material_updates"][0]["path"])
    material.dagormat.optional["roughness"] = 0.75
    calls = []

    def unexpected_fbx(path):
        calls.append(path)
        Path(path).write_bytes(b"unexpected geometry rewrite")

    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx", unexpected_fbx)
    updated = export_fbx_collection(
        selected, tmp_path, export_materials=True, source_root=tmp_path)

    assert updated["written"] is False
    assert updated["manifest_written"] is True
    assert calls == []
    assert updated["material_updates"][0]["written"] is True
    payload = json.loads(material_path.read_text(encoding="utf-8"))
    assert payload["params"]["roughness"] == 0.75


def test_export_materials_off_does_not_update_touched_material(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("MaterialOffMesh")
    bpy.context.scene.collection.children.link(selected)
    obj = _mesh_object("MaterialOffObject", selected)
    material = bpy.data.materials.new("UntouchedMaterial")
    material.dagormat.shader_class = "rendinst_simple"
    material.dagormat.optional["roughness"] = 0.25
    obj.data.materials.append(material)
    initial = export_fbx_collection(
        selected, tmp_path, export_materials=True, source_root=tmp_path)
    material_path = Path(initial["material_updates"][0]["path"])
    original_material = material_path.read_bytes()
    material.dagormat.optional["roughness"] = 0.75
    calls = []

    def unexpected_fbx(path):
        calls.append(path)
        Path(path).write_bytes(b"unexpected geometry rewrite")

    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx", unexpected_fbx)
    updated = export_fbx_collection(
        selected, tmp_path, export_materials=False, source_root=tmp_path)

    assert updated["written"] is False
    assert updated["manifest_written"] is True
    assert calls == []
    assert updated["material_updates"] == []
    assert updated["materials_exported"] == 0
    assert material_path.read_bytes() == original_material


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
        "resources": [],
        "shader_classes": ["rendinst_simple"],
    }), encoding="utf-8")

    report = export_fbx_collection(
        selected, tmp_path / "output", dry_run=True,
        registry_path=str(registry), source_root=tmp_path)

    assert report["ok"]
    assert [row["code"] for row in report["validation"]["warnings"]] == [
        "MH_W_UNKNOWN_SHADER_CLASS", "MH_W_MATERIAL_NOT_FOUND"]


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
        export_fbx_collection(
            selected, tmp_path, source_root=tmp_path)

    assert (tmp_path / "export_manifest.json.tmp").exists()
    assert not (tmp_path / "export_manifest.json").exists()
    assert not any(path.name.endswith(".mesh.fbx") for path in tmp_path.iterdir())
    assert not any(path.name.endswith(".mesh.fbx.tmp") for path in tmp_path.iterdir())
