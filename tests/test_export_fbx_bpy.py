"""Blender gates for the Source Protocol v4 FBX writer."""

import importlib
from pathlib import Path
import sys

import pytest

bpy = pytest.importorskip("bpy")
REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.validate import MHValidationError  # noqa: E402
from mh4blend.scene.export_fbx import export_fbx_collection  # noqa: E402

export_fbx_module = importlib.import_module("mh4blend.scene.export_fbx")


def _mesh_object(name, collection):
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    return obj


def _empty(name, collection):
    obj = bpy.data.objects.new(name, None)
    collection.objects.link(obj)
    return obj


def _material(name):
    return bpy.data.materials.new(name)


def _assign_material(obj, material):
    obj.data.materials.append(material)


def _collection(name="selected_resource"):
    collection = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(collection)
    return collection


def _build_joined(name="selected_resource"):
    root = _collection(name)
    child = bpy.data.collections.new("Child")
    root.children.link(child)
    direct = _mesh_object("Direct", root)
    nested = _mesh_object("Nested", child)
    return root, direct, nested


def _build_lods(base="garage"):
    root = _collection(base + ".lods")
    lod0 = bpy.data.collections.new(base + ".lod00")
    lod1 = bpy.data.collections.new(base + ".lod01")
    root.children.link(lod0)
    root.children.link(lod1)
    render0 = _mesh_object("Body", lod0)
    render1a = _mesh_object("BodyA", lod1)
    render1b = _mesh_object("BodyB_lod01", lod1)
    collision = _mesh_object("Body_cls_phys", lod0)
    ucx_collision = _mesh_object("UCX_Body", lod0)
    socket = _empty("SOCKET_Muzzle", lod0)
    ignored_collision = _mesh_object("UCX_Body_High", lod1)
    ignored_socket = _empty("SOCKET_High", lod1)
    return {
        "root": root,
        "render": (render0, render1a, render1b),
        "collision": collision,
        "ucx_collision": ucx_collision,
        "socket": socket,
        "ignored": (ignored_collision, ignored_socket),
    }


def _fbx_models(path):
    from io_scene_fbx import parse_fbx
    root, _version = parse_fbx.parse(str(path))
    objects = next(item for item in root.elems if item.id == b"Objects")
    result = {}
    for model in objects.elems:
        if model.id != b"Model":
            continue
        name = model.props[1].split(b"\x00", 1)[0].decode("utf-8")
        props = {}
        props70 = next(
            (item for item in model.elems if item.id == b"Properties70"), None)
        if props70 is not None:
            for item in props70.elems:
                if item.id == b"P" and len(item.props) >= 5:
                    props[item.props[0].decode("utf-8")] = item.props[4]
        result[name] = {"kind": model.props[2], "properties": props}
    return result


def _fbx_model_parents(path):
    from io_scene_fbx import parse_fbx
    root, _version = parse_fbx.parse(str(path))
    objects = next(item for item in root.elems if item.id == b"Objects")
    names = {model.props[0]: model.props[1].split(b"\x00", 1)[0].decode("utf-8")
             for model in objects.elems if model.id == b"Model"}
    connections = next(item for item in root.elems if item.id == b"Connections")
    parents = {name: None for name in names.values()}
    for link in connections.elems:
        if (link.id == b"C" and len(link.props) >= 3
                and link.props[0] == b"OO"):
            child_id, parent_id = link.props[1], link.props[2]
            if child_id in names and parent_id in names:
                parents[names[child_id]] = names[parent_id]
    return parents


def test_export_rewrites_plain_fbx_without_custom_properties(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, direct, nested = _build_joined()
    direct["mh_old_property"] = "artist value"
    first = export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    second = export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert Path(first["filepath"]).name == "selected_resource.mesh.fbx"
    assert first["written"] is second["written"] is True
    assert direct["mh_old_property"] == "artist value"
    models = _fbx_models(second["filepath"])
    assert {"Direct", "Nested"} <= set(models)
    assert all(not key.startswith("mh_")
               for row in models.values() for key in row["properties"])
    assert nested.name == "Nested"


def test_existing_file_is_replaced_but_directory_is_blocked(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, _direct, _nested = _build_joined("replace_me")
    target = tmp_path / "replace_me.mesh.fbx"
    target.write_bytes(b"old")
    export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert target.read_bytes() != b"old"
    target.unlink()
    target.mkdir()
    with pytest.raises(ValueError, match="exists as a directory"):
        export_fbx_collection(collection, tmp_path, source_root=tmp_path)


@pytest.mark.parametrize(
    "name", ["Garage", "garage a", "garage.a", "гараж"])
def test_export_rejects_noncanonical_logical_name(tmp_path, name):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, _direct, _nested = _build_joined(name)
    with pytest.raises(ValueError, match="MH_E_NON_ASCII_RESOURCE_NAME"):
        export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert not list(tmp_path.glob("*.mesh.fbx"))


def test_lod_mesh_names_are_temporary_and_classifiable(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    built = _build_lods()
    original_names = [obj.name for obj in built["render"]]
    report = export_fbx_collection(built["root"], tmp_path, source_root=tmp_path)
    models = _fbx_models(report["filepath"])
    assert report["lod_levels"] == [0, 1]
    assert {"Body_lod00", "BodyA_lod01", "BodyB_lod01"} <= set(models)
    assert "Body_cls_phys" in models
    assert "UCX_Body" in models
    assert "SOCKET_Muzzle" in models
    assert "UCX_Body_High" not in models
    assert "SOCKET_High" not in models
    assert [obj.name for obj in built["render"]] == original_names


def test_mismatched_existing_lod_suffix_fails_closed(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    built = _build_lods("mismatch")
    built["render"][0].name = "Body_lod01"
    with pytest.raises(ValueError, match="MH_E_INVALID_LOD_HIERARCHY"):
        export_fbx_collection(built["root"], tmp_path, source_root=tmp_path)


def test_lod_names_restore_when_exporter_raises(tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    built = _build_lods("atomic")
    original_names = [obj.name for obj in built["render"]]
    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx",
        lambda _path: (_ for _ in ()).throw(RuntimeError("synthetic failure")))
    with pytest.raises(RuntimeError, match="synthetic failure"):
        export_fbx_collection(built["root"], tmp_path, source_root=tmp_path)
    assert [obj.name for obj in built["render"]] == original_names
    assert not (tmp_path / "atomic.mesh.fbx").exists()
    assert not list(tmp_path.glob(".atomic.mesh.fbx.mh-tmp-*"))


def test_group_empty_hierarchy_is_transported(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, direct, nested = _build_joined("grouped_resource")
    group = _empty("GroupNode", collection)
    direct.parent = group
    nested.parent = group
    result = export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    models = _fbx_models(result["filepath"])
    parents = _fbx_model_parents(result["filepath"])
    assert models["GroupNode"]["kind"] == b"Null"
    assert parents["Direct"] == "GroupNode"
    assert parents["Nested"] == "GroupNode"


def test_lods_container_group_empty_is_transported(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    built = _build_lods("depot")
    group = _empty("Rig", built["root"].children[0])
    built["render"][0].parent = group

    result = export_fbx_collection(
        built["root"], tmp_path, source_root=tmp_path)
    models = _fbx_models(result["filepath"])
    parents = _fbx_model_parents(result["filepath"])
    assert models["Rig"]["kind"] == b"Null"
    assert parents["Body_lod00"] == "Rig"


def test_lod_slot_set_must_be_subset_of_lod0(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    built = _build_lods("slots")
    _assign_material(built["render"][0], _material("base"))
    _assign_material(built["render"][1], _material("only_higher"))
    with pytest.raises(MHValidationError) as exc:
        export_fbx_collection(built["root"], tmp_path, source_root=tmp_path)
    assert exc.value.code == "MH_E_LOD_SLOT_NOT_IN_BASE"


def test_centimeter_context_preserves_exact_mesh_datablock(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, direct, _nested = _build_joined("exact_restore")
    direct.data.vertices[0].co = (1.0e-7, 123456789.0, -0.333333333)
    direct.shape_key_add(name="Basis")
    key = direct.shape_key_add(name="Offset")
    key.data[0].co = (2.0e-7, 123456700.0, -0.666666667)

    original_mesh = direct.data
    vertex_coordinates = [vertex.co.copy() for vertex in original_mesh.vertices]
    shape_coordinates = {
        block.name: [point.co.copy() for point in block.data]
        for block in original_mesh.shape_keys.key_blocks
    }

    export_fbx_collection(collection, tmp_path, source_root=tmp_path)

    assert direct.data is original_mesh
    assert [vertex.co.copy() for vertex in original_mesh.vertices] == vertex_coordinates
    assert {
        block.name: [point.co.copy() for point in block.data]
        for block in original_mesh.shape_keys.key_blocks
    } == shape_coordinates


def test_parent_outside_resource_fails_closed(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, direct, _nested = _build_joined("escaping_resource")
    outsider = bpy.data.objects.new("Outsider", None)
    bpy.context.scene.collection.objects.link(outsider)
    direct.parent = outsider
    with pytest.raises(MHValidationError) as exc:
        export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert exc.value.code == "MH_E_PARENT_OUTSIDE_RESOURCE"


def test_shift_d_duplicate_exports_without_identity_warnings(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, direct, _nested = _build_joined("duplicated_resource")
    duplicate = direct.copy()
    duplicate.data = direct.data.copy()
    collection.objects.link(duplicate)
    result = export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert result["ok"] is True
    assert result["validation"]["warnings"] == []
    assert len([name for name in _fbx_models(result["filepath"])
                if name.startswith("Direct")]) == 2
