"""Pure semantic tests for the v2 reader-side source differ."""

from copy import deepcopy
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.diff import diff_source_snapshots  # noqa: E402
from mh4blend.core.fbx_passport import (  # noqa: E402
    descriptor_hash,
    make_fbx_passport,
)


MESH_UID = "10000000-0000-0000-0000-000000000001"
COMP_UID = "20000000-0000-0000-0000-000000000001"
NODE_A = "30000000-0000-0000-0000-000000000001"
NODE_B = "30000000-0000-0000-0000-000000000002"
MAT_UID = "40000000-0000-0000-0000-000000000001"


IDENTITY = {
    "translation_cm": [0.0, 0.0, 0.0],
    "rotation_quat": [0.0, 0.0, 0.0, 1.0],
    "scale": [1.0, 1.0, 1.0],
}


def node(uid=NODE_A, **overrides):
    value = {
        "node_uid": uid,
        "parent_uid": None,
        "kind": "mesh",
        "display_name": "node",
        "resource_uid": MESH_UID,
        "local_transform": deepcopy(IDENTITY),
        "properties": {},
    }
    value.update(overrides)
    return value


def composite_doc(*, name="assembly", properties=None, nodes=None):
    return {
        "schema": "mh.composite",
        "schema_version": 2,
        "uid": COMP_UID,
        "name": name,
        "properties": deepcopy(properties or {}),
        "nodes": deepcopy(nodes if nodes is not None else [node()]),
    }


def material_doc(*, name="m_wall", shader="rendinst_simple", params=None,
                 textures=None):
    return {
        "schema": "mh.material",
        "schema_version": 1,
        "uid": MAT_UID,
        "name": name,
        "shader_class": shader,
        "params": deepcopy(params or {}),
        "textures": deepcopy(textures or {}),
    }


def mesh_row(*, name="wall", path="meshes/wall.mesh.fbx",
             geometry="xxh3:aaaaaaaaaaaaaaaa", properties=None,
             slots=None, lod_levels=None, exporter="mh4blend test"):
    passport = make_fbx_passport(
        resource_uid=MESH_UID,
        name=name,
        lod_levels=lod_levels or [0],
        lod_policy="authored" if len(lod_levels or [0]) > 1 else "generated",
        geometry_hash=geometry,
        material_slots=slots or [],
        properties=properties or {},
        exporter=exporter,
    )
    return {
        "uid": MESH_UID,
        "kind": "static_mesh",
        "name": name,
        "source_path": path,
        "geometry_hash": geometry,
        "descriptor_hash": descriptor_hash(passport),
        "document": passport,
    }


def json_row(document, kind, path):
    return {
        "uid": document["uid"],
        "kind": kind,
        "name": document["name"],
        "source_path": path,
        "document": document,
    }


def snapshot(*rows):
    return {"resources": {row["uid"]: row for row in rows}}


def test_identical_source_snapshots_produce_empty_diff():
    old = snapshot(
        mesh_row(),
        json_row(composite_doc(), "composite", "cmp/assembly.composite"),
        json_row(material_doc(), "material", "mat/m_wall.material"),
    )
    report = diff_source_snapshots(old, deepcopy(old))
    assert report["resources"] == {}
    assert report["nodes"] == {}


def test_move_is_per_resource_and_independent_of_semantic_update():
    old = snapshot(mesh_row(path="old/wall.mesh.fbx"))
    moved_only = snapshot(mesh_row(path="new/wall.mesh.fbx"))
    report = diff_source_snapshots(old, moved_only)
    assert report["resources"] == {MESH_UID: ["MOVE"]}

    moved_and_changed = snapshot(mesh_row(
        path="new/wall.mesh.fbx", geometry="xxh3:bbbbbbbbbbbbbbbb"))
    report = diff_source_snapshots(old, moved_and_changed)
    assert report["resources"][MESH_UID] == ["UPDATE_GEOMETRY", "MOVE"]


def test_mesh_rename_geometry_and_descriptor_semantics_are_classified():
    old = snapshot(mesh_row())
    new = snapshot(mesh_row(
        name="wall_main",
        geometry="xxh3:bbbbbbbbbbbbbbbb",
        properties={"role": "structure"},
        slots=[{
            "slot_name": "surface",
            "material_uid": MAT_UID,
            "material_name_hint": "m_wall",
        }],
    ))
    report = diff_source_snapshots(old, new)
    assert report["resources"][MESH_UID] == [
        "RENAME", "UPDATE_GEOMETRY", "UPDATE_PROPERTIES"]


def test_exporter_only_descriptor_churn_is_not_asset_update():
    old = snapshot(mesh_row(exporter="mh4blend 1"))
    new = snapshot(mesh_row(exporter="mh4blend 2"))
    assert old["resources"][MESH_UID]["descriptor_hash"] != \
        new["resources"][MESH_UID]["descriptor_hash"]
    report = diff_source_snapshots(old, new)
    assert report["resources"] == {}


def test_material_uses_canonical_semantics_and_rename_is_separate():
    old = snapshot(json_row(
        material_doc(params={"roughness": 0.25}),
        "material", "mat/m_wall.material"))
    quantized_equal = snapshot(json_row(
        material_doc(params={"roughness": 0.2500004}),
        "material", "mat/m_wall.material"))
    assert diff_source_snapshots(old, quantized_equal)["resources"] == {}

    changed = snapshot(json_row(
        material_doc(name="m_wall_new", params={"roughness": 0.5}),
        "material", "mat/m_wall.material"))
    assert diff_source_snapshots(old, changed)["resources"][MAT_UID] == [
        "RENAME", "UPDATE_PROPERTIES"]


def test_composite_resource_properties_and_node_flags():
    old_doc = composite_doc(nodes=[node(), node(NODE_B)])
    changed_transform = deepcopy(IDENTITY)
    changed_transform["translation_cm"] = [100.0, 0.0, 0.0]
    new_doc = composite_doc(
        properties={"category": "vehicle"},
        nodes=[
            node(
                display_name="renamed",
                local_transform=changed_transform,
                parent_uid=NODE_B,
                resource_uid=COMP_UID,
                kind="composite_ref",
                properties={"role": "decal"},
            ),
            node(NODE_B),
        ],
    )
    old = snapshot(json_row(old_doc, "composite", "cmp/assembly.composite"))
    new = snapshot(json_row(new_doc, "composite", "cmp/assembly.composite"))
    report = diff_source_snapshots(old, new)
    assert report["resources"] == {COMP_UID: ["UPDATE_PROPERTIES"]}
    assert report["nodes"][COMP_UID][NODE_A] == [
        "RENAME", "UPDATE_TRANSFORM", "UPDATE_PROPERTIES", "REPARENT",
        "UPDATE_RESOURCE", "UPDATE_KIND"]


def test_composite_node_create_remove_and_decimal_spelling():
    old = snapshot(json_row(
        composite_doc(nodes=[node()]), "composite", "assembly.composite"))
    new = snapshot(json_row(
        composite_doc(nodes=[node(NODE_B)]),
        "composite", "assembly.composite"))
    report = diff_source_snapshots(old, new)
    assert report["nodes"][COMP_UID] == {
        NODE_A: ["REMOVE"], NODE_B: ["CREATE"]}

    equivalent = composite_doc(nodes=[node()])
    equivalent["nodes"][0]["local_transform"]["translation_cm"] = [0, 0, 0]
    equivalent["nodes"][0]["local_transform"]["scale"] = [1, 1.0, 1.00]
    report = diff_source_snapshots(old, snapshot(json_row(
        equivalent, "composite", "assembly.composite")))
    assert report["resources"] == {}
    assert report["nodes"] == {}


def test_create_remove_and_kind_change():
    old = snapshot(mesh_row())
    created = json_row(material_doc(), "material", "m_wall.material")
    new = snapshot(created)
    report = diff_source_snapshots(old, new)
    assert report["resources"] == {
        MAT_UID: ["CREATE"], MESH_UID: ["REMOVE"]}

    material_same_uid = material_doc(name="wall")
    material_same_uid["uid"] = MESH_UID
    changed_kind = snapshot(json_row(
        material_same_uid, "material", "wall.material"))
    report = diff_source_snapshots(old, changed_kind)
    assert report["resources"][MESH_UID] == ["UPDATE_KIND", "MOVE"]
