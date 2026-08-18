import json
import os
from pathlib import Path
import sys

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.scene.source_manifest import (  # noqa: E402
    MANIFEST_NAME,
    PENDING_MANIFEST_NAME,
    prepare_manifest_update,
    validate_export_manifest,
)
from tools.migrate_per_file_lods import (  # noqa: E402
    MigrationError,
    NEXT_ACTION,
    backup_path_for,
    migrate_manifest,
    restore_manifest,
)


MESH_UID = "10000000-0000-0000-0000-000000000001"
MATERIAL_UID = "30000000-0000-0000-0000-000000000003"


def legacy_mesh(*, lods=None):
    return {
        "uid": MESH_UID,
        "kind": "static_mesh",
        "name": "wall_a",
        "source": "mesh/wall_a__10000000.mesh.fbx",
        "content_hash": "xxh3:0000000000000001",
        "material_slots": [],
        "properties": {"artist": "owner"},
        "lod_policy": "authored",
        "lods": [{
            "level": 1,
            "source": "mesh/wall_a__10000000.lod1.mesh.fbx",
            "content_hash": "xxh3:0000000000000002",
        }] if lods is None else lods,
    }


def material_row():
    return {
        "uid": MATERIAL_UID,
        "kind": "material",
        "name": "wall_material",
        "source": "materials/wall_material__30000000.material",
        "content_hash": "xxh3:0000000000000003",
    }


def document(mesh=None):
    rows = [mesh or legacy_mesh(), material_row()]
    return {
        "schema": "mh.export_manifest",
        "schema_version": 1,
        "exporter_version": "0.4.1",
        "resources": sorted(rows, key=lambda row: row["uid"]),
    }


def write_case(tmp_path, *, mesh=None, payload_state="files"):
    owner = tmp_path / "library"
    owner.mkdir()
    manifest = owner / MANIFEST_NAME
    original = (json.dumps(
        document(mesh), indent=3, ensure_ascii=False) + "\n\n").encode("utf-8")
    manifest.write_bytes(original)
    primary = owner / "mesh" / "wall_a__10000000.mesh.fbx"
    lod1 = owner / "mesh" / "wall_a__10000000.lod1.mesh.fbx"
    if payload_state != "missing":
        primary.parent.mkdir()
        primary.write_bytes(b"primary")
        if payload_state == "nonfile":
            lod1.mkdir()
        else:
            lod1.write_bytes(b"lod1")
    return manifest, original, primary, lod1


def test_success_preserves_exact_backup_and_prepares_strict_reexport(tmp_path):
    manifest, original, primary, lod1 = write_case(tmp_path)
    receipt = migrate_manifest(manifest, MESH_UID, source_root=tmp_path)

    backup = backup_path_for(manifest, MESH_UID)
    assert backup.read_bytes() == original
    active = json.loads(manifest.read_text(encoding="utf-8"))
    assert active["resources"] == [material_row()]
    assert validate_export_manifest(active) == active
    # The unrelated row retains the original indent=3 byte representation;
    # migration does not canonicalize/re-encode resources it does not own.
    assert b'         "name": "wall_material"' in manifest.read_bytes()
    assert primary.read_bytes() == b"primary"
    assert lod1.read_bytes() == b"lod1"
    assert receipt["backup_path"] == str(backup)
    assert receipt["primary_payload"] == str(primary.absolute())
    assert receipt["lod_payloads"] == [{
        "level": 1, "path": str(lod1.absolute())}]
    assert receipt["payloads_deleted"] is False
    assert receipt["next_action"] == NEXT_ACTION

    # UID is deliberately unowned, so the ordinary writer can immediately
    # reserve the same UID/source for the new one-file Combined-LOD export.
    combined = legacy_mesh()
    combined.pop("lods")
    prepared = prepare_manifest_update(
        str(manifest.parent), resources=[combined], exporter_version="0.4.1",
        source_root=str(tmp_path))
    assert {row["uid"] for row in prepared["resources"]} == {
        MESH_UID, MATERIAL_UID}


def test_empty_legacy_lods_is_still_migratable(tmp_path):
    manifest, _original, _primary, _lod1 = write_case(
        tmp_path, mesh=legacy_mesh(lods=[]))
    receipt = migrate_manifest(manifest, MESH_UID)
    assert receipt["lod_payloads"] == []
    assert MESH_UID not in manifest.read_text(encoding="utf-8")


def test_pending_marker_refuses_without_writes(tmp_path):
    manifest, original, _primary, _lod1 = write_case(tmp_path)
    pending = manifest.with_name(PENDING_MANIFEST_NAME)
    pending.write_text("pending", encoding="utf-8")
    with pytest.raises(MigrationError, match="pending"):
        migrate_manifest(manifest, MESH_UID)
    assert manifest.read_bytes() == original
    assert not backup_path_for(manifest, MESH_UID).exists()
    assert not (manifest.parent / ".mh_manifest_writer.lock").exists()


@pytest.mark.parametrize("case", ["escaping", "missing", "nonfile"])
def test_unsafe_payload_refuses_without_writes(tmp_path, case):
    mesh = legacy_mesh()
    state = case
    if case == "escaping":
        mesh["lods"][0]["source"] = "../wall_a__10000000.lod1.mesh.fbx"
        state = "files"
    manifest, original, _primary, _lod1 = write_case(
        tmp_path, mesh=mesh, payload_state=state)
    with pytest.raises(MigrationError):
        migrate_manifest(manifest, MESH_UID, source_root=tmp_path)
    assert manifest.read_bytes() == original
    assert not backup_path_for(manifest, MESH_UID).exists()
    assert not (manifest.parent / ".mh_manifest_writer.lock").exists()


def test_existing_backup_collision_refuses_without_active_write(tmp_path):
    manifest, original, _primary, _lod1 = write_case(tmp_path)
    backup = backup_path_for(manifest, MESH_UID)
    backup.write_bytes(b"do not overwrite")
    with pytest.raises(MigrationError, match="backup already exists"):
        migrate_manifest(manifest, MESH_UID)
    assert manifest.read_bytes() == original
    assert backup.read_bytes() == b"do not overwrite"
    assert not (manifest.parent / ".mh_manifest_writer.lock").exists()


@pytest.mark.parametrize("case", ["ambiguous", "nonlegacy"])
def test_ambiguous_or_nonlegacy_uid_refuses_without_writes(tmp_path, case):
    manifest, original, _primary, _lod1 = write_case(tmp_path)
    legacy = json.loads(original.decode("utf-8"))
    if case == "ambiguous":
        duplicate = dict(legacy_mesh(), name="duplicate")
        legacy["resources"].insert(1, duplicate)
    else:
        legacy["resources"][0].pop("lods")
    rewritten = (json.dumps(legacy, indent=2) + "\n").encode("utf-8")
    manifest.write_bytes(rewritten)

    with pytest.raises(MigrationError, match=(
            "ambiguous" if case == "ambiguous" else "no deprecated lods")):
        migrate_manifest(manifest, MESH_UID)
    assert manifest.read_bytes() == rewritten
    assert not backup_path_for(manifest, MESH_UID).exists()
    assert not (manifest.parent / ".mh_manifest_writer.lock").exists()


def test_restore_requires_unchanged_prepared_bytes_and_restores_exact(tmp_path):
    manifest, original, primary, lod1 = write_case(tmp_path)
    migrate_manifest(manifest, MESH_UID)
    backup = backup_path_for(manifest, MESH_UID)

    receipt = restore_manifest(manifest, MESH_UID)
    assert manifest.read_bytes() == original
    assert not backup.exists()
    assert receipt["operation"] == "restore_legacy_manifest"
    assert receipt["backup_consumed"] is True
    assert primary.read_bytes() == b"primary"
    assert lod1.read_bytes() == b"lod1"


def test_restore_refuses_if_active_manifest_changed(tmp_path):
    manifest, _original, _primary, _lod1 = write_case(tmp_path)
    migrate_manifest(manifest, MESH_UID)
    backup = backup_path_for(manifest, MESH_UID)
    active = json.loads(manifest.read_text(encoding="utf-8"))
    active["exporter_version"] = "changed"
    manifest.write_text(json.dumps(active), encoding="utf-8")
    changed = manifest.read_bytes()

    with pytest.raises(MigrationError, match="changed since migration"):
        restore_manifest(manifest, MESH_UID)
    assert manifest.read_bytes() == changed
    assert backup.exists()
