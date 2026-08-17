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
    assert_manifest_stable,
    commit_staged_manifest,
    load_export_manifest,
    load_export_manifest_snapshot,
    manifest_resource_index,
    prepare_manifest_update,
    stage_manifest,
    validate_export_manifest,
)


MESH_A = {
    "uid": "10000000-0000-0000-0000-000000000001",
    "kind": "static_mesh",
    "name": "mesh_a",
    "source": "mesh_a.fbx",
    "content_hash": "xxh3:0000000000000001",
}
COMPOSITE_B = {
    "uid": "20000000-0000-0000-0000-000000000002",
    "kind": "composite",
    "name": "composite_b",
    "source": "composite_b.composite",
    "content_hash": "xxh3:0000000000000002",
}
MATERIAL_C = {
    "uid": "30000000-0000-0000-0000-000000000003",
    "kind": "material",
    "name": "material_c",
    "shader_class": "rendinst_simple",
    "params": {},
    "textures": {},
    "content_hash": "xxh3:0000000000000003",
}


def prepare(directory, **kwargs):
    return prepare_manifest_update(
        str(directory), exporter_version="0.3.0", blend_file="scene.blend",
        **kwargs)


def test_incremental_updates_preserve_unrelated_resources(tmp_path):
    first = prepare(tmp_path, resources=[MESH_A])
    stage_manifest(str(tmp_path), first)
    commit_staged_manifest(str(tmp_path))

    second = prepare(tmp_path, resources=[COMPOSITE_B])
    assert [row["uid"] for row in second["resources"]] == [
        MESH_A["uid"], COMPOSITE_B["uid"]]
    assert manifest_resource_index(second)[MESH_A["uid"]] == MESH_A


def test_pending_marker_blocks_read_until_promoted(tmp_path):
    document = prepare(tmp_path, resources=[MESH_A])
    stage_manifest(str(tmp_path), document)
    assert (tmp_path / PENDING_MANIFEST_NAME).exists()
    with pytest.raises(ManifestError, match="export in progress"):
        load_export_manifest(str(tmp_path))
    commit_staged_manifest(str(tmp_path))
    assert not (tmp_path / PENDING_MANIFEST_NAME).exists()
    assert load_export_manifest(str(tmp_path))["resources"] == [MESH_A]


def test_reader_rejects_marker_created_during_manifest_read(
        tmp_path, monkeypatch):
    document = prepare(tmp_path, resources=[MESH_A])
    stage_manifest(str(tmp_path), document)
    commit_staged_manifest(str(tmp_path))

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


def test_reader_rejects_manifest_bytes_replaced_without_visible_marker(
        tmp_path, monkeypatch):
    document = prepare(tmp_path, resources=[MESH_A])
    stage_manifest(str(tmp_path), document)
    commit_staged_manifest(str(tmp_path))
    replacement = prepare(tmp_path, resources=[COMPOSITE_B])
    replacement_bytes = (
        json.dumps(replacement, indent=2, ensure_ascii=False) + "\n"
    ).encode("utf-8")

    original_read = source_manifest_mod._read_file_bytes
    calls = 0

    def raced_read(path):
        nonlocal calls
        payload = original_read(path)
        calls += 1
        if calls == 1:
            (tmp_path / MANIFEST_NAME).write_bytes(replacement_bytes)
        return payload

    monkeypatch.setattr(source_manifest_mod, "_read_file_bytes", raced_read)
    with pytest.raises(ManifestError, match="changed while reading"):
        load_export_manifest(str(tmp_path))
    assert calls == 2


def test_snapshot_token_rejects_export_started_during_payload_reads(tmp_path):
    document = prepare(tmp_path, resources=[MESH_A])
    stage_manifest(str(tmp_path), document)
    commit_staged_manifest(str(tmp_path))
    loaded, token = load_export_manifest_snapshot(str(tmp_path))
    assert loaded["resources"] == [MESH_A]

    # Models the consumer having read one or more composite/FBX payloads,
    # followed by a writer beginning another transaction before Blender apply.
    changed = prepare(
        tmp_path,
        resources=[dict(MESH_A, content_hash="xxh3:0000000000000099")])
    stage_manifest(str(tmp_path), changed)
    with pytest.raises(ManifestError, match="export in progress"):
        assert_manifest_stable(str(tmp_path), token)


def test_snapshot_token_rejects_commit_completed_during_payload_reads(tmp_path):
    document = prepare(tmp_path, resources=[MESH_A])
    stage_manifest(str(tmp_path), document)
    commit_staged_manifest(str(tmp_path))
    _loaded, token = load_export_manifest_snapshot(str(tmp_path))

    changed = prepare(
        tmp_path,
        resources=[dict(MESH_A, content_hash="xxh3:0000000000000099")])
    stage_manifest(str(tmp_path), changed)
    commit_staged_manifest(str(tmp_path))
    with pytest.raises(ManifestError, match="changed after it was read"):
        assert_manifest_stable(str(tmp_path), token)


def test_missing_snapshot_token_rejects_manifest_appearing_later(tmp_path):
    document, token = load_export_manifest_snapshot(
        str(tmp_path), allow_missing=True)
    assert document is None and token is None

    created = prepare(tmp_path, resources=[MESH_A])
    stage_manifest(str(tmp_path), created)
    commit_staged_manifest(str(tmp_path))
    with pytest.raises(ManifestError, match="changed after it was read"):
        assert_manifest_stable(str(tmp_path), token)


def test_staging_uses_lf_and_terminal_newline(tmp_path):
    document = prepare(tmp_path, resources=[MESH_A])
    stage_manifest(str(tmp_path), document)
    payload = (tmp_path / PENDING_MANIFEST_NAME).read_bytes()
    assert payload.endswith(b"\n")
    assert b"\r\n" not in payload


def test_legacy_bundle_manifest_is_migrated_on_next_update(tmp_path):
    legacy = {
        "schema": "mh.bundle_manifest",
        "schema_version": 2,
        "exporter_version": "0.2.0",
        "bundle_uid": "ignored",
        "bundle_name": "ignored",
        "source": {"blend_file": "old.blend"},
        "resources": [MESH_A],
        "materials": [],
        "external_dependencies": [],
    }
    (tmp_path / MANIFEST_NAME).write_text(json.dumps(legacy), encoding="utf-8")
    updated = prepare(tmp_path, resources=[COMPOSITE_B])
    assert updated["schema"] == "mh.export_manifest"
    assert "bundle_uid" not in updated
    assert {r["uid"] for r in updated["resources"]} == {
        MESH_A["uid"], COMPOSITE_B["uid"]}


def test_duplicate_rows_in_one_update_are_rejected(tmp_path):
    with pytest.raises(ManifestError, match="duplicate uid"):
        prepare(tmp_path, resources=[MESH_A, dict(MESH_A)])


@pytest.mark.parametrize(("field", "value"), [
    ("uid", 7),
    ("kind", 7),
    ("name", None),
    ("source", 7),
    ("content_hash", False),
])
def test_incoming_resource_requires_strict_writer_fields(
        tmp_path, field, value):
    malformed = dict(MESH_A, **{field: value})
    with pytest.raises(ManifestError):
        prepare(tmp_path, resources=[malformed])


def test_malformed_stable_resource_fails_before_incoming_merge(tmp_path):
    malformed = {
        "schema": "mh.export_manifest",
        "schema_version": 1,
        "exporter_version": "0.3.0",
        "source": {"blend_file": "old.blend"},
        "resources": [{
            key: value for key, value in MESH_A.items()
            if key != "content_hash"
        }],
        "materials": [],
    }
    (tmp_path / MANIFEST_NAME).write_text(
        json.dumps(malformed), encoding="utf-8")
    with pytest.raises(ManifestError, match="content_hash"):
        prepare(tmp_path, resources=[COMPOSITE_B])
    assert not (tmp_path / PENDING_MANIFEST_NAME).exists()


def test_schema_version_bool_is_not_integer_v1(tmp_path):
    malformed = prepare(tmp_path, resources=[MESH_A])
    malformed["schema_version"] = True
    with pytest.raises(ManifestError, match="schema_version"):
        stage_manifest(str(tmp_path), malformed)


def test_static_mesh_material_slots_are_strict_and_cross_referenced(tmp_path):
    mesh = dict(MESH_A, material_slots=[{
        "slot_name": "slot_a", "material_uid": MATERIAL_C["uid"]}])
    document = prepare(
        tmp_path, resources=[mesh], materials=[MATERIAL_C])
    assert document["resources"][0]["material_slots"] == [{
        "slot_name": "slot_a", "material_uid": MATERIAL_C["uid"]}]


@pytest.mark.parametrize("slots", [
    {},
    ["not-an-object"],
    [{"slot_name": "slot_a"}],
    [{"slot_name": 1, "material_uid": MATERIAL_C["uid"]}],
    [{"slot_name": "slot_a", "material_uid": 1}],
])
def test_static_mesh_rejects_malformed_material_slots(tmp_path, slots):
    mesh = dict(MESH_A, material_slots=slots)
    with pytest.raises(ManifestError, match="material_slot|material slot"):
        prepare(tmp_path, resources=[mesh], materials=[MATERIAL_C])


def test_static_mesh_rejects_duplicate_normalized_slot_names(tmp_path):
    mesh = dict(MESH_A, material_slots=[
        {"slot_name": "m\u00e9tal", "material_uid": MATERIAL_C["uid"]},
        {"slot_name": "me\u0301tal", "material_uid": MATERIAL_C["uid"]},
    ])
    with pytest.raises(ManifestError, match="duplicate material slot_name"):
        prepare(tmp_path, resources=[mesh], materials=[MATERIAL_C])


def test_static_mesh_rejects_missing_material_reference(tmp_path):
    mesh = dict(MESH_A, material_slots=[{
        "slot_name": "slot_a", "material_uid": MATERIAL_C["uid"]}])
    with pytest.raises(ManifestError, match="references missing material"):
        prepare(tmp_path, resources=[mesh])


def test_reader_tolerates_dangling_material_for_importer_warning(tmp_path):
    mesh = dict(MESH_A, material_slots=[{
        "slot_name": "missing_slot", "material_uid": MATERIAL_C["uid"]}])
    document = {
        "schema": "mh.export_manifest",
        "schema_version": 1,
        "exporter_version": "0.3.0",
        "source": {"blend_file": "legacy.blend"},
        "resources": [mesh],
        "materials": [],
    }
    # A reader accepts structurally valid legacy/external incompleteness so
    # the importer can surface MH_W_MATERIAL_NOT_FOUND and keep the mesh.
    (tmp_path / MANIFEST_NAME).write_text(
        json.dumps(document), encoding="utf-8")
    loaded = load_export_manifest(str(tmp_path))
    assert loaded["resources"][0]["material_slots"] == mesh["material_slots"]
    snap, token = load_export_manifest_snapshot(str(tmp_path))
    assert snap == loaded and isinstance(token, bytes)
    assert manifest_resource_index(loaded)[MESH_A["uid"]]["kind"] \
        == "static_mesh"


def test_explicit_strict_reference_validation_rejects_dangling_material():
    mesh = dict(MESH_A, material_slots=[{
        "slot_name": "missing_slot", "material_uid": MATERIAL_C["uid"]}])
    document = {
        "schema": "mh.export_manifest",
        "schema_version": 1,
        "exporter_version": "0.3.0",
        "source": {},
        "resources": [mesh],
        "materials": [],
    }
    assert validate_export_manifest(document)["resources"] == [mesh]
    with pytest.raises(ManifestError, match="references missing material"):
        validate_export_manifest(document, strict_references=True)


def test_stage_and_update_keep_writer_references_strict(tmp_path):
    mesh = dict(MESH_A, material_slots=[{
        "slot_name": "missing_slot", "material_uid": MATERIAL_C["uid"]}])
    document = {
        "schema": "mh.export_manifest",
        "schema_version": 1,
        "exporter_version": "0.3.0",
        "source": {},
        "resources": [mesh],
        "materials": [],
    }
    with pytest.raises(ManifestError, match="references missing material"):
        stage_manifest(str(tmp_path), document)
    assert not (tmp_path / PENDING_MANIFEST_NAME).exists()

    # Even if an incomplete manifest came from an older/external writer, the
    # next local writer refuses to preserve it into a new staged transaction.
    (tmp_path / MANIFEST_NAME).write_text(
        json.dumps(document), encoding="utf-8")
    with pytest.raises(ManifestError, match="references missing material"):
        prepare(tmp_path, resources=[COMPOSITE_B])


def test_reader_still_rejects_malformed_slot_shape(tmp_path):
    malformed = dict(MESH_A, material_slots={})
    document = {
        "schema": "mh.export_manifest",
        "schema_version": 1,
        "exporter_version": "0.3.0",
        "source": {},
        "resources": [malformed],
        "materials": [],
    }
    (tmp_path / MANIFEST_NAME).write_text(
        json.dumps(document), encoding="utf-8")
    with pytest.raises(ManifestError, match="material_slots must be an array"):
        load_export_manifest(str(tmp_path))


def test_composite_rejects_mesh_only_material_slots(tmp_path):
    malformed = dict(COMPOSITE_B, material_slots=[])
    with pytest.raises(ManifestError, match="cannot contain material_slots"):
        prepare(tmp_path, resources=[malformed])


@pytest.mark.parametrize(("field", "value"), [
    ("uid", 7),
    ("kind", "static_mesh"),
    ("name", None),
    ("content_hash", 7),
    ("shader_class", ""),
    ("params", []),
    ("textures", []),
])
def test_incoming_material_requires_strict_writer_fields(
        tmp_path, field, value):
    malformed = dict(MATERIAL_C, **{field: value})
    with pytest.raises(ManifestError):
        prepare(tmp_path, materials=[malformed])


def test_material_payload_is_normalized_before_return_and_stage(tmp_path):
    material = dict(
        MATERIAL_C,
        shader_class="re\u0301ndinst_simple",
        params={"roughness": 0.2500004},
        textures={"te\u0301x0": "A:\\textures\\metal_d.tif"},
    )
    document = prepare(tmp_path, materials=[material])
    row = document["materials"][0]
    assert row["shader_class"] == "r\u00e9ndinst_simple"
    assert row["params"] == {"roughness": 0.25}
    assert row["textures"] == {"t\u00e9x0": "A:\\textures\\metal_d.tif"}


def test_material_rejects_non_string_texture_path(tmp_path):
    malformed = dict(MATERIAL_C, textures={"tex0": 123})
    with pytest.raises(ManifestError, match="texture slots and paths"):
        prepare(tmp_path, materials=[malformed])


def test_material_rejects_non_finite_nested_param(tmp_path):
    malformed = dict(MATERIAL_C, params={"nested": [float("nan")]})
    with pytest.raises(ManifestError, match="invalid payload"):
        prepare(tmp_path, materials=[malformed])


def test_two_resource_uids_cannot_own_one_source_path(tmp_path):
    collision = dict(COMPOSITE_B, source=MESH_A["source"])
    with pytest.raises(ManifestError, match="source .* is shared"):
        prepare(tmp_path, resources=[MESH_A, collision])


@pytest.mark.parametrize("first, alias", [
    ("payload/mesh_a.fbx", "./payload/mesh_a.fbx"),
    ("payload/mesh_a.fbx", "payload\\mesh_a.fbx"),
    ("payload/mesh_a.fbx", "PAYLOAD/MESH_A.FBX"),
])
def test_source_ownership_rejects_relative_and_windows_aliases(
        tmp_path, first, alias):
    mesh = dict(MESH_A, source=first)
    collision = dict(COMPOSITE_B, source=alias)
    with pytest.raises(ManifestError, match="source .* is shared"):
        prepare(tmp_path, resources=[mesh, collision])


def test_relative_source_is_stored_in_canonical_slash_form(tmp_path):
    aliased = dict(MESH_A, source="./payload\\temp/../mesh_a.fbx")
    document = prepare(tmp_path, resources=[aliased])
    assert document["resources"][0]["source"] == "payload/mesh_a.fbx"
    stage_manifest(str(tmp_path), document)
    commit_staged_manifest(str(tmp_path))
    assert load_export_manifest(str(tmp_path))["resources"][0]["source"] \
        == "payload/mesh_a.fbx"


def test_existing_resource_uid_cannot_change_kind(tmp_path):
    stable = prepare(tmp_path, resources=[MESH_A])
    stage_manifest(str(tmp_path), stable)
    commit_staged_manifest(str(tmp_path))
    changed_kind = dict(COMPOSITE_B, uid=MESH_A["uid"])
    with pytest.raises(ManifestError, match="cannot change kind"):
        prepare(tmp_path, resources=[changed_kind])


def test_resource_and_material_uid_spaces_cannot_overlap(tmp_path):
    material = {
        "uid": MESH_A["uid"],
        "kind": "material",
        "name": "m",
        "shader_class": "rendinst_simple",
        "params": {},
        "textures": {},
        "content_hash": "xxh3:0000000000000003",
    }
    with pytest.raises(ManifestError, match="resource and material"):
        prepare(tmp_path, resources=[MESH_A], materials=[material])


def test_missing_manifest_can_be_optional(tmp_path):
    assert load_export_manifest(str(tmp_path), allow_missing=True) is None


def test_interrupted_resource_must_be_reexported_before_unrelated_uid(tmp_path):
    stable = prepare(tmp_path, resources=[MESH_A])
    stage_manifest(str(tmp_path), stable)
    commit_staged_manifest(str(tmp_path))

    changed_a = dict(MESH_A, content_hash="xxh3:0000000000000099")
    pending = prepare(tmp_path, resources=[changed_a])
    stage_manifest(str(tmp_path), pending)

    with pytest.raises(ManifestError, match=MESH_A["uid"]):
        prepare(tmp_path, resources=[COMPOSITE_B])

    recovered = prepare(tmp_path, resources=[changed_a])
    assert manifest_resource_index(recovered)[MESH_A["uid"]] == changed_a


def test_first_interrupted_resource_cannot_recover_as_another_kind(tmp_path):
    pending = prepare(tmp_path, resources=[COMPOSITE_B])
    stage_manifest(str(tmp_path), pending)
    changed_kind = dict(
        MESH_A, uid=COMPOSITE_B["uid"], source="composite_b.mesh.fbx")
    with pytest.raises(ManifestError, match="cannot change kind from interrupted"):
        prepare(tmp_path, resources=[changed_kind])


def test_inline_material_only_pending_state_does_not_block_other_export(tmp_path):
    stable = prepare(tmp_path, resources=[MESH_A])
    stage_manifest(str(tmp_path), stable)
    commit_staged_manifest(str(tmp_path))
    material = {
        "uid": "30000000-0000-0000-0000-000000000003",
        "kind": "material",
        "name": "m",
        "shader_class": "rendinst_simple",
        "params": {"roughness": 0.5},
        "textures": {},
        "content_hash": "xxh3:0000000000000003",
    }
    pending = prepare(tmp_path, materials=[material])
    stage_manifest(str(tmp_path), pending)

    unrelated = prepare(tmp_path, resources=[COMPOSITE_B])
    assert unrelated["materials"] == []


def test_top_level_blend_file_is_kept_for_one_source_file(tmp_path):
    stable = prepare(tmp_path, resources=[MESH_A])
    stage_manifest(str(tmp_path), stable)
    commit_staged_manifest(str(tmp_path))

    updated = prepare(tmp_path, resources=[COMPOSITE_B])
    assert updated["source"] == {"blend_file": "scene.blend"}


def test_top_level_blend_file_is_removed_for_mixed_directory_index(tmp_path):
    stable = prepare(tmp_path, resources=[MESH_A])
    stage_manifest(str(tmp_path), stable)
    commit_staged_manifest(str(tmp_path))

    mixed = prepare_manifest_update(
        str(tmp_path), resources=[COMPOSITE_B], exporter_version="0.3.0",
        blend_file="other_scene.blend")
    assert mixed["source"] == {}
    stage_manifest(str(tmp_path), mixed)
    commit_staged_manifest(str(tmp_path))

    # Unknown/mixed provenance is sticky; a later single-resource update must
    # not claim the whole directory for its own authoring file again.
    later = prepare(tmp_path, resources=[MESH_A])
    assert later["source"] == {}
