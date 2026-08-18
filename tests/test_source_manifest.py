import json
import os
import sys

import pytest


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "addon"))

import mh4blend.scene.source_manifest as source_manifest_mod  # noqa: E402
from mh4blend.scene.source_manifest import (  # noqa: E402
    MANIFEST_NAME,
    PENDING_MANIFEST_NAME,
    ManifestError,
    abandon_staged_manifest,
    assert_manifest_stable,
    commit_staged_manifest,
    load_export_manifest,
    load_export_manifest_snapshot,
    manifest_resource_index,
    prepare_manifest_update,
    stage_manifest,
    validate_export_manifest,
)


MESH_UID = "10000000-0000-0000-0000-000000000001"
COMPOSITE_UID = "20000000-0000-0000-0000-000000000002"
MATERIAL_UID = "30000000-0000-0000-0000-000000000003"

MESH = {
    "uid": MESH_UID,
    "kind": "static_mesh",
    "name": "mesh_a",
    "source": "meshes/mesh_a__10000000.mesh.fbx",
    "content_hash": "xxh3:0000000000000001",
}
COMPOSITE = {
    "uid": COMPOSITE_UID,
    "kind": "composite",
    "name": "composite_b",
    "source": "composites/composite_b__20000000.composite",
    "content_hash": "xxh3:0000000000000002",
}
MATERIAL = {
    "uid": MATERIAL_UID,
    "kind": "material",
    "name": "material_c",
    "source": "materials/material_c__30000000.material",
    "content_hash": "xxh3:0000000000000003",
}


def manifest(rows=(), exporter_version="0.4.0"):
    return {
        "schema": "mh.export_manifest",
        "schema_version": 1,
        "exporter_version": exporter_version,
        "resources": sorted((dict(row) for row in rows), key=lambda row: row["uid"]),
    }


def prepare(directory, row, exporter_version="0.4.0"):
    return prepare_manifest_update(
        str(directory), resources=[row], exporter_version=exporter_version,
        blend_file="ignored.blend", source_root=str(directory))


def commit(directory, document):
    stage_manifest(str(directory), document)
    commit_staged_manifest(str(directory))


def test_frozen_v1_manifest_has_resources_only_and_material_is_ordinary_row():
    document = validate_export_manifest(manifest([MESH, COMPOSITE, MATERIAL]))
    assert list(document) == [
        "schema", "schema_version", "exporter_version", "resources"]
    assert {row["kind"] for row in document["resources"]} == {
        "static_mesh", "composite", "material"}
    assert manifest_resource_index(document)[MATERIAL_UID] == MATERIAL


@pytest.mark.parametrize("legacy", [
    {"source": {"blend_file": "old.blend"}},
    {"materials": []},
    {"external_dependencies": []},
    {"bundle_uid": "old"},
])
def test_pre_freeze_top_level_fields_are_rejected(legacy):
    document = manifest([MESH])
    document.update(legacy)
    with pytest.raises(ManifestError, match="invalid fields"):
        validate_export_manifest(document)


def test_pre_freeze_bundle_schema_has_no_production_reader():
    document = manifest([MESH])
    document["schema"] = "mh.bundle_manifest"
    document["schema_version"] = 2
    with pytest.raises(ManifestError, match="unsupported export manifest schema"):
        validate_export_manifest(document)


@pytest.mark.parametrize("row", [
    dict(MATERIAL, shader_class="rendinst_simple"),
    dict(COMPOSITE, material_slots=[]),
    dict(MESH, future_field=True),
])
def test_unknown_or_wrong_kind_fields_are_rejected(row):
    with pytest.raises(ManifestError, match="invalid fields"):
        validate_export_manifest(manifest([row]))


@pytest.mark.parametrize("source", [
    "meshes\\mesh_a__10000000.mesh.fbx",
    "./meshes/mesh_a__10000000.mesh.fbx",
    "meshes/temp/../mesh_a__10000000.mesh.fbx",
    "../mesh_a__10000000.mesh.fbx",
    "C:/mesh_a__10000000.mesh.fbx",
    "/mesh_a__10000000.mesh.fbx",
    "meshes/mesh_a__10000000.fbx",
    "meshes/mesh_a__deadbeef.mesh.fbx",
    "meshes/mesh_a__10000000.MESH.FBX",
])
def test_source_must_be_canonical_and_match_kind_uid8_suffix(source):
    with pytest.raises(ManifestError) as caught:
        validate_export_manifest(manifest([dict(MESH, source=source)]))
    assert caught.value.code == "MH_E_INVALID_RESOURCE_SOURCE"


def test_source_path_nfd_is_rejected_instead_of_silently_repaired():
    source = "materials/me\u0301__30000000.material"
    with pytest.raises(ManifestError) as caught:
        validate_export_manifest(manifest([dict(MATERIAL, source=source)]))
    assert caught.value.code == "MH_E_INVALID_RESOURCE_SOURCE"


@pytest.mark.parametrize(("field", "value"), [
    ("uid", "10000000-0000-0000-0000-00000000000A"),
    ("uid", "not-a-uuid"),
    ("content_hash", "xxh3:ABCDEF0000000001"),
    ("content_hash", "xxh3:1234"),
    ("name", "mesh/invalid"),
])
def test_common_row_identity_fields_are_strict(field, value):
    with pytest.raises(ManifestError):
        validate_export_manifest(manifest([dict(MESH, **{field: value})]))


def test_material_slots_may_reference_external_owner_and_are_not_local_closure():
    mesh = dict(MESH, material_slots=[{
        "slot_name": "m_primary", "material_uid": MATERIAL_UID}])
    document = validate_export_manifest(manifest([mesh]))
    assert document["resources"][0]["material_slots"] == mesh["material_slots"]


@pytest.mark.parametrize("slots", [
    {},
    [{"slot_name": "m", "material_uid": MATERIAL_UID, "extra": 1}],
    [
        {"slot_name": "a", "material_uid": MATERIAL_UID},
        {"slot_name": "b", "material_uid": MATERIAL_UID},
    ],
    [
        {"slot_name": "me\u0301tal", "material_uid": MATERIAL_UID},
        {"slot_name": "m\u00e9tal", "material_uid": COMPOSITE_UID},
    ],
])
def test_material_slot_shape_and_deduplication_are_strict(slots):
    with pytest.raises(ManifestError, match="material"):
        validate_export_manifest(manifest([dict(MESH, material_slots=slots)]))


@pytest.mark.parametrize("legacy_lods", [
    [],
    [{
        "level": 1,
        "source": "meshes/mesh_a__10000000.lod1.mesh.fbx",
        "content_hash": "xxh3:0000000000000009",
    }],
])
def test_static_mesh_rejects_deprecated_per_file_lod_rows(legacy_lods):
    row = dict(MESH, lod_policy="authored", lods=legacy_lods)
    with pytest.raises(ManifestError) as caught:
        validate_export_manifest(manifest([row]))
    assert caught.value.code == "MH_E_DEPRECATED_LOD_ROWS"


def test_combined_authored_lod_mesh_is_one_manifest_payload():
    row = dict(MESH, lod_policy="authored")
    normalized = validate_export_manifest(manifest([row]))["resources"][0]
    assert normalized == row
    assert "lods" not in normalized


def test_resources_are_required_to_be_uid_sorted_and_unique():
    with pytest.raises(ManifestError, match="sorted"):
        validate_export_manifest({
            **manifest(), "resources": [COMPOSITE, MESH]})
    with pytest.raises(ManifestError, match="duplicate uid"):
        validate_export_manifest({
            **manifest(), "resources": [MESH, dict(MESH)]})


def test_manifest_rejects_two_full_uids_with_same_uid8():
    collision_uid = "10000000-1111-1111-1111-111111111111"
    collision = dict(
        MESH,
        uid=collision_uid,
        name="mesh_b",
        source="meshes/mesh_b__10000000.mesh.fbx",
    )
    with pytest.raises(ManifestError) as caught:
        validate_export_manifest(manifest([MESH, collision]))
    assert caught.value.code == "MH_E_UID8_COLLISION"


def test_incremental_upsert_preserves_unrelated_resources(tmp_path):
    commit(tmp_path, prepare(tmp_path, MESH))
    updated = prepare(tmp_path, COMPOSITE)
    assert [row["uid"] for row in updated["resources"]] == [
        MESH_UID, COMPOSITE_UID]
    assert set(updated) == {
        "schema", "schema_version", "exporter_version", "resources"}


def test_existing_uid_keeps_kind_and_source_location(tmp_path):
    commit(tmp_path, prepare(tmp_path, MATERIAL))
    with pytest.raises(ManifestError, match="cannot change kind"):
        prepare(tmp_path, dict(MESH, uid=MATERIAL_UID,
                               source="mesh__30000000.mesh.fbx"))
    moved = dict(MATERIAL, source="other/material_c__30000000.material")
    with pytest.raises(ManifestError, match="existing source"):
        prepare(tmp_path, moved)


def test_global_owner_scan_prevents_creating_same_uid_in_second_manifest(tmp_path):
    owner = tmp_path / "library_a"
    candidate = tmp_path / "library_b"
    owner.mkdir()
    candidate.mkdir()
    first = prepare_manifest_update(
        str(owner), resources=[MATERIAL], exporter_version="0.4.0",
        source_root=str(tmp_path))
    commit(owner, first)
    with pytest.raises(ManifestError) as caught:
        prepare_manifest_update(
            str(candidate), resources=[MATERIAL], exporter_version="0.4.0",
            source_root=str(tmp_path))
    assert caught.value.code == "MH_E_AMBIGUOUS_RESOURCE_OWNER"


def test_writer_admission_rejects_uid8_collision_in_another_manifest(tmp_path):
    owner = tmp_path / "library_a"
    candidate = tmp_path / "library_b"
    owner.mkdir()
    candidate.mkdir()
    first = prepare_manifest_update(
        str(owner), resources=[MESH], exporter_version="0.4.0",
        source_root=str(tmp_path))
    commit(owner, first)
    collision_uid = "10000000-1111-1111-1111-111111111111"
    collision = dict(
        MESH, uid=collision_uid, name="mesh_b",
        source="mesh_b__10000000.mesh.fbx")
    with pytest.raises(ManifestError) as caught:
        prepare_manifest_update(
            str(candidate), resources=[collision], exporter_version="0.4.0",
            source_root=str(tmp_path))
    assert caught.value.code == "MH_E_UID8_COLLISION"


def test_owner_directory_must_be_inside_configured_source_root(tmp_path):
    source_root = tmp_path / "root"
    output = tmp_path / "outside"
    source_root.mkdir()
    output.mkdir()
    with pytest.raises(ManifestError) as caught:
        prepare_manifest_update(
            str(output), resources=[MESH], exporter_version="0.4.0",
            source_root=str(source_root))
    assert caught.value.code == "MH_E_INVALID_RESOURCE_SOURCE"


def test_owner_directory_real_target_cannot_escape_source_root(
        tmp_path, monkeypatch):
    source_root = tmp_path / "root"
    external = tmp_path / "external"
    source_root.mkdir()
    external.mkdir()
    linked_output = source_root / "linked"
    linked_output.mkdir()
    original_realpath = source_manifest_mod.os.path.realpath

    def junction_realpath(path):
        if os.path.normpath(os.path.abspath(path)) == os.path.normpath(
                str(linked_output)):
            return str(external)
        return original_realpath(path)

    monkeypatch.setattr(
        source_manifest_mod.os.path, "realpath", junction_realpath)
    with pytest.raises(ManifestError) as caught:
        prepare_manifest_update(
            str(linked_output), resources=[MESH], exporter_version="0.4.0",
            source_root=str(source_root))
    assert caught.value.code == "MH_E_INVALID_RESOURCE_SOURCE"


def test_positive_prepare_requires_source_root(tmp_path):
    with pytest.raises(ManifestError, match="source_root is required"):
        prepare_manifest_update(
            str(tmp_path), resources=[MESH], exporter_version="0.4.0")


def test_one_writer_transaction_upserts_exactly_one_resource(tmp_path):
    with pytest.raises(ManifestError, match="exactly one"):
        prepare_manifest_update(
            str(tmp_path), resources=[MESH, COMPOSITE],
            exporter_version="0.4.0")
    with pytest.raises(ManifestError, match="exactly one"):
        prepare_manifest_update(
            str(tmp_path), resources=[], exporter_version="0.4.0")


def test_pending_marker_blocks_read_until_promoted(tmp_path):
    document = prepare(tmp_path, MESH)
    stage_manifest(str(tmp_path), document)
    with pytest.raises(ManifestError, match="export in progress"):
        load_export_manifest(str(tmp_path))
    commit_staged_manifest(str(tmp_path))
    assert load_export_manifest(str(tmp_path)) == document


def test_two_prepared_writers_cannot_overwrite_first_marker(tmp_path):
    first = prepare(tmp_path, MESH)
    second = prepare(tmp_path, COMPOSITE)
    stage_manifest(str(tmp_path), first)
    with pytest.raises(ManifestError, match="another writer"):
        stage_manifest(str(tmp_path), second)
    commit_staged_manifest(str(tmp_path))
    assert load_export_manifest(str(tmp_path)) == first


def test_cross_directory_stage_rechecks_global_owner_under_root_lock(tmp_path):
    owner_a = tmp_path / "a"
    owner_b = tmp_path / "b"
    owner_a.mkdir()
    owner_b.mkdir()
    row_b = dict(MESH, source="mesh_b__10000000.mesh.fbx")
    prepared_a = prepare_manifest_update(
        str(owner_a), resources=[MESH], exporter_version="0.4.0",
        source_root=str(tmp_path))
    prepared_b = prepare_manifest_update(
        str(owner_b), resources=[row_b], exporter_version="0.4.0",
        source_root=str(tmp_path))
    stage_manifest(str(owner_a), prepared_a)
    with pytest.raises(ManifestError, match="pending marker"):
        stage_manifest(str(owner_b), prepared_b)
    commit_staged_manifest(str(owner_a))


def test_stage_snapshot_guard_fails_under_locks_without_marker_and_releases(
        tmp_path, monkeypatch):
    document = prepare(tmp_path, MESH)
    acquired = []
    original_acquire = source_manifest_mod._acquire_file_lock

    def tracked_acquire(path, owner):
        stream = original_acquire(path, owner)
        acquired.append(stream)
        return stream

    monkeypatch.setattr(
        source_manifest_mod, "_acquire_file_lock", tracked_acquire)

    def stale_guard():
        assert len(acquired) == 2
        assert all(not stream.closed for stream in acquired)
        raise ManifestError("dependency snapshot changed")

    with pytest.raises(ManifestError, match="dependency snapshot changed"):
        stage_manifest(
            str(tmp_path), document, snapshot_guard=stale_guard)
    assert not (tmp_path / PENDING_MANIFEST_NAME).exists()
    assert all(stream.closed for stream in acquired)
    directory_key = source_manifest_mod._directory_key(str(tmp_path))
    assert directory_key not in source_manifest_mod._STAGED_TOKENS
    assert directory_key not in source_manifest_mod._STAGED_LOCKS

    acquired.clear()
    stage_manifest(str(tmp_path), document)
    commit_staged_manifest(str(tmp_path))


def test_recovery_stage_guard_failure_preserves_marker_and_releases_locks(
        tmp_path, monkeypatch):
    interrupted = prepare(tmp_path, MESH)
    marker = stage_manifest(str(tmp_path), interrupted)
    abandon_staged_manifest(str(tmp_path))
    marker_before = (tmp_path / PENDING_MANIFEST_NAME).read_bytes()
    recovery = prepare(
        tmp_path, dict(MESH, content_hash="xxh3:0000000000000099"))
    acquired = []
    original_acquire = source_manifest_mod._acquire_file_lock

    def tracked_acquire(path, owner):
        stream = original_acquire(path, owner)
        acquired.append(stream)
        return stream

    monkeypatch.setattr(
        source_manifest_mod, "_acquire_file_lock", tracked_acquire)

    def stale_guard():
        assert len(acquired) == 2
        assert all(not stream.closed for stream in acquired)
        raise ManifestError("recovery dependency snapshot changed")

    with pytest.raises(
            ManifestError, match="recovery dependency snapshot changed"):
        stage_manifest(
            str(tmp_path), recovery, snapshot_guard=stale_guard)
    assert (tmp_path / PENDING_MANIFEST_NAME).read_bytes() == marker_before
    assert all(stream.closed for stream in acquired)
    directory_key = source_manifest_mod._directory_key(str(tmp_path))
    assert directory_key not in source_manifest_mod._STAGED_TOKENS
    assert directory_key not in source_manifest_mod._STAGED_LOCKS


def test_two_recovery_writers_cannot_replace_same_marker(tmp_path):
    initial = prepare(tmp_path, MESH)
    stage_manifest(str(tmp_path), initial)
    abandon_staged_manifest(str(tmp_path))
    changed_a = dict(MESH, content_hash="xxh3:0000000000000099")
    changed_b = dict(MESH, content_hash="xxh3:0000000000000088")
    recovery_a = prepare(tmp_path, changed_a)
    recovery_b = prepare(tmp_path, changed_b)
    stage_manifest(str(tmp_path), recovery_a)
    with pytest.raises(ManifestError, match="another writer"):
        stage_manifest(str(tmp_path), recovery_b)
    commit_staged_manifest(str(tmp_path))
    assert manifest_resource_index(load_export_manifest(str(tmp_path)))[
        MESH_UID]["content_hash"] == "xxh3:0000000000000099"


def test_stage_rejects_document_that_bypassed_owner_preparation(tmp_path):
    with pytest.raises(ManifestError, match="prepare_manifest_update"):
        stage_manifest(str(tmp_path), manifest([MESH]))
    assert not (tmp_path / PENDING_MANIFEST_NAME).exists()


def test_commit_is_bound_to_exact_bytes_staged_by_writer(tmp_path):
    document = prepare(tmp_path, MESH)
    stage_manifest(str(tmp_path), document)
    replacement = manifest([COMPOSITE])
    (tmp_path / PENDING_MANIFEST_NAME).write_text(
        json.dumps(replacement, indent=2) + "\n", encoding="utf-8")
    with pytest.raises(ManifestError, match="changed before commit"):
        commit_staged_manifest(str(tmp_path))


def _windows_os_error(winerror):
    error = OSError(f"injected WinError {winerror}")
    error.winerror = winerror
    return error


def test_commit_retries_transient_windows_replace_with_lock_held(
        tmp_path, monkeypatch):
    document = prepare(tmp_path, MESH)
    marker = stage_manifest(str(tmp_path), document)
    directory_key = source_manifest_mod._directory_key(str(tmp_path))
    lock_stream = source_manifest_mod._STAGED_LOCKS[directory_key]
    original_replace = source_manifest_mod.os.replace
    calls = []
    sleeps = []

    def transient_then_success(source, destination):
        calls.append((source, destination))
        assert directory_key in source_manifest_mod._STAGED_TOKENS
        assert source_manifest_mod._STAGED_LOCKS[directory_key] is lock_stream
        assert not lock_stream.closed
        if len(calls) <= 2:
            raise _windows_os_error(5 if len(calls) == 1 else 32)
        return original_replace(source, destination)

    monkeypatch.setattr(source_manifest_mod.os, "replace", transient_then_success)
    monkeypatch.setattr(source_manifest_mod.time, "sleep", sleeps.append)
    final = commit_staged_manifest(str(tmp_path))
    assert final == os.path.join(str(tmp_path), MANIFEST_NAME)
    assert len(calls) == 3
    assert sleeps == [0.01, 0.02]
    assert not os.path.exists(marker)
    assert os.path.exists(final)
    assert lock_stream.closed
    assert directory_key not in source_manifest_mod._STAGED_TOKENS
    assert directory_key not in source_manifest_mod._STAGED_LOCKS


def test_commit_exhausted_windows_replace_retry_is_fail_closed(
        tmp_path, monkeypatch):
    document = prepare(tmp_path, MESH)
    marker = stage_manifest(str(tmp_path), document)
    marker_bytes = (tmp_path / PENDING_MANIFEST_NAME).read_bytes()
    directory_key = source_manifest_mod._directory_key(str(tmp_path))
    lock_stream = source_manifest_mod._STAGED_LOCKS[directory_key]
    calls = []
    sleeps = []

    def always_transient(source, destination):
        calls.append((source, destination))
        assert not lock_stream.closed
        raise _windows_os_error(32)

    monkeypatch.setattr(source_manifest_mod.os, "replace", always_transient)
    monkeypatch.setattr(source_manifest_mod.time, "sleep", sleeps.append)
    with pytest.raises(OSError, match="WinError 32"):
        commit_staged_manifest(str(tmp_path))
    assert len(calls) == 7
    assert sum(sleeps) == pytest.approx(0.51)
    assert os.path.exists(marker)
    assert (tmp_path / PENDING_MANIFEST_NAME).read_bytes() == marker_bytes
    assert not (tmp_path / MANIFEST_NAME).exists()
    assert lock_stream.closed
    assert directory_key not in source_manifest_mod._STAGED_TOKENS
    assert directory_key not in source_manifest_mod._STAGED_LOCKS


def test_commit_nontransient_replace_error_is_not_retried(
        tmp_path, monkeypatch):
    document = prepare(tmp_path, MESH)
    marker = stage_manifest(str(tmp_path), document)
    directory_key = source_manifest_mod._directory_key(str(tmp_path))
    lock_stream = source_manifest_mod._STAGED_LOCKS[directory_key]
    calls = []
    sleeps = []
    nontransient = _windows_os_error(87)

    def fail_once(source, destination):
        calls.append((source, destination))
        raise nontransient

    monkeypatch.setattr(source_manifest_mod.os, "replace", fail_once)
    monkeypatch.setattr(source_manifest_mod.time, "sleep", sleeps.append)
    with pytest.raises(OSError, match="WinError 87"):
        commit_staged_manifest(str(tmp_path))
    assert len(calls) == 1
    assert sleeps == []
    assert os.path.exists(marker)
    assert lock_stream.closed
    assert directory_key not in source_manifest_mod._STAGED_TOKENS
    assert directory_key not in source_manifest_mod._STAGED_LOCKS


def _prepare_recovery_marker(directory):
    commit(directory, prepare(directory, MESH))
    interrupted = prepare(
        directory, dict(MESH, content_hash="xxh3:0000000000000099"))
    stage_manifest(str(directory), interrupted)
    abandon_staged_manifest(str(directory))
    marker_bytes = (directory / PENDING_MANIFEST_NAME).read_bytes()
    recovery = prepare(
        directory, dict(MESH, content_hash="xxh3:0000000000000088"))
    return recovery, marker_bytes


def test_recovery_replace_exhaustion_preserves_original_error_and_marker(
        tmp_path, monkeypatch):
    recovery, marker_bytes = _prepare_recovery_marker(tmp_path)
    directory_key = source_manifest_mod._directory_key(str(tmp_path))
    acquired = []
    original_acquire = source_manifest_mod._acquire_file_lock

    def tracked_acquire(path, owner):
        stream = original_acquire(path, owner)
        acquired.append(stream)
        return stream

    monkeypatch.setattr(
        source_manifest_mod, "_acquire_file_lock", tracked_acquire)
    replace_error = _windows_os_error(32)
    cleanup_error = _windows_os_error(5)
    replace_calls = []
    cleanup_calls = []
    sleeps = []
    original_remove = source_manifest_mod.os.remove
    with monkeypatch.context() as injected:
        def fail_replace(source, destination):
            replace_calls.append((source, destination))
            raise replace_error

        def fail_staging_cleanup(path):
            if ".writing." in os.path.basename(path):
                cleanup_calls.append(path)
                raise cleanup_error
            return original_remove(path)

        injected.setattr(source_manifest_mod.os, "replace", fail_replace)
        injected.setattr(source_manifest_mod.os, "remove", fail_staging_cleanup)
        injected.setattr(source_manifest_mod.time, "sleep", sleeps.append)
        with pytest.raises(OSError) as caught:
            stage_manifest(str(tmp_path), recovery)
    assert caught.value is replace_error
    assert len(replace_calls) == 7
    assert len(cleanup_calls) == 6
    assert sum(sleeps) == pytest.approx(0.82)
    assert (tmp_path / PENDING_MANIFEST_NAME).read_bytes() == marker_bytes
    assert all(stream.closed for stream in acquired)
    assert directory_key not in source_manifest_mod._STAGED_TOKENS
    assert directory_key not in source_manifest_mod._STAGED_LOCKS

    # Both filesystem locks were released despite replace+cleanup failure.
    stage_manifest(str(tmp_path), recovery)
    commit_staged_manifest(str(tmp_path))


def test_recovery_nontransient_replace_error_is_immediate_and_cleanup_runs(
        tmp_path, monkeypatch):
    recovery, marker_bytes = _prepare_recovery_marker(tmp_path)
    directory_key = source_manifest_mod._directory_key(str(tmp_path))
    acquired = []
    original_acquire = source_manifest_mod._acquire_file_lock

    def tracked_acquire(path, owner):
        stream = original_acquire(path, owner)
        acquired.append(stream)
        return stream

    monkeypatch.setattr(
        source_manifest_mod, "_acquire_file_lock", tracked_acquire)
    replace_error = _windows_os_error(87)
    replace_calls = []
    sleeps = []
    with monkeypatch.context() as injected:
        def fail_replace(source, destination):
            replace_calls.append((source, destination))
            raise replace_error

        injected.setattr(source_manifest_mod.os, "replace", fail_replace)
        injected.setattr(source_manifest_mod.time, "sleep", sleeps.append)
        with pytest.raises(OSError) as caught:
            stage_manifest(str(tmp_path), recovery)
    assert caught.value is replace_error
    assert len(replace_calls) == 1
    assert sleeps == []
    assert (tmp_path / PENDING_MANIFEST_NAME).read_bytes() == marker_bytes
    assert not list(tmp_path.glob(f"{PENDING_MANIFEST_NAME}.writing.*"))
    assert all(stream.closed for stream in acquired)
    assert directory_key not in source_manifest_mod._STAGED_TOKENS
    assert directory_key not in source_manifest_mod._STAGED_LOCKS


def test_reader_rejects_marker_created_during_manifest_read(tmp_path, monkeypatch):
    commit(tmp_path, prepare(tmp_path, MESH))
    original_read = source_manifest_mod._read_file_bytes
    calls = 0

    def raced_read(path):
        nonlocal calls
        payload = original_read(path)
        calls += 1
        if calls == 1:
            (tmp_path / PENDING_MANIFEST_NAME).write_bytes(b"writer marker")
        return payload

    monkeypatch.setattr(source_manifest_mod, "_read_file_bytes", raced_read)
    with pytest.raises(ManifestError, match="export in progress"):
        load_export_manifest(str(tmp_path))
    assert calls == 1


def test_snapshot_token_rejects_later_commit(tmp_path):
    commit(tmp_path, prepare(tmp_path, MESH))
    _loaded, token = load_export_manifest_snapshot(str(tmp_path))
    changed = dict(MESH, content_hash="xxh3:0000000000000099")
    commit(tmp_path, prepare(tmp_path, changed))
    with pytest.raises(ManifestError, match="changed after it was read"):
        assert_manifest_stable(str(tmp_path), token)


def test_missing_snapshot_token_rejects_manifest_appearing(tmp_path):
    document, token = load_export_manifest_snapshot(
        str(tmp_path), allow_missing=True)
    assert document is None and token is None
    commit(tmp_path, prepare(tmp_path, MESH))
    with pytest.raises(ManifestError, match="changed after it was read"):
        assert_manifest_stable(str(tmp_path), token)


def test_recovery_requires_marker_to_change_exactly_incoming_uid(tmp_path):
    commit(tmp_path, prepare(tmp_path, MESH))
    interrupted = prepare(tmp_path, dict(
        MESH, content_hash="xxh3:0000000000000099"))
    stage_manifest(str(tmp_path), interrupted)
    with pytest.raises(ManifestError, match=MESH_UID):
        prepare(tmp_path, COMPOSITE)
    recovered = prepare(tmp_path, dict(
        MESH, content_hash="xxh3:0000000000000099"))
    assert manifest_resource_index(recovered)[MESH_UID]["content_hash"] \
        == "xxh3:0000000000000099"


def test_first_export_recovery_keeps_marker_source(tmp_path):
    pending = prepare(tmp_path, MATERIAL)
    stage_manifest(str(tmp_path), pending)
    moved = dict(MATERIAL, source="other/material_c__30000000.material")
    with pytest.raises(ManifestError, match="recovery source"):
        prepare(tmp_path, moved)


def test_recovery_rejects_marker_that_also_changed_unrelated_row(tmp_path):
    commit(tmp_path, prepare(tmp_path, MESH))
    commit(tmp_path, prepare(tmp_path, COMPOSITE))
    tampered = manifest([
        dict(MESH, content_hash="xxh3:0000000000000099"),
        dict(COMPOSITE, content_hash="xxh3:0000000000000088"),
    ])
    (tmp_path / PENDING_MANIFEST_NAME).write_text(
        json.dumps(tampered, indent=2) + "\n", encoding="utf-8")
    with pytest.raises(ManifestError, match="exactly incoming UID"):
        prepare(tmp_path, dict(MESH, content_hash="xxh3:0000000000000099"))


def test_recovery_owner_scan_ignores_only_own_marker(tmp_path):
    owner = tmp_path / "owner"
    other = tmp_path / "other"
    owner.mkdir()
    other.mkdir()
    first = prepare_manifest_update(
        str(owner), resources=[MESH], exporter_version="0.4.0",
        source_root=str(tmp_path))
    commit(owner, first)
    changed = dict(MESH, content_hash="xxh3:0000000000000099")
    pending = prepare_manifest_update(
        str(owner), resources=[changed], exporter_version="0.4.0",
        source_root=str(tmp_path))
    stage_manifest(str(owner), pending)
    (other / PENDING_MANIFEST_NAME).write_text("pending", encoding="utf-8")
    with pytest.raises(ManifestError, match="pending marker"):
        prepare_manifest_update(
            str(owner), resources=[changed], exporter_version="0.4.0",
            source_root=str(tmp_path))


def test_staging_uses_lf_and_terminal_newline(tmp_path):
    stage_manifest(str(tmp_path), prepare(tmp_path, MATERIAL))
    payload = (tmp_path / PENDING_MANIFEST_NAME).read_bytes()
    assert payload.endswith(b"\n")
    assert b"\r\n" not in payload


def test_schema_version_bool_is_not_integer_v1():
    document = manifest([MESH])
    document["schema_version"] = True
    with pytest.raises(ManifestError) as caught:
        validate_export_manifest(document)
    assert caught.value.code == "MH_E_UNKNOWN_SCHEMA_VERSION"


def test_missing_manifest_can_be_optional(tmp_path):
    assert load_export_manifest(str(tmp_path), allow_missing=True) is None
