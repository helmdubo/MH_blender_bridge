import json
import os
import sys

import pytest


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "addon"))

import mh4blend.core.source_resolver as source_resolver_mod  # noqa: E402
from mh4blend.core.source_resolver import (  # noqa: E402
    ResolverError,
    assert_source_snapshot_stable,
    pending_writer_uid,
    resolve_resource,
    resolve_resource_for_import,
    resolve_resource_for_writer,
    resolve_resources,
    scan_source_root,
)
from mh4blend.scene.source_manifest import (  # noqa: E402
    MANIFEST_NAME,
    PENDING_MANIFEST_NAME,
    ManifestError,
    commit_staged_manifest,
    prepare_manifest_update,
    stage_manifest,
)


MESH_UID = "10000000-0000-0000-0000-000000000001"
COMPOSITE_UID = "20000000-0000-0000-0000-000000000002"
MATERIAL_UID = "30000000-0000-0000-0000-000000000003"


def mesh_row(uid=MESH_UID, name="mesh_a", source=None):
    return {
        "uid": uid,
        "kind": "static_mesh",
        "name": name,
        "source": source or f"{name}__{uid[:8]}.mesh.fbx",
        "content_hash": "xxh3:0000000000000001",
    }


def composite_row(uid=COMPOSITE_UID, name="composite_b", source=None):
    return {
        "uid": uid,
        "kind": "composite",
        "name": name,
        "source": source or f"{name}__{uid[:8]}.composite",
        "content_hash": "xxh3:0000000000000002",
    }


def material_row(uid=MATERIAL_UID, name="material_c", source=None):
    return {
        "uid": uid,
        "kind": "material",
        "name": name,
        "source": source or f"{name}__{uid[:8]}.material",
        "content_hash": "xxh3:0000000000000003",
    }


def write_manifest(directory, rows):
    directory.mkdir(parents=True, exist_ok=True)
    document = {
        "schema": "mh.export_manifest",
        "schema_version": 1,
        "exporter_version": "0.4.0",
        "resources": sorted(rows, key=lambda row: row["uid"]),
    }
    path = directory / MANIFEST_NAME
    path.write_text(json.dumps(document), encoding="utf-8")
    return path


def write_pending_manifest(directory, rows, exporter_version="0.4.0"):
    directory.mkdir(parents=True, exist_ok=True)
    document = {
        "schema": "mh.export_manifest",
        "schema_version": 1,
        "exporter_version": exporter_version,
        "resources": sorted(rows, key=lambda row: row["uid"]),
    }
    path = directory / PENDING_MANIFEST_NAME
    path.write_text(json.dumps(document), encoding="utf-8")
    return path


def write_payload(directory, row, *, uid=None, name=None):
    path = directory / row["source"]
    path.parent.mkdir(parents=True, exist_ok=True)
    if row["kind"] == "static_mesh":
        path.write_bytes(b"Kaydara FBX Binary")
    elif row["kind"] == "composite":
        path.write_text(json.dumps({
            "schema": "mh.composite",
            "schema_version": 1,
            "uid": uid or row["uid"],
            "name": name or row["name"],
            "nodes": [],
        }), encoding="utf-8")
    else:
        path.write_text(json.dumps({
            "schema": "mh.material",
            "schema_version": 1,
            "uid": uid or row["uid"],
            "name": name or row["name"],
            "shader_class": "rendinst_simple",
            "params": {},
            "textures": {},
        }), encoding="utf-8")
    return path


def test_full_scan_resolves_honest_payload_owner_row_triple(tmp_path):
    mesh = mesh_row(source="payload/mesh_a__10000000.mesh.fbx")
    composite = composite_row()
    manifest_a = write_manifest(tmp_path / "lib_a", [composite])
    manifest_b = write_manifest(tmp_path / "nested" / "lib_b", [mesh])
    composite_path = write_payload(tmp_path / "lib_a", composite)
    mesh_path = write_payload(tmp_path / "nested" / "lib_b", mesh)

    snapshot = scan_source_root(str(tmp_path))
    resolved_mesh = resolve_resource(
        snapshot, MESH_UID, expected_kind="static_mesh")
    resolved_composite = resolve_resource(
        snapshot, COMPOSITE_UID, expected_kind="composite")
    assert resolved_mesh.payload_path == os.path.normpath(str(mesh_path))
    assert resolved_mesh.owning_manifest_path == os.path.normpath(str(manifest_b))
    assert resolved_mesh.manifest_row == mesh
    assert resolved_composite.payload_path == os.path.normpath(str(composite_path))
    assert resolved_composite.owning_manifest_path == os.path.normpath(str(manifest_a))


def test_scan_only_accepts_exact_manifest_basename(tmp_path):
    row = mesh_row()
    ignored = tmp_path / "EXPORT_MANIFEST.JSON"
    ignored.write_text(json.dumps({"not": "a manifest"}), encoding="utf-8")
    snapshot = scan_source_root(str(tmp_path))
    assert snapshot.manifests == {}
    assert resolve_resource(snapshot, row["uid"]) is None


def test_zero_owner_is_unresolved_not_guessed_from_payload_extension(tmp_path):
    (tmp_path / "orphan__10000000.mesh.fbx").write_bytes(b"fbx")
    snapshot = scan_source_root(str(tmp_path))
    assert resolve_resource(snapshot, MESH_UID) is None


def test_two_owning_manifests_are_ambiguity_error(tmp_path):
    row_a = mesh_row(source="a__10000000.mesh.fbx")
    row_b = mesh_row(source="b__10000000.mesh.fbx")
    write_manifest(tmp_path / "a", [row_a])
    write_manifest(tmp_path / "b", [row_b])
    write_payload(tmp_path / "a", row_a)
    write_payload(tmp_path / "b", row_b)
    snapshot = scan_source_root(str(tmp_path))
    with pytest.raises(ResolverError) as caught:
        resolve_resource(snapshot, MESH_UID)
    assert caught.value.code == "MH_E_AMBIGUOUS_RESOURCE_OWNER"


def test_uid8_collision_is_detected_across_manifest_set(tmp_path):
    other_uid = "10000000-1111-1111-1111-111111111111"
    write_manifest(tmp_path / "a", [mesh_row()])
    write_manifest(tmp_path / "b", [mesh_row(
        uid=other_uid, name="mesh_b", source="mesh_b__10000000.mesh.fbx")])
    with pytest.raises(ResolverError) as caught:
        scan_source_root(str(tmp_path))
    assert caught.value.code == "MH_E_UID8_COLLISION"


def test_any_pending_marker_under_root_blocks_global_snapshot(tmp_path):
    nested = tmp_path / "unrelated" / "deep"
    nested.mkdir(parents=True)
    (nested / PENDING_MANIFEST_NAME).write_text("pending", encoding="utf-8")
    with pytest.raises(ResolverError) as caught:
        scan_source_root(str(tmp_path))
    assert caught.value.code == "MH_E_INVALID_EXPORT_MANIFEST"


def test_writer_resolution_without_marker_matches_normal_scan_resolve(tmp_path):
    row = material_row()
    manifest_path = write_manifest(tmp_path / "materials", [row])
    payload_path = write_payload(tmp_path / "materials", row)
    result = resolve_resource_for_writer(
        str(tmp_path), MATERIAL_UID, expected_kind="material")
    assert not result.recovery
    assert result.pending_manifest_path is None
    assert result.owner.payload_path == os.path.normpath(str(payload_path))
    assert result.owner.owning_manifest_path == os.path.normpath(
        str(manifest_path))
    assert result.owner.manifest_row == row
    assert_source_snapshot_stable(result.snapshot)


def test_writer_normal_returns_missing_owned_payload_for_repair(tmp_path):
    row = material_row()
    directory = tmp_path / "materials"
    manifest_path = write_manifest(directory, [row])
    result = resolve_resource_for_writer(
        str(tmp_path), MATERIAL_UID, expected_kind="material")
    assert not result.recovery
    assert result.owner.payload_path == os.path.normpath(
        str(directory / row["source"]))
    assert result.owner.owning_manifest_path == os.path.normpath(
        str(manifest_path))
    assert result.owner.manifest_row == row
    assert result.owner.payload_path in result.snapshot.missing_payload_paths

    write_payload(directory, row)
    with pytest.raises(ResolverError, match="missing payload appeared"):
        assert_source_snapshot_stable(result.snapshot)


@pytest.mark.parametrize("row_factory, uid", [
    (material_row, MATERIAL_UID),
    (composite_row, COMPOSITE_UID),
])
def test_writer_normal_captures_corrupt_payload_as_opaque_stable_bytes(
        tmp_path, row_factory, uid):
    row = row_factory()
    directory = tmp_path / "owned"
    write_manifest(directory, [row])
    payload = directory / row["source"]
    payload.write_bytes(b"corrupt payload that the writer must repair")
    strict_snapshot = scan_source_root(str(tmp_path))
    with pytest.raises(ResolverError):
        resolve_resource(strict_snapshot, uid, expected_kind=row["kind"])
    result = resolve_resource_for_writer(
        str(tmp_path), uid, expected_kind=row["kind"])
    assert result.owner.payload_path == os.path.normpath(str(payload))
    assert result.snapshot.payload_bytes[result.owner.payload_path] == \
        b"corrupt payload that the writer must repair"

    payload.write_bytes(b"changed after writer preflight")
    with pytest.raises(ResolverError, match="payload changed after snapshot"):
        assert_source_snapshot_stable(result.snapshot)


def test_writer_normal_rejects_non_file_primary_payload(tmp_path):
    row = material_row()
    directory = tmp_path / "materials"
    write_manifest(directory, [row])
    os.mkdir(directory / row["source"])
    with pytest.raises(ResolverError, match="payload path is not a file"):
        resolve_resource_for_writer(
            str(tmp_path), MATERIAL_UID, expected_kind="material")


def test_writer_normal_rejects_payload_real_target_outside_owner(
        tmp_path, monkeypatch):
    row = material_row()
    directory = tmp_path / "materials"
    outside = tmp_path / "outside"
    write_manifest(directory, [row])
    outside.mkdir()
    payload = directory / row["source"]
    escaped = outside / row["source"]
    original_realpath = source_resolver_mod.os.path.realpath

    def escaped_realpath(path):
        if os.path.normpath(os.path.abspath(path)) == os.path.normpath(
                str(payload)):
            return str(escaped)
        return original_realpath(path)

    monkeypatch.setattr(
        source_resolver_mod.os.path, "realpath", escaped_realpath)
    with pytest.raises(ResolverError, match="owning manifest directory"):
        resolve_resource_for_writer(
            str(tmp_path), MATERIAL_UID, expected_kind="material")


def test_combined_authored_lod_writer_uses_one_primary_payload(tmp_path):
    row = dict(mesh_row(), lod_policy="authored")
    directory = tmp_path / "meshes"
    write_manifest(directory, [row])
    result = resolve_resource_for_writer(
        str(tmp_path), MESH_UID, expected_kind="static_mesh")
    payload_path = os.path.normpath(str(directory / row["source"]))
    assert result.owner.manifest_row == row
    assert result.owner.payload_path == payload_path
    assert result.snapshot.missing_payload_paths == {payload_path}
    assert result.snapshot.payload_bytes == {}

    write_payload(directory, row)
    with pytest.raises(ResolverError, match="missing payload appeared"):
        assert_source_snapshot_stable(result.snapshot)


def test_writer_resolution_admits_first_create_crash_without_payload(tmp_path):
    row = material_row()
    marker = write_pending_manifest(tmp_path / "materials", [row])
    result = resolve_resource_for_writer(
        str(tmp_path), MATERIAL_UID, expected_kind="material")
    assert result.recovery
    assert result.pending_manifest_path == os.path.normpath(str(marker))
    assert result.owner.payload_path == os.path.normpath(
        str(tmp_path / "materials" / row["source"]))
    assert result.owner.owning_manifest_path == os.path.normpath(
        str(tmp_path / "materials" / MANIFEST_NAME))
    assert result.owner.manifest_row == row
    assert not os.path.exists(result.owner.payload_path)
    assert_source_snapshot_stable(result.snapshot)


def test_writer_resolution_admits_update_crash_with_missing_payload(tmp_path):
    stable = material_row()
    directory = tmp_path / "materials"
    manifest_path = write_manifest(directory, [stable])
    changed = dict(stable, content_hash="xxh3:0000000000000099")
    marker = write_pending_manifest(directory, [changed], "0.4.1")
    result = resolve_resource_for_writer(
        str(tmp_path), MATERIAL_UID, expected_kind="material",
        expected_source=stable["source"])
    assert result.recovery
    assert result.pending_manifest_path == os.path.normpath(str(marker))
    assert result.owner.owning_manifest_path == os.path.normpath(
        str(manifest_path))
    assert result.owner.manifest_row == changed
    assert not os.path.exists(result.owner.payload_path)


def test_writer_recovery_does_not_trust_already_replaced_payload(tmp_path):
    stable = material_row()
    directory = tmp_path / "materials"
    write_manifest(directory, [stable])
    changed = dict(stable, content_hash="xxh3:0000000000000099")
    write_pending_manifest(directory, [changed])
    payload = directory / stable["source"]
    payload.write_bytes(b"partially or fully replaced payload is not proof")
    result = resolve_resource_for_writer(
        str(tmp_path), MATERIAL_UID, expected_kind="material")
    assert result.recovery
    assert result.owner.payload_path == os.path.normpath(str(payload))


def test_writer_resolution_blocks_marker_for_other_uid(tmp_path):
    material = material_row()
    write_manifest(tmp_path / "materials", [material])
    write_payload(tmp_path / "materials", material)
    write_pending_manifest(tmp_path / "meshes", [mesh_row()])
    with pytest.raises(ResolverError, match="does not contain selected"):
        resolve_resource_for_writer(
            str(tmp_path), MATERIAL_UID, expected_kind="material")


def test_writer_resolution_blocks_two_pending_markers(tmp_path):
    write_pending_manifest(tmp_path / "materials", [material_row()])
    write_pending_manifest(tmp_path / "meshes", [mesh_row()])
    with pytest.raises(ResolverError, match="exactly one pending marker"):
        resolve_resource_for_writer(
            str(tmp_path), MATERIAL_UID, expected_kind="material")


def test_writer_recovery_snapshot_detects_marker_mutation(tmp_path):
    row = material_row()
    marker = write_pending_manifest(tmp_path / "materials", [row])
    result = resolve_resource_for_writer(
        str(tmp_path), MATERIAL_UID, expected_kind="material")
    marker.write_text("{}", encoding="utf-8")
    with pytest.raises(ResolverError, match="recovery marker changed"):
        assert_source_snapshot_stable(result.snapshot)


def test_pending_writer_uid_returns_none_without_marker(tmp_path):
    assert pending_writer_uid(str(tmp_path)) is None


def test_pending_writer_uid_identifies_first_create_upsert(tmp_path):
    row = material_row()
    marker = write_pending_manifest(tmp_path / "materials", [row])
    pending = pending_writer_uid(str(tmp_path), expected_kind="material")
    assert pending.uid == MATERIAL_UID
    assert pending.kind == "material"
    assert pending.source == row["source"]
    assert pending.pending_manifest_path == os.path.normpath(str(marker))


def test_pending_writer_uid_identifies_only_changed_update_row(tmp_path):
    material = material_row()
    mesh = mesh_row()
    directory = tmp_path / "common"
    write_manifest(directory, [material, mesh])
    changed_material = dict(
        material, content_hash="xxh3:0000000000000099")
    write_pending_manifest(directory, [changed_material, mesh], "0.4.1")
    pending = pending_writer_uid(str(tmp_path), expected_kind="material")
    assert pending.uid == MATERIAL_UID
    assert pending.kind == "material"


def test_pending_writer_uid_rejects_kind_mismatch(tmp_path):
    write_pending_manifest(tmp_path / "materials", [material_row()])
    with pytest.raises(ResolverError, match="expected static_mesh"):
        pending_writer_uid(str(tmp_path), expected_kind="static_mesh")


def test_pending_writer_uid_rejects_multiple_changed_rows(tmp_path):
    material = material_row()
    mesh = mesh_row()
    directory = tmp_path / "common"
    write_manifest(directory, [material, mesh])
    write_pending_manifest(directory, [
        dict(material, content_hash="xxh3:0000000000000099"),
        dict(mesh, content_hash="xxh3:0000000000000088"),
    ])
    with pytest.raises(ResolverError, match="exactly one changed resource"):
        pending_writer_uid(str(tmp_path))


def test_pending_writer_uid_rejects_deletion_only_diff(tmp_path):
    material = material_row()
    mesh = mesh_row()
    directory = tmp_path / "common"
    write_manifest(directory, [material, mesh])
    write_pending_manifest(directory, [mesh])
    with pytest.raises(ResolverError, match="deletion, not an upsert"):
        pending_writer_uid(str(tmp_path))


def test_pending_writer_uid_rejects_invalid_or_multiple_markers(tmp_path):
    invalid = tmp_path / "invalid"
    invalid.mkdir()
    (invalid / PENDING_MANIFEST_NAME).write_text("{}", encoding="utf-8")
    with pytest.raises(ManifestError):
        pending_writer_uid(str(tmp_path))

    (invalid / PENDING_MANIFEST_NAME).unlink()
    write_pending_manifest(tmp_path / "materials", [material_row()])
    write_pending_manifest(tmp_path / "meshes", [mesh_row()])
    with pytest.raises(ResolverError, match="exactly one pending marker"):
        pending_writer_uid(str(tmp_path))


def test_writer_recovery_combined_authored_lod_uses_primary_payload(tmp_path):
    directory = tmp_path / "meshes"
    row = dict(mesh_row(), lod_policy="authored")
    write_pending_manifest(directory, [row])
    result = resolve_resource_for_writer(
        str(tmp_path), MESH_UID, expected_kind="static_mesh")
    assert result.recovery
    assert result.owner.manifest_row == row
    assert result.owner.payload_path == os.path.normpath(
        str(directory / row["source"]))


def test_missing_payload_and_wrong_expected_kind_block_resolve(tmp_path):
    row = composite_row()
    write_manifest(tmp_path, [row])
    snapshot = scan_source_root(str(tmp_path))
    with pytest.raises(ResolverError) as missing:
        resolve_resource(snapshot, COMPOSITE_UID)
    assert missing.value.code == "MH_E_UNRESOLVED_EXTERNAL"

    write_payload(tmp_path, row)
    snapshot = scan_source_root(str(tmp_path))
    with pytest.raises(ResolverError) as wrong_kind:
        resolve_resource(snapshot, COMPOSITE_UID, expected_kind="static_mesh")
    assert wrong_kind.value.code == "MH_E_UNRESOLVED_EXTERNAL"


def test_import_resolver_preserves_unique_owner_when_payload_is_missing(
        tmp_path):
    row = composite_row()
    manifest_path = write_manifest(tmp_path / "composites", [row])
    snapshot = scan_source_root(str(tmp_path))
    resolved = resolve_resource_for_import(
        snapshot, COMPOSITE_UID, expected_kind="composite")
    assert resolved.payload_path == os.path.normpath(
        str(tmp_path / "composites" / row["source"]))
    assert resolved.owning_manifest_path == os.path.normpath(
        str(manifest_path))
    assert resolved.manifest_row == row
    assert resolved.payload_path in snapshot.missing_payload_paths
    assert_source_snapshot_stable(snapshot)


def test_import_resolver_missing_payload_appearance_invalidates_snapshot(
        tmp_path):
    row = composite_row()
    directory = tmp_path / "composites"
    write_manifest(directory, [row])
    snapshot = scan_source_root(str(tmp_path))
    resolved = resolve_resource_for_import(snapshot, COMPOSITE_UID)
    write_payload(directory, row)
    with pytest.raises(ResolverError, match="missing payload appeared"):
        assert_source_snapshot_stable(snapshot)
    assert os.path.isfile(resolved.payload_path)


def test_import_resolver_missing_payload_directory_appearance_invalidates(
        tmp_path):
    row = composite_row()
    directory = tmp_path / "composites"
    write_manifest(directory, [row])
    snapshot = scan_source_root(str(tmp_path))
    resolved = resolve_resource_for_import(snapshot, COMPOSITE_UID)
    os.mkdir(resolved.payload_path)
    with pytest.raises(ResolverError, match="missing payload appeared"):
        assert_source_snapshot_stable(snapshot)


def test_import_resolver_rejects_existing_non_file_primary_path(tmp_path):
    row = composite_row()
    directory = tmp_path / "composites"
    write_manifest(directory, [row])
    os.mkdir(directory / row["source"])
    snapshot = scan_source_root(str(tmp_path))
    with pytest.raises(ResolverError, match="payload path is not a file"):
        resolve_resource_for_import(snapshot, COMPOSITE_UID)


def test_import_resolver_preserves_missing_combined_authored_owner(tmp_path):
    row = dict(mesh_row(), lod_policy="authored")
    directory = tmp_path / "meshes"
    manifest_path = write_manifest(directory, [row])
    snapshot = scan_source_root(str(tmp_path))
    resolved = resolve_resource_for_import(snapshot, MESH_UID)
    assert resolved.manifest_row == row
    assert resolved.owning_manifest_path == os.path.normpath(
        str(manifest_path))
    assert resolved.payload_path in snapshot.missing_payload_paths
    assert snapshot.payload_bytes == {}

    write_payload(directory, row)
    with pytest.raises(ResolverError, match="missing payload appeared"):
        assert_source_snapshot_stable(snapshot)


def test_import_resolver_does_not_relax_owner_or_kind_errors(tmp_path):
    row = composite_row()
    write_manifest(tmp_path / "a", [row])
    write_manifest(tmp_path / "b", [row])
    snapshot = scan_source_root(str(tmp_path))
    with pytest.raises(ResolverError) as ambiguous:
        resolve_resource_for_import(snapshot, COMPOSITE_UID)
    assert ambiguous.value.code == "MH_E_AMBIGUOUS_RESOURCE_OWNER"

    (tmp_path / "b" / MANIFEST_NAME).unlink()
    snapshot = scan_source_root(str(tmp_path))
    with pytest.raises(ResolverError) as wrong_kind:
        resolve_resource_for_import(
            snapshot, COMPOSITE_UID, expected_kind="static_mesh")
    assert wrong_kind.value.code == "MH_E_UNRESOLVED_EXTERNAL"


def test_import_resolver_existing_payload_keeps_strict_validation(tmp_path):
    row = composite_row()
    directory = tmp_path / "composites"
    write_manifest(directory, [row])
    payload = write_payload(directory, row, uid=MESH_UID)
    snapshot = scan_source_root(str(tmp_path))
    with pytest.raises(ResolverError, match="UID/name"):
        resolve_resource_for_import(snapshot, COMPOSITE_UID)

    payload.unlink()
    snapshot = scan_source_root(str(tmp_path))
    assert resolve_resource_for_import(snapshot, "ffffffff-ffff-ffff-ffff-ffffffffffff") is None


def test_payload_real_target_cannot_escape_owning_manifest_directory(
        tmp_path, monkeypatch):
    owner = tmp_path / "owner"
    target_dir = tmp_path / "other"
    row = mesh_row()
    write_manifest(owner, [row])
    target_dir.mkdir()
    target = target_dir / row["source"]
    target.write_bytes(b"fbx")
    payload = write_payload(owner, row)
    original_realpath = source_resolver_mod.os.path.realpath

    def symlink_realpath(path):
        if os.path.normpath(os.path.abspath(path)) == os.path.normpath(
                str(payload)):
            return str(target)
        return original_realpath(path)

    monkeypatch.setattr(
        source_resolver_mod.os.path, "realpath", symlink_realpath)
    with pytest.raises(ResolverError, match="owning manifest directory"):
        scan_source_root(str(tmp_path))


def test_resolve_combined_authored_lod_reads_only_primary_payload(tmp_path):
    row = dict(mesh_row(), lod_policy="authored")
    write_manifest(tmp_path, [row])
    payload = write_payload(tmp_path, row)
    snapshot = scan_source_root(str(tmp_path))
    resolved = resolve_resource(snapshot, MESH_UID)
    normalized = os.path.normpath(str(payload))
    assert resolved.payload_path == normalized
    assert snapshot.payload_bytes == {normalized: b"Kaydara FBX Binary"}


@pytest.mark.parametrize("legacy_lods", [[], [{
    "level": 1,
    "source": "mesh_a__10000000.lod1.mesh.fbx",
    "content_hash": "xxh3:0000000000000009",
}]])
def test_scan_rejects_deprecated_per_file_lod_rows(tmp_path, legacy_lods):
    write_manifest(tmp_path, [dict(
        mesh_row(), lod_policy="authored", lods=legacy_lods)])
    with pytest.raises(ManifestError) as caught:
        scan_source_root(str(tmp_path))
    assert caught.value.code == "MH_E_DEPRECATED_LOD_ROWS"


def test_manifest_real_target_cannot_make_external_file_authoritative(
        tmp_path, monkeypatch):
    source_root = tmp_path / "root"
    external = tmp_path / "external"
    source_root.mkdir()
    write_manifest(external, [])
    manifest_path = write_manifest(source_root, [])
    original_realpath = source_resolver_mod.os.path.realpath

    def symlink_realpath(path):
        if os.path.normpath(os.path.abspath(path)) == os.path.normpath(
                str(manifest_path)):
            return str(external / MANIFEST_NAME)
        return original_realpath(path)

    monkeypatch.setattr(
        source_resolver_mod.os.path, "realpath", symlink_realpath)
    with pytest.raises(ResolverError, match="escapes source_root"):
        scan_source_root(str(source_root))


@pytest.mark.parametrize(("uid", "name"), [
    (MESH_UID, "composite_b"),
    (COMPOSITE_UID, "wrong_name"),
])
def test_json_payload_identity_must_match_owning_row(tmp_path, uid, name):
    row = composite_row()
    write_manifest(tmp_path, [row])
    write_payload(tmp_path, row, uid=uid, name=name)
    snapshot = scan_source_root(str(tmp_path))
    with pytest.raises(ResolverError, match="UID/name"):
        resolve_resource(snapshot, COMPOSITE_UID)


def test_resolver_accepts_self_contained_composite_v2(tmp_path):
    row = composite_row()
    write_manifest(tmp_path, [row])
    path = tmp_path / row["source"]
    path.write_text(json.dumps({
        "schema": "mh.composite",
        "schema_version": 2,
        "uid": row["uid"],
        "name": row["name"],
        "properties": {"category": "building"},
        "nodes": [],
    }), encoding="utf-8")
    snapshot = scan_source_root(str(tmp_path))
    assert resolve_resource(snapshot, COMPOSITE_UID) is not None


@pytest.mark.parametrize("mutator,code", [
    (lambda document: document.update(schema_version=3),
     "MH_E_UNKNOWN_SCHEMA_VERSION"),
    (lambda document: document.update(future=True),
     "MH_E_INVALID_COMPOSITE"),
    (lambda document: document.__setitem__("properties", []),
     "MH_E_INVALID_COMPOSITE"),
])
def test_resolver_rejects_invalid_composite_v2_envelope(
        tmp_path, mutator, code):
    row = composite_row()
    write_manifest(tmp_path, [row])
    document = {
        "schema": "mh.composite",
        "schema_version": 2,
        "uid": row["uid"],
        "name": row["name"],
        "properties": {},
        "nodes": [],
    }
    mutator(document)
    (tmp_path / row["source"]).write_text(
        json.dumps(document), encoding="utf-8")
    snapshot = scan_source_root(str(tmp_path))
    with pytest.raises(ResolverError) as caught:
        resolve_resource(snapshot, COMPOSITE_UID)
    assert caught.value.code == code


def test_material_payload_uses_strict_frozen_v1_codec(tmp_path):
    row = material_row()
    write_manifest(tmp_path, [row])
    path = write_payload(tmp_path, row)
    document = json.loads(path.read_text(encoding="utf-8"))
    document["unknown"] = True
    path.write_text(json.dumps(document), encoding="utf-8")
    snapshot = scan_source_root(str(tmp_path))
    with pytest.raises(ResolverError) as caught:
        resolve_resource(snapshot, MATERIAL_UID)
    assert caught.value.code == "MH_E_INVALID_MATERIAL_VALUE"


def test_material_texture_policy_is_recomputed_during_resolve(tmp_path):
    row = material_row()
    write_manifest(tmp_path, [row])
    path = write_payload(tmp_path, row)
    document = json.loads(path.read_text(encoding="utf-8"))
    document["textures"] = {"tex0": "D:/external/texture.tif"}
    path.write_text(json.dumps(document), encoding="utf-8")

    transitional = scan_source_root(
        str(tmp_path), texture_policy="transitional")
    assert resolve_resource(transitional, MATERIAL_UID) is not None
    assert transitional.diagnostics[-1].code == "MH_W_TEXTURE_OUTSIDE_ROOT"

    strict = scan_source_root(str(tmp_path), texture_policy="strict")
    with pytest.raises(ResolverError) as caught:
        resolve_resource(strict, MATERIAL_UID)
    assert caught.value.code == "MH_E_TEXTURE_OUTSIDE_ROOT"


def write_registry(path, rows, **extra):
    document = {
        "schema": "mh.registry",
        "schema_version": 1,
        "resources": rows,
        **extra,
    }
    path.write_text(json.dumps(document), encoding="utf-8")


def registry_hint(row, source_path, manifest_path=None):
    hint = {
        "uid": row["uid"],
        "kind": row["kind"],
        "name": row["name"],
        "source_path": source_path,
    }
    if manifest_path is not None:
        hint["manifest_path"] = manifest_path
    return hint


def test_current_registry_hint_is_confirmed_by_full_scan(tmp_path):
    row = material_row()
    write_manifest(tmp_path / "common", [row])
    write_payload(tmp_path / "common", row)
    registry = tmp_path / "registry.json"
    write_registry(registry, [registry_hint(
        row, f"common/{row['source']}", "common/export_manifest.json")],
        shader_classes=["rendinst_simple"])
    snapshot = scan_source_root(str(tmp_path), registry_path=str(registry))
    resolved = resolve_resource(snapshot, MATERIAL_UID, expected_kind="material")
    assert resolved is not None
    assert snapshot.diagnostics == []


@pytest.mark.parametrize("mutator", [
    lambda hint: hint.update(kind="composite"),
    lambda hint: hint.update(name="old_name"),
    lambda hint: hint.update(source_path="wrong/material__30000000.material"),
    lambda hint: hint.update(manifest_path="wrong/export_manifest.json"),
])
def test_stale_registry_hint_warns_and_scan_result_wins(tmp_path, mutator):
    row = material_row()
    write_manifest(tmp_path / "common", [row])
    write_payload(tmp_path / "common", row)
    hint = registry_hint(
        row, f"common/{row['source']}", "common/export_manifest.json")
    mutator(hint)
    registry = tmp_path / "registry.json"
    write_registry(registry, [hint])
    snapshot = scan_source_root(str(tmp_path), registry_path=str(registry))
    resolved = resolve_resource(snapshot, MATERIAL_UID)
    assert resolved.manifest_row == row
    assert [item.code for item in snapshot.diagnostics] == [
        "MH_W_REGISTRY_STALE"]


@pytest.mark.parametrize("document", [
    {"schema": "wrong", "schema_version": 1, "resources": []},
    {"schema": "mh.registry", "schema_version": 1,
     "resources": [{
         "uid": MESH_UID, "kind": "static_mesh", "name": "mesh",
         "source_path": "../outside.mesh.fbx"}]},
])
def test_invalid_registry_warns_and_does_not_block_scan(tmp_path, document):
    row = mesh_row()
    write_manifest(tmp_path / "lib", [row])
    write_payload(tmp_path / "lib", row)
    registry = tmp_path / "registry.json"
    registry.write_text(json.dumps(document), encoding="utf-8")
    snapshot = scan_source_root(str(tmp_path), registry_path=str(registry))
    assert resolve_resource(snapshot, MESH_UID) is not None
    assert [item.code for item in snapshot.diagnostics] == [
        "MH_W_REGISTRY_INVALID"]


def test_registry_hint_without_owner_is_stale_unresolved(tmp_path):
    row = mesh_row()
    registry = tmp_path / "registry.json"
    write_registry(registry, [registry_hint(row, row["source"])])
    snapshot = scan_source_root(str(tmp_path), registry_path=str(registry))
    assert resolve_resource(snapshot, MESH_UID) is None
    assert snapshot.diagnostics[-1].code == "MH_W_REGISTRY_STALE"


def test_resolve_wave_deduplicates_repeated_uid(tmp_path):
    row = mesh_row()
    write_manifest(tmp_path, [row])
    write_payload(tmp_path, row)
    snapshot = scan_source_root(str(tmp_path))
    result = resolve_resources(snapshot, [
        (MESH_UID, "static_mesh"),
        (MESH_UID, "static_mesh"),
    ])
    assert list(result) == [MESH_UID]


def test_resolve_wave_rejects_conflicting_expected_kinds(tmp_path):
    snapshot = scan_source_root(str(tmp_path))
    with pytest.raises(ResolverError, match="conflicting expected kinds"):
        resolve_resources(snapshot, [
            (MESH_UID, "static_mesh"),
            (MESH_UID, "composite"),
        ])


def test_snapshot_rejects_manifest_appearance_mutation_and_pending(tmp_path):
    snapshot = scan_source_root(str(tmp_path))
    write_manifest(tmp_path / "new", [mesh_row()])
    with pytest.raises(ResolverError, match="manifest set changed"):
        assert_source_snapshot_stable(snapshot)

    (tmp_path / "new" / MANIFEST_NAME).unlink()
    snapshot = scan_source_root(str(tmp_path))
    (tmp_path / "new" / PENDING_MANIFEST_NAME).write_text(
        "pending", encoding="utf-8")
    with pytest.raises(ResolverError, match="pending marker"):
        assert_source_snapshot_stable(snapshot)


def test_stage_guard_rejects_dependency_interleave_before_marker(tmp_path):
    dependency_directory = tmp_path / "dependency"
    output_directory = tmp_path / "root_composite"
    dependency = mesh_row()
    write_manifest(dependency_directory, [dependency])
    snapshot = scan_source_root(str(tmp_path))
    output_directory.mkdir()
    incoming = composite_row(source="root__20000000.composite")
    prepared = prepare_manifest_update(
        str(output_directory), resources=[incoming], exporter_version="0.4.0",
        source_root=str(tmp_path))

    write_manifest(dependency_directory, [dict(
        dependency, content_hash="xxh3:0000000000000099")])
    with pytest.raises(ResolverError, match="manifest changed after snapshot"):
        stage_manifest(
            str(output_directory), prepared,
            snapshot_guard=lambda: assert_source_snapshot_stable(snapshot))
    assert not (output_directory / PENDING_MANIFEST_NAME).exists()

    # Guard failure released both locks; a freshly prepared retry can stage.
    retry = prepare_manifest_update(
        str(output_directory), resources=[incoming], exporter_version="0.4.0",
        source_root=str(tmp_path))
    stage_manifest(str(output_directory), retry)
    commit_staged_manifest(str(output_directory))


def test_snapshot_rejects_consumed_payload_and_registry_mutation(tmp_path):
    row = mesh_row()
    write_manifest(tmp_path, [row])
    payload = write_payload(tmp_path, row)
    registry = tmp_path / "registry.json"
    write_registry(registry, [registry_hint(row, row["source"], MANIFEST_NAME)])
    snapshot = scan_source_root(str(tmp_path), registry_path=str(registry))
    resolve_resource(snapshot, MESH_UID)
    payload.write_bytes(b"changed fbx")
    with pytest.raises(ResolverError, match="payload changed"):
        assert_source_snapshot_stable(snapshot)

    payload.write_bytes(b"Kaydara FBX Binary")
    snapshot = scan_source_root(str(tmp_path), registry_path=str(registry))
    registry.write_text("{}", encoding="utf-8")
    with pytest.raises(ResolverError, match="registry changed"):
        assert_source_snapshot_stable(snapshot)
