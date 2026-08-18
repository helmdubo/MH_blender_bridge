"""Blender gates for the isolated one-shot v1 -> v2 source migration."""

from __future__ import annotations

import importlib
import json
from pathlib import Path
import sys

import pytest

bpy = pytest.importorskip("bpy")

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.fbx_passport import read_fbx_passport  # noqa: E402

migration = importlib.import_module("mh4blend.scene.migrate_sources_v2")


MESH_UID = "10000000-0000-4000-8000-000000000001"
COMPOSITE_UID = "20000000-0000-4000-8000-000000000002"
MATERIAL_UID = "30000000-0000-4000-8000-000000000003"


def _row(uid, kind, name, source, number, **extra):
    return {
        "uid": uid,
        "kind": kind,
        "name": name,
        "source": source,
        "content_hash": f"xxh3:{number:016x}",
        **extra,
    }


def _manifest(root, rows):
    path = root / "export_manifest.json"
    path.write_text(json.dumps({
        "schema": "mh.export_manifest",
        "schema_version": 1,
        "exporter_version": "0.3.0",
        "resources": sorted(rows, key=lambda row: row["uid"]),
    }, indent=2) + "\n", encoding="utf-8")
    return path


def _write_json(path, document):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(document, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")


def _mesh_object(name, collection):
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata(
        [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)],
        [], [(0, 1, 2)])
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    return obj


def _mesh_collection(name="Mesh Asset", *, lods=False):
    collection_name = name + ".lods" if lods else name
    collection = bpy.data.collections.new(collection_name)
    collection["mh_uid"] = MESH_UID
    bpy.context.scene.collection.children.link(collection)
    if lods:
        for level in (0, 1):
            child = bpy.data.collections.new(f"{name}.lod{level:02d}")
            collection.children.link(child)
            _mesh_object(f"BodyLOD{level}", child)
    else:
        _mesh_object("Body", collection)
    return collection


def _tree_bytes(root):
    return {
        path.relative_to(root).as_posix(): path.read_bytes()
        for path in root.rglob("*") if path.is_file()
    }


def _legacy_material():
    return {
        "schema": "mh.material",
        "schema_version": 1,
        "uid": MATERIAL_UID,
        "name": "Surface Mat",
        "shader_class": "rendinst_simple",
        "params": {},
        "textures": {},
    }


def _legacy_composite():
    return {
        "schema": "mh.composite",
        "schema_version": 1,
        "uid": COMPOSITE_UID,
        "name": "Garage Composite",
        "nodes": [],
    }


def test_success_migrates_all_payloads_then_removes_manifest(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    _mesh_collection(lods=True)
    old_mesh = tmp_path / "meshes/mesh_asset__10000000.mesh.fbx"
    old_lod = tmp_path / "meshes/mesh_asset__10000000.lod1.mesh.fbx"
    old_composite = tmp_path / "composites/garage_composite__20000000.composite"
    old_material = tmp_path / "materials/surface_mat__30000000.material"
    old_mesh.parent.mkdir(parents=True)
    old_mesh.write_bytes(b"legacy fbx bytes")
    old_lod.write_bytes(b"legacy lod fbx bytes")
    _write_json(old_composite, _legacy_composite())
    _write_json(old_material, _legacy_material())
    manifest = _manifest(tmp_path, [
        _row(MESH_UID, "static_mesh", "Mesh Asset",
             "meshes/mesh_asset__10000000.mesh.fbx", 1,
             lod_policy="authored", lods=[{
                 "level": 1,
                 "source": "meshes/mesh_asset__10000000.lod1.mesh.fbx",
                 "content_hash": "xxh3:0000000000000004",
             }]),
        _row(COMPOSITE_UID, "composite", "Garage Composite",
             "composites/garage_composite__20000000.composite", 2,
             properties={"role": "garage"}),
        _row(MATERIAL_UID, "material", "Surface Mat",
             "materials/surface_mat__30000000.material", 3),
    ])

    receipt = migration.migrate_sources_v2(tmp_path)

    assert receipt["ok"] is True, receipt
    assert receipt["failed"] == []
    assert {row["uid"] for row in receipt["migrated"]} == {
        MESH_UID, COMPOSITE_UID, MATERIAL_UID}
    assert len(receipt["renamed"]) == 3
    assert not manifest.exists()
    assert not old_mesh.exists()
    assert not old_lod.exists()
    assert not old_composite.exists()
    assert not old_material.exists()

    mesh = tmp_path / "meshes/mesh_asset.mesh.fbx"
    composite = tmp_path / "composites/garage_composite.composite"
    material = tmp_path / "materials/surface_mat.material"
    mesh_passport = read_fbx_passport(mesh).document
    assert mesh_passport["resource_uid"] == MESH_UID
    assert mesh_passport["lod_levels"] == [0, 1]
    composite_document = json.loads(composite.read_text(encoding="utf-8"))
    assert composite_document["schema_version"] == 2
    assert composite_document["uid"] == COMPOSITE_UID
    assert composite_document["properties"] == {"role": "garage"}
    assert json.loads(material.read_text(encoding="utf-8")) == _legacy_material()


def test_clean_target_collision_blocks_entire_root_without_writes(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    old = tmp_path / "materials/surface_mat__30000000.material"
    clean = tmp_path / "materials/surface_mat.material"
    _write_json(old, _legacy_material())
    clean.write_bytes(b"foreign clean payload")
    _manifest(tmp_path, [
        _row(MATERIAL_UID, "material", "Surface Mat",
             "materials/surface_mat__30000000.material", 1),
    ])
    before = _tree_bytes(tmp_path)

    receipt = migration.migrate_sources_v2(tmp_path)

    assert receipt["ok"] is False
    assert receipt["blocked"] is True
    assert receipt["migrated"] == []
    assert receipt["failed"][0]["code"] == "MH_E_NAME_COLLISION_DIFFERENT_UID"
    assert receipt["conflicted"][0]["code"] == \
        "MH_E_NAME_COLLISION_DIFFERENT_UID"
    assert _tree_bytes(tmp_path) == before


def test_missing_mesh_collection_preserves_legacy_manifest_and_payload(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    old = tmp_path / "meshes/mesh_asset__10000000.mesh.fbx"
    old.parent.mkdir(parents=True)
    old.write_bytes(b"legacy fbx")
    manifest = _manifest(tmp_path, [
        _row(MESH_UID, "static_mesh", "Mesh Asset",
             "meshes/mesh_asset__10000000.mesh.fbx", 1),
    ])
    before = _tree_bytes(tmp_path)

    receipt = migration.migrate_sources_v2(tmp_path)

    assert receipt["ok"] is False
    assert receipt["blocked"] is True
    assert receipt["failed"][0]["code"] == \
        "MH_E_MIGRATION_MESH_COLLECTION_NOT_FOUND"
    assert manifest.exists() and old.exists()
    assert _tree_bytes(tmp_path) == before


def test_mesh_descriptor_mismatch_blocks_before_filesystem_writes(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    _mesh_collection(name="Mesh Asset")
    old = tmp_path / "meshes/other_name__10000000.mesh.fbx"
    old.parent.mkdir(parents=True)
    old.write_bytes(b"legacy fbx")
    _manifest(tmp_path, [
        _row(MESH_UID, "static_mesh", "Other Name",
             "meshes/other_name__10000000.mesh.fbx", 1,
             lod_policy="generated"),
    ])
    before = _tree_bytes(tmp_path)

    receipt = migration.migrate_sources_v2(tmp_path)

    assert receipt["blocked"] is True
    assert receipt["failed"][0]["code"] == \
        "MH_E_V1_MIGRATION_IDENTITY_MISMATCH"
    assert "name" in receipt["failed"][0]["message"]
    assert _tree_bytes(tmp_path) == before


def test_duplicate_uid_across_manifests_blocks_entire_root(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    first_dir = tmp_path / "a"
    second_dir = tmp_path / "b"
    first_payload = first_dir / "surface_a__30000000.material"
    second_payload = second_dir / "surface_b__30000000.material"
    first = _legacy_material()
    first["name"] = "Surface A"
    second = _legacy_material()
    second["name"] = "Surface B"
    _write_json(first_payload, first)
    _write_json(second_payload, second)
    _manifest(first_dir, [
        _row(MATERIAL_UID, "material", "Surface A",
             "surface_a__30000000.material", 1),
    ])
    _manifest(second_dir, [
        _row(MATERIAL_UID, "material", "Surface B",
             "surface_b__30000000.material", 2),
    ])
    before = _tree_bytes(tmp_path)

    receipt = migration.migrate_sources_v2(tmp_path)

    assert receipt["blocked"] is True
    assert any(row["code"] == "MH_E_AMBIGUOUS_RESOURCE_OWNER"
               for row in receipt["failed"])
    assert any(row["code"] == "MH_E_AMBIGUOUS_RESOURCE_OWNER"
               for row in receipt["conflicted"])
    assert receipt["migrated"] == []
    assert _tree_bytes(tmp_path) == before


def test_pending_marker_anywhere_under_root_blocks_without_writes(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    old = tmp_path / "materials/surface_mat__30000000.material"
    _write_json(old, _legacy_material())
    _manifest(tmp_path, [
        _row(MATERIAL_UID, "material", "Surface Mat",
             "materials/surface_mat__30000000.material", 1),
    ])
    marker = tmp_path / "another_owner/export_manifest.json.tmp"
    marker.parent.mkdir()
    marker.write_bytes(b"pending")
    before = _tree_bytes(tmp_path)

    receipt = migration.migrate_sources_v2(tmp_path)

    assert receipt["ok"] is False
    assert receipt["blocked"] is True
    assert receipt["failed"][0]["code"] == "MH_E_PENDING_EXPORT_MARKER"
    assert _tree_bytes(tmp_path) == before


def test_dry_run_returns_complete_receipt_without_writes(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    old = tmp_path / "materials/surface_mat__30000000.material"
    _write_json(old, _legacy_material())
    manifest = _manifest(tmp_path, [
        _row(MATERIAL_UID, "material", "Surface Mat",
             "materials/surface_mat__30000000.material", 1),
    ])
    before = _tree_bytes(tmp_path)

    receipt = migration.migrate_sources_v2(tmp_path, dry_run=True)

    assert receipt["ok"] is True
    assert receipt["dry_run"] is True
    assert receipt["migrated"][0]["status"] == "would_migrate"
    assert receipt["manifests_removed"] == [{
        "path": str(manifest.absolute()), "status": "would_remove"}]
    assert _tree_bytes(tmp_path) == before


def test_mesh_dry_run_restores_lazy_node_and_data_uids(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = _mesh_collection()
    obj = next(iter(collection.objects))
    old = tmp_path / "meshes/mesh_asset__10000000.mesh.fbx"
    old.parent.mkdir(parents=True)
    old.write_bytes(b"legacy fbx")
    _manifest(tmp_path, [
        _row(MESH_UID, "static_mesh", "Mesh Asset",
             "meshes/mesh_asset__10000000.mesh.fbx", 1),
    ])
    before = _tree_bytes(tmp_path)
    assert "mh_uid" not in obj and "mh_uid" not in obj.data

    receipt = migration.migrate_sources_v2(tmp_path, dry_run=True)

    assert receipt["ok"] is True, receipt
    assert "mh_uid" not in obj and "mh_uid" not in obj.data
    assert _tree_bytes(tmp_path) == before
