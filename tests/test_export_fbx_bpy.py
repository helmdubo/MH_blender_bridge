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
from mh4blend.core.validate import MHValidationError  # noqa: E402
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


def _build_lod_collection(
        base="sovmod_garage_shell_a_type_a", *, duplicate_suffix=""):
    root = bpy.data.collections.new(f"{base}.lods{duplicate_suffix}")
    lod0 = bpy.data.collections.new(f"{base}.lod00{duplicate_suffix}")
    lod1 = bpy.data.collections.new(f"{base}.lod01{duplicate_suffix}")
    bpy.context.scene.collection.children.link(root)
    root.children.link(lod0)
    root.children.link(lod1)
    object0 = _mesh_object("GarageLOD0Mesh", lod0)
    object1 = _mesh_object("GarageLOD1Mesh", lod1)
    return root, lod0, lod1, object0, object1


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


def test_dag4blend_lods_exports_one_combined_authored_payload_roundtrip(
        tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root, _lod0, _lod1, object0, object1 = _build_lod_collection()
    object0.location = (1.25, -2.0, 3.5)
    object1.location = (-4.0, 5.0, 0.25)
    object1.data.vertices[0].co.x += 0.5
    original0 = object0.matrix_basis.copy()
    original1 = object1.matrix_basis.copy()

    report = export_fbx_collection(root, tmp_path, source_root=tmp_path)

    assert report["ok"]
    assert report["objects_exported"] == 2
    primary = Path(report["filepath"])
    assert primary.name.startswith("sovmod_garage_shell_a_type_a__")
    assert primary.name.endswith(".mesh.fbx")
    assert primary.is_file()
    assert len(list(tmp_path.glob("*.mesh.fbx"))) == 1
    assert report["payload_updates"] == [{
        "level": 0,
        "filepath": str(primary),
        "written": True,
    }]
    manifest = json.loads(
        (tmp_path / "export_manifest.json").read_text(encoding="utf-8"))
    assert len(manifest["resources"]) == 1
    row = manifest["resources"][0]
    assert row["name"] == "sovmod_garage_shell_a_type_a"
    assert row["source"] == primary.name
    assert row["content_hash"] == report["resource_entry"]["content_hash"]
    assert row["lod_policy"] == "authored"
    assert "lods" not in row
    assert "mh_lod_level" not in object0
    assert "mh_lod_level" not in object1
    assert object0.matrix_basis == original0
    assert object1.matrix_basis == original1

    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=str(primary))
    imported0 = bpy.data.objects.get("GarageLOD0Mesh")
    imported1 = bpy.data.objects.get("GarageLOD1Mesh")
    assert imported0 is not None and imported0["mh_lod_level"] == 0
    assert imported1 is not None and imported1["mh_lod_level"] == 1


def test_dag4blend_duplicate_suffix_and_path_metadata_use_plain_base(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root, _lod0, _lod1, _object0, _object1 = _build_lod_collection(
        duplicate_suffix=".001")
    root["name"] = r"A:\assets\garage.lod00.dag"

    report = export_fbx_collection(
        root, tmp_path, dry_run=True, source_root=tmp_path)

    assert report["ok"]
    assert report["resource_entry"]["name"] == \
        "sovmod_garage_shell_a_type_a"
    assert report["resource_entry"]["source"].startswith(
        "sovmod_garage_shell_a_type_a__")
    assert report["resource_entry"]["lod_policy"] == "authored"
    assert "lods" not in report["resource_entry"]


def test_dag4blend_valid_custom_name_overrides_semantic_resource_name(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root, _lod0, _lod1, _object0, _object1 = _build_lod_collection()
    root["name"] = "Garage Shell"

    report = export_fbx_collection(
        root, tmp_path, dry_run=True, source_root=tmp_path)

    assert report["ok"]
    assert report["resource_entry"]["name"] == "Garage Shell"
    assert report["resource_entry"]["source"].startswith("garage_shell__")


def test_dag4blend_lods_allows_subset_and_exports_deduplicated_material_union(
        tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root, _lod0, _lod1, object0, object1 = _build_lod_collection()
    shared = bpy.data.materials.new("SharedLODMaterial")
    unique = bpy.data.materials.new("LOD1Material")
    shared.dagormat.shader_class = "rendinst_simple"
    unique.dagormat.shader_class = "rendinst_simple"
    object0.data.materials.append(shared)
    object0.data.materials.append(unique)
    object1.data.materials.append(shared)

    report = export_fbx_collection(
        root, tmp_path, dry_run=True, export_materials=True,
        source_root=tmp_path)

    assert report["ok"]
    assert report["materials_exported"] == 2
    assert {material.uid for material in report["materials"]} == {
        shared["mh_uid"], unique["mh_uid"]}
    assert len(report["material_entries"]) == 2


def test_dag4blend_lods_hash_skip_and_any_level_change_rewrites_combined_fbx(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root, _lod0, _lod1, _object0, object1 = _build_lod_collection()
    initial = export_fbx_collection(root, tmp_path, source_root=tmp_path)
    primary = Path(initial["filepath"])
    calls = []

    def write_fbx(path):
        calls.append(path)
        Path(path).write_bytes(b"updated authored level")

    monkeypatch.setattr(export_fbx_module, "_export_selected_fbx", write_fbx)
    unchanged = export_fbx_collection(root, tmp_path, source_root=tmp_path)
    assert unchanged["written"] is False
    assert unchanged["resource_entry"]["content_hash"] == \
        initial["resource_entry"]["content_hash"]
    assert calls == []

    object1.data.vertices[0].co.x += 0.25
    object1.data.update()
    changed = export_fbx_collection(root, tmp_path, source_root=tmp_path)
    assert changed["written"] is True
    assert changed["resource_entry"]["content_hash"] != \
        initial["resource_entry"]["content_hash"]
    assert calls == [str(primary) + ".tmp"]
    assert changed["payload_updates"] == [
        {"level": 0, "filepath": str(primary), "written": True},
    ]
    assert len(list(tmp_path.glob("*.mesh.fbx"))) == 1


def test_dag4blend_lods_repairs_missing_combined_payload(tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root, _lod0, _lod1, _object0, _object1 = _build_lod_collection()
    initial = export_fbx_collection(root, tmp_path, source_root=tmp_path)
    primary = Path(initial["filepath"])
    primary.unlink()
    calls = []

    def rebuild(path):
        calls.append(path)
        Path(path).write_bytes(b"repaired higher LOD")

    monkeypatch.setattr(export_fbx_module, "_export_selected_fbx", rebuild)
    repaired = export_fbx_collection(root, tmp_path, source_root=tmp_path)

    assert repaired["written"] is True
    assert calls == [str(primary) + ".tmp"]
    assert primary.read_bytes() == b"repaired higher LOD"


def test_dag4blend_lods_recovery_forces_combined_payload(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root, _lod0, _lod1, object0, object1 = _build_lod_collection()
    initial = export_fbx_collection(root, tmp_path, source_root=tmp_path)
    primary = Path(initial["filepath"])
    object0.data.vertices[0].co.x += 0.25
    object1.data.vertices[0].co.x += 0.5
    object0.data.update()
    object1.data.update()
    def fail_combined(_path):
        raise RuntimeError("injected combined LOD failure")

    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx", fail_combined)
    with pytest.raises(RuntimeError, match="injected combined LOD failure"):
        export_fbx_collection(root, tmp_path, source_root=tmp_path)
    marker = tmp_path / "export_manifest.json.tmp"
    assert marker.is_file()

    recovery_calls = []

    def recover(path):
        recovery_calls.append(path)
        Path(path).write_bytes(b"recovered " + Path(path).name.encode("utf-8"))

    monkeypatch.setattr(export_fbx_module, "_export_selected_fbx", recover)
    recovered = export_fbx_collection(root, tmp_path, source_root=tmp_path)

    assert recovered["written"] is True
    assert recovery_calls == [str(primary) + ".tmp"]
    assert [update["written"] for update in recovered["payload_updates"]] == [
        True]
    assert not marker.exists()


def test_dag4blend_lods_material_slot_conflict_blocks_export(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root, _lod0, _lod1, _object0, _object1 = _build_lod_collection()

    def conflicting_slots(_objects):
        raise MHValidationError(
            "MH_E_MATERIAL_SLOT_CONFLICT",
            ["aaaaaaaa-0000-0000-0000-000000000001",
             "bbbbbbbb-0000-0000-0000-000000000002"],
            "slot name 'Shared' maps to different materials")

    monkeypatch.setattr(
        export_fbx_module, "extract_materials_from_objects",
        conflicting_slots)
    with pytest.raises(MHValidationError, match="MH_E_MATERIAL_SLOT_CONFLICT"):
        export_fbx_collection(
            root, tmp_path, dry_run=True, source_root=tmp_path)


@pytest.mark.parametrize(
    "case", ["leaf", "mixed", "root_mesh", "missing_lod0", "lod_gap",
             "duplicate_level"])
def test_dag4blend_lods_rejects_malformed_or_leaf_selection(tmp_path, case):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    base = "bad_lod_asset"
    root = bpy.data.collections.new(base + ".lods")
    lod0 = bpy.data.collections.new(base + ".lod00")
    bpy.context.scene.collection.children.link(root)
    root.children.link(lod0)
    _mesh_object("LOD0Mesh", lod0)
    selected = root
    if case == "leaf":
        selected = lod0
    elif case == "mixed":
        helper = bpy.data.collections.new("not_a_lod")
        root.children.link(helper)
    elif case == "root_mesh":
        _mesh_object("MisplacedRootMesh", root)
    elif case == "missing_lod0":
        root.children.unlink(lod0)
        lod1 = bpy.data.collections.new(base + ".lod01")
        root.children.link(lod1)
        _mesh_object("LOD1Mesh", lod1)
    elif case == "lod_gap":
        lod2 = bpy.data.collections.new(base + ".lod02")
        root.children.link(lod2)
        _mesh_object("LOD2Mesh", lod2)
    elif case == "duplicate_level":
        duplicate = bpy.data.collections.new(base + ".lod00.001")
        root.children.link(duplicate)
        _mesh_object("DuplicateLOD0Mesh", duplicate)

    expected_code = (
        "MH_E_LOD_LEVELS_SPARSE"
        if case in {"missing_lod0", "lod_gap"}
        else "MH_E_INVALID_LOD_HIERARCHY"
    )
    with pytest.raises(ValueError, match=expected_code):
        export_fbx_collection(
            selected, tmp_path, dry_run=True, source_root=tmp_path)


def test_dag4blend_lod_slot_must_exist_in_lod0(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root, _lod0, _lod1, object0, object1 = _build_lod_collection()
    base_material = bpy.data.materials.new("BaseOnlySlot")
    higher_material = bpy.data.materials.new("HigherOnlySlot")
    base_material.dagormat.shader_class = "rendinst_simple"
    higher_material.dagormat.shader_class = "rendinst_simple"
    object0.data.materials.append(base_material)
    object1.data.materials.append(higher_material)

    with pytest.raises(ValueError, match="MH_E_LOD_SLOT_NOT_IN_BASE"):
        export_fbx_collection(
            root, tmp_path, dry_run=True, source_root=tmp_path)


def test_dag4blend_higher_lod_aux_mesh_is_ignored_with_warning(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root, _lod0, lod1, _object0, _object1 = _build_lod_collection()
    _mesh_object("UCX_GarageCollision", lod1)

    report = export_fbx_collection(
        root, tmp_path, dry_run=True, source_root=tmp_path)

    assert report["ok"]
    assert report["objects_exported"] == 2
    warning = next(
        row for row in report["validation"]["warnings"]
        if row["code"] == "MH_W_LOD_AUX_NODE_IGNORED")
    assert "UCX_GarageCollision" in warning["message"]


def test_dag4blend_lod0_aux_nodes_export_but_do_not_change_geometry_hash(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root, lod0, _lod1, _object0, _object1 = _build_lod_collection()
    collision = _mesh_object("UCX_GarageCollision", lod0)
    socket = bpy.data.objects.new("SOCKET_Roof", None)
    lod0.objects.link(socket)
    seen = []

    def inspect_and_write(path):
        seen.append((
            {obj.name for obj in bpy.context.selected_objects},
            collision["mh_lod_level"],
            socket["mh_lod_level"],
        ))
        Path(path).write_bytes(b"combined geometry and aux")

    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx", inspect_and_write)
    with_aux = export_fbx_collection(root, tmp_path, source_root=tmp_path)
    hash_with_aux = with_aux["resource_entry"]["content_hash"]

    lod0.objects.unlink(collision)
    lod0.objects.unlink(socket)
    without_aux = export_fbx_collection(
        root, tmp_path, dry_run=True, source_root=tmp_path)

    assert seen == [({
        "GarageLOD0Mesh", "GarageLOD1Mesh",
        "UCX_GarageCollision", "SOCKET_Roof",
    }, 0, 0)]
    assert with_aux["objects_exported"] == 4
    assert hash_with_aux == without_aux["resource_entry"]["content_hash"]
    assert "mh_lod_level" not in collision
    assert "mh_lod_level" not in socket


def test_dag4blend_temporary_lod_properties_restore_exactly(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root, _lod0, _lod1, object0, object1 = _build_lod_collection()
    object0["mh_lod_level"] = "artist-authored-value"
    seen = []

    def inspect_and_write(path):
        seen.append((object0["mh_lod_level"], object1["mh_lod_level"]))
        Path(path).write_bytes(b"combined LOD payload")

    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx", inspect_and_write)
    export_fbx_collection(root, tmp_path, source_root=tmp_path)

    assert seen == [(0, 1)]
    assert object0["mh_lod_level"] == "artist-authored-value"
    assert "mh_lod_level" not in object1


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


def test_metadata_only_mesh_update_rewrites_fbx_and_updates_manifest(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("MetadataOnlyMesh")
    bpy.context.scene.collection.children.link(selected)
    _mesh_object("MetadataOnlyObject", selected)
    initial = export_fbx_collection(
        selected, tmp_path, source_root=tmp_path)
    payload = Path(initial["filepath"])
    original_hash = initial["resource_entry"]["content_hash"]
    selected["mh_p_category"] = "architecture"
    calls = []

    def rewrite_metadata(path):
        calls.append(path)
        Path(path).write_bytes(b"metadata-carrying payload rewrite")

    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx", rewrite_metadata)
    updated = export_fbx_collection(
        selected, tmp_path, source_root=tmp_path)

    assert updated["resource_entry"]["content_hash"] == original_hash
    assert updated["written"] is True
    assert updated["manifest_written"] is True
    assert calls == [str(payload) + ".tmp"]
    assert payload.read_bytes() == b"metadata-carrying payload rewrite"
    manifest = json.loads(
        Path(updated["manifest_path"]).read_text(encoding="utf-8"))
    row = next(item for item in manifest["resources"]
               if item["uid"] == selected["mh_uid"])
    assert row["properties"] == {"category": "architecture"}


@pytest.mark.parametrize("mutation", ["material_uid", "slot_name"])
def test_material_slot_descriptor_mutation_rewrites_fbx(
        tmp_path, monkeypatch, mutation):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("MaterialDescriptorMesh")
    bpy.context.scene.collection.children.link(selected)
    obj = _mesh_object("MaterialDescriptorObject", selected)
    material = bpy.data.materials.new("DescriptorMaterial")
    material.dagormat.shader_class = "rendinst_simple"
    obj.data.materials.append(material)
    initial = export_fbx_collection(
        selected, tmp_path, source_root=tmp_path)
    payload = Path(initial["filepath"])
    initial_hash = initial["resource_entry"]["content_hash"]

    if mutation == "material_uid":
        material["mh_uid"] = "11111111-2222-4333-8444-555555555555"
    else:
        material.name = "RenamedDescriptorMaterial"
    calls = []

    def rewrite_metadata(path):
        calls.append(path)
        Path(path).write_bytes(mutation.encode("ascii"))

    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx", rewrite_metadata)
    updated = export_fbx_collection(
        selected, tmp_path, source_root=tmp_path)

    if mutation == "material_uid":
        assert updated["resource_entry"]["content_hash"] == initial_hash
    else:
        assert updated["resource_entry"]["content_hash"] != initial_hash
    assert updated["written"] is True
    assert calls == [str(payload) + ".tmp"]
    assert updated["resource_entry"]["material_slots"] != \
        initial["resource_entry"]["material_slots"]


def test_resource_rename_rewrites_existing_payload_in_place(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("OriginalResourceName")
    bpy.context.scene.collection.children.link(selected)
    _mesh_object("RenameDescriptorObject", selected)
    owner_dir = tmp_path / "original_owner"
    initial = export_fbx_collection(
        selected, owner_dir, source_root=tmp_path)
    original_payload = Path(initial["filepath"])
    original_manifest = Path(initial["manifest_path"])
    requested_dir = tmp_path / "artist_selected_another_folder"
    selected.name = "RenamedResource"
    calls = []

    def rewrite_metadata(path):
        calls.append(path)
        Path(path).write_bytes(b"renamed descriptor")

    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx", rewrite_metadata)
    updated = export_fbx_collection(
        selected, requested_dir, source_root=tmp_path)

    assert updated["written"] is True
    assert Path(updated["filepath"]) == original_payload
    assert Path(updated["manifest_path"]) == original_manifest
    assert updated["resource_entry"]["source"] == original_payload.name
    assert updated["payload_updates"][0]["filepath"] == str(original_payload)
    assert calls == [str(original_payload) + ".tmp"]
    assert not requested_dir.exists()
    manifest = json.loads(original_manifest.read_text(encoding="utf-8"))
    row = next(item for item in manifest["resources"]
               if item["uid"] == selected["mh_uid"])
    assert row["name"] == "RenamedResource"
    assert row["source"] == original_payload.name


def test_explicit_v1_descriptor_defaults_still_hash_skip(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("ExplicitDefaultsMesh")
    bpy.context.scene.collection.children.link(selected)
    _mesh_object("ExplicitDefaultsObject", selected)
    initial = export_fbx_collection(
        selected, tmp_path, source_root=tmp_path)
    manifest_path = Path(initial["manifest_path"])
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    row = next(item for item in manifest["resources"]
               if item["uid"] == selected["mh_uid"])
    row["material_slots"] = []
    row["properties"] = {}
    row["lod_policy"] = "generated"
    manifest_path.write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    calls = []
    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx", calls.append)

    updated = export_fbx_collection(
        selected, tmp_path, source_root=tmp_path)

    assert updated["written"] is False
    assert calls == []


def test_existing_lods_row_is_deprecated_even_when_empty(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    selected = bpy.data.collections.new("DeprecatedLodRows")
    bpy.context.scene.collection.children.link(selected)
    _mesh_object("DeprecatedLodRowsObject", selected)
    initial = export_fbx_collection(
        selected, tmp_path, source_root=tmp_path)
    manifest_path = Path(initial["manifest_path"])
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["resources"][0]["lods"] = []
    manifest_path.write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    with pytest.raises(ValueError, match="MH_E_DEPRECATED_LOD_ROWS"):
        export_fbx_collection(selected, tmp_path, source_root=tmp_path)

    assert not (tmp_path / "export_manifest.json.tmp").exists()


def test_lod_policy_descriptor_mutation_rewrites_combined_fbx(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root, _lod0, _lod1, _object0, _object1 = _build_lod_collection()
    initial = export_fbx_collection(root, tmp_path, source_root=tmp_path)
    primary = Path(initial["filepath"])
    manifest_path = Path(initial["manifest_path"])
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["resources"][0]["lod_policy"] = "nanite"
    manifest_path.write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    calls = []

    def rewrite_metadata(path):
        calls.append(path)
        Path(path).write_bytes(b"authored policy restored")

    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx", rewrite_metadata)
    updated = export_fbx_collection(root, tmp_path, source_root=tmp_path)

    assert updated["written"] is True
    assert calls == [str(primary) + ".tmp"]
    assert updated["payload_updates"] == [
        {"level": 0, "filepath": str(primary), "written": True},
    ]


def test_declared_lod_set_mutation_rewrites_combined_fbx(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root, _lod0, _lod1, _object0, _object1 = _build_lod_collection()
    initial = export_fbx_collection(root, tmp_path, source_root=tmp_path)
    primary = Path(initial["filepath"])
    base = "sovmod_garage_shell_a_type_a"
    lod2 = bpy.data.collections.new(base + ".lod02")
    root.children.link(lod2)
    _mesh_object("GarageLOD2Mesh", lod2)
    calls = []

    def write_payload(path):
        calls.append(path)
        Path(path).write_bytes(b"updated descriptor or new LOD")

    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx", write_payload)
    updated = export_fbx_collection(root, tmp_path, source_root=tmp_path)

    assert updated["written"] is True
    assert calls == [str(primary) + ".tmp"]
    assert updated["payload_updates"] == [
        {"level": 0, "filepath": str(primary), "written": True},
    ]


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
