"""End-to-end standalone source workflow in real Blender."""

import json
import os
from pathlib import Path
import sys

import pytest

bpy = pytest.importorskip("bpy")


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "addon"))

from mh4blend.scene.export_composite import (  # noqa: E402
    export_composite_collection,
)
from mh4blend.scene.export_fbx import export_fbx_collection  # noqa: E402
from mh4blend.scene.import_composite import import_composite_file  # noqa: E402


def _mesh_collection(scene):
    collection = bpy.data.collections.new("SM_Source")
    scene.collection.children.link(collection)
    mesh = bpy.data.meshes.new("SourceMesh")
    mesh.from_pydata(
        [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)],
        [], [(0, 1, 2)])
    obj = bpy.data.objects.new("SourceMesh", mesh)
    collection.objects.link(obj)
    return collection


def _instance_node(owner, name, target, location):
    obj = bpy.data.objects.new(name, None)
    obj.instance_type = "COLLECTION"
    obj.instance_collection = target
    obj.location = location
    owner.objects.link(obj)
    return obj


def _collection_by_uid(uid):
    return next(collection for collection in bpy.data.collections
                if collection.get("mh_uid") == uid)


def test_export_in_any_order_then_dependency_import_and_roundtrip(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    mesh = _mesh_collection(scene)
    child = bpy.data.collections.new("CMP_Child")
    parent = bpy.data.collections.new("CMP_Parent")
    scene.collection.children.link(child)
    scene.collection.children.link(parent)
    child_node = _instance_node(child, "MeshPlacement", mesh, (1, 2, 3))
    child_node["mh_p_role"] = "window"
    _instance_node(parent, "ChildPlacement", child, (-4, 0.5, 2))

    output = tmp_path / "sources"
    # Deliberately export the parent before either dependency.
    parent_report = export_composite_collection(parent, str(output))
    mesh_report = export_fbx_collection(mesh, str(output))
    child_report = export_composite_collection(child, str(output))
    assert parent_report["ok"] and mesh_report["ok"] and child_report["ok"]
    parent_source = parent_report["path"]
    parent_uid = parent_report["uid"]
    child_uid = child_report["uid"]
    mesh_uid = mesh_report["resource_entry"]["uid"]
    original_parent = json.loads(Path(parent_source).read_text(encoding="utf-8"))

    bpy.ops.wm.read_factory_settings(use_empty=True)
    imported = import_composite_file(parent_source)

    assert imported["ok"]
    assert set(imported["imported_composites"]) == {parent_uid, child_uid}
    assert imported["imported_meshes"] == [mesh_uid]
    assert imported["placeholders"] == []
    imported_parent = _collection_by_uid(parent_uid)
    imported_child = _collection_by_uid(child_uid)
    imported_mesh = _collection_by_uid(mesh_uid)
    parent_instance = next(iter(imported_parent.objects))
    child_instance = next(iter(imported_child.objects))
    assert parent_instance.instance_collection == imported_child
    assert child_instance.instance_collection == imported_mesh
    assert child_instance["mh_p_role"] == "window"
    assert any(obj.type == "MESH" for obj in imported_mesh.objects)

    roundtrip_dir = tmp_path / "roundtrip"
    roundtrip = export_composite_collection(
        imported_parent, str(roundtrip_dir))
    roundtrip_doc = json.loads(
        Path(roundtrip["path"]).read_text(encoding="utf-8"))
    assert roundtrip_doc == original_parent
