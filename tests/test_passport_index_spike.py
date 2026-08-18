import json
from pathlib import Path
import shutil
import uuid

import pytest

from tools.spikes.passport_index import (
    PROVISIONAL_EMPTY_REBUILD_NOTE,
    PayloadObservation,
    canonical_path,
    exact_payload_fingerprint,
    fork_document,
    local_index_path,
    rebuild_index,
)


def _passport(
    resource_uid,
    *,
    lod_levels=(0,),
    material_uids=(),
    schema_version=1,
    deprecated_lod_level=None,
):
    document = {
        "schema": "mh.fbx_passport",
        "schema_version": schema_version,
        "resource_uid": str(resource_uid),
        "kind": "static_mesh",
        "name": "wall_a",
        "lod_policy": "authored" if len(lod_levels) > 1 else "generated",
        "geometry_hash": "xxh3:0123456789abcdef",
        "material_slots": [
            {
                "slot_name": f"slot_{index}",
                "material_uid": str(material_uid),
                "material_name_hint": f"m_{index}",
            }
            for index, material_uid in enumerate(material_uids)
        ],
        "properties": {},
        "exporter": "G3 test",
    }
    if deprecated_lod_level is None:
        document["lod_levels"] = list(lod_levels)
    else:
        document["lod_level"] = deprecated_lod_level
    return json.dumps(document, sort_keys=True, separators=(",", ":"))


def _payload(path: Path, data: bytes, passport: str | None):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return PayloadObservation.from_text(path, passport)


def test_local_index_path_inventory_is_host_local_and_project_scoped(tmp_path):
    project_uid = str(uuid.uuid4())
    source_root = tmp_path / "source"
    result = local_index_path(
        project_uid,
        platform_name="win32",
        local_appdata=tmp_path / "LocalAppData",
    )

    assert result == (
        tmp_path / "LocalAppData" / "MimirHead" / "MHBridge"
        / project_uid / "index.json"
    )
    assert source_root not in result.parents
    assert local_index_path(
        project_uid, platform_name="darwin", home=tmp_path / "mac-home"
    ) == (
        tmp_path / "mac-home" / "Library" / "Caches" / "MimirHead"
        / "MHBridge" / project_uid / "index.json"
    )
    assert local_index_path(
        project_uid,
        platform_name="linux",
        home=tmp_path / "linux-home",
        xdg_cache_home=tmp_path / "xdg-cache",
    ) == (
        tmp_path / "xdg-cache" / "MimirHead" / "MHBridge"
        / project_uid / "index.json"
    )


def test_exact_fingerprint_keeps_byte_hash_and_uses_stat_only_as_fast_path(tmp_path):
    payload = tmp_path / "asset.fbx"
    payload.write_bytes(b"revision-one")
    first = exact_payload_fingerprint(payload)
    second = exact_payload_fingerprint(payload, cached_row=first)
    payload.write_bytes(b"revision-two-is-longer")
    third = exact_payload_fingerprint(payload, cached_row=second)

    assert first["payload_fingerprint"].startswith("sha256:")
    assert first["fingerprint_fast_path"] is False
    assert second["fingerprint_fast_path"] is True
    assert second["payload_fingerprint"] == first["payload_fingerprint"]
    assert third["fingerprint_fast_path"] is False
    assert third["payload_fingerprint"] != first["payload_fingerprint"]


def test_one_combined_lod_payload_is_one_uid_candidate_and_reports_material(tmp_path):
    uid = uuid.uuid4()
    material_uid = uuid.uuid4()
    observation = _payload(
        tmp_path / "wall.fbx",
        b"combined-lod-fbx",
        _passport(uid, lod_levels=(0, 1, 2), material_uids=(material_uid,)),
    )

    missing = rebuild_index([observation])
    complete = rebuild_index(
        [observation], known_material_uids={str(material_uid)}
    )

    row = missing["uids"][str(uid)]
    assert row["resolution_status"] == "adopt_new"
    assert row["lod_levels"] == [0, 1, 2]
    assert row["model_lod_verification"] == "carrier_reader_responsibility"
    assert row["material_status"] == "missing"
    assert row["missing_material_uids"] == [str(material_uid)]
    assert complete["uids"][str(uid)]["material_status"] == "complete"


def test_partial_lod_declaration_is_quarantined_and_per_file_lod_is_deprecated(tmp_path):
    uid_a = uuid.uuid4()
    uid_b = uuid.uuid4()
    partial = _payload(
        tmp_path / "partial.fbx", b"partial", _passport(uid_a, lod_levels=(0, 2))
    )
    deprecated = _payload(
        tmp_path / "old-lod1.fbx",
        b"old",
        _passport(uid_b, deprecated_lod_level=1),
    )

    index = rebuild_index([partial, deprecated])
    partial_row = index["paths"][canonical_path(partial.path)]
    deprecated_row = index["paths"][canonical_path(deprecated.path)]

    assert partial_row["parse_status"] == "malformed"
    assert partial_row["quarantined"] is True
    assert "contiguous [0..N]" in partial_row["diagnostics"][0]
    assert deprecated_row["parse_status"] == "deprecated_per_lod_passport"
    assert deprecated_row["quarantined"] is False
    assert canonical_path(deprecated.path) in index["legacy_paths"]
    assert index["uids"] == {}


def test_deprecated_lod_level_is_rejected_even_beside_combined_lod_levels(
        tmp_path):
    uid = uuid.uuid4()
    document = json.loads(_passport(uid, lod_levels=(0, 1)))
    document["lod_level"] = 0
    observation = _payload(
        tmp_path / "mixed-passport.fbx",
        b"mixed legacy and combined fields",
        json.dumps(document, sort_keys=True, separators=(",", ":")),
    )

    index = rebuild_index([observation])
    row = index["paths"][canonical_path(observation.path)]

    assert row["parse_status"] == "deprecated_per_lod_passport"
    assert row["parsed_passport"] is None
    assert "DEPRECATED_PER_LOD" in row["diagnostics"][0]
    assert canonical_path(observation.path) in index["legacy_paths"]
    assert index["uids"] == {}


def test_missing_malformed_unknown_version_and_carrier_disagreement_statuses(tmp_path):
    missing = _payload(tmp_path / "legacy.fbx", b"legacy", None)
    malformed = PayloadObservation(
        tmp_path / "malformed.fbx", ("{",)
    )
    malformed.path.write_bytes(b"malformed")
    unsupported = _payload(
        tmp_path / "future.fbx",
        b"future",
        _passport(uuid.uuid4(), schema_version=99),
    )
    valid = _passport(uuid.uuid4())
    disagreement = PayloadObservation(
        tmp_path / "copies-differ.fbx", (valid, valid + " ")
    )
    disagreement.path.write_bytes(b"copies-differ")

    index = rebuild_index([missing, malformed, unsupported, disagreement])
    statuses = {
        Path(path).name: row["parse_status"] for path, row in index["paths"].items()
    }

    assert statuses == {
        "legacy.fbx": "legacy_missing_passport",
        "malformed.fbx": "malformed",
        "future.fbx": "unsupported_version",
        "copies-differ.fbx": "malformed",
    }
    assert len(index["legacy_paths"]) == 1
    assert len(index["quarantine_paths"]) == 3


def test_move_and_identical_duplicate_preserve_prior_resolution(tmp_path):
    uid = uuid.uuid4()
    passport = _passport(uid, lod_levels=(0, 1))
    old = _payload(tmp_path / "old" / "wall.fbx", b"same", passport)
    initial = rebuild_index([old])

    moved_path = tmp_path / "new" / "wall.fbx"
    moved_path.parent.mkdir()
    shutil.move(old.path, moved_path)
    moved = PayloadObservation.from_text(moved_path, passport)
    moved_index = rebuild_index([moved], previous_index=initial)
    assert moved_index["uids"][str(uid)]["resolution_status"] == "moved"
    assert moved_index["uids"][str(uid)]["resolved_path"] == canonical_path(
        moved_path
    )

    duplicate_path = tmp_path / "copy" / "wall.fbx"
    duplicate_path.parent.mkdir()
    shutil.copy2(moved_path, duplicate_path)
    duplicate = PayloadObservation.from_text(duplicate_path, passport)
    duplicate_index = rebuild_index(
        [moved, duplicate], previous_index=moved_index
    )
    row = duplicate_index["uids"][str(uid)]
    assert row["resolution_status"] == "duplicate_copy_warning"
    assert row["resolved_path"] == canonical_path(moved_path)
    assert row["resolution_basis"] == "preserved_prior_resolved_path"
    assert duplicate_index["provisional_resolutions"] == []


def test_empty_rebuild_duplicate_is_provisional_and_divergence_is_hard(tmp_path):
    uid = uuid.uuid4()
    passport = _passport(uid)
    a = _payload(tmp_path / "a" / "wall.fbx", b"same", passport)
    b = _payload(tmp_path / "b" / "wall.fbx", b"same", passport)

    identical = rebuild_index([b, a])
    row = identical["uids"][str(uid)]
    assert row["resolution_status"] == "duplicate_copy_warning"
    assert row["resolved_path"] == min(
        (canonical_path(a.path), canonical_path(b.path)),
        key=lambda value: (value.casefold(), value),
    )
    assert identical["provisional_resolutions"][0]["note"] == (
        PROVISIONAL_EMPTY_REBUILD_NOTE
    )

    b.path.write_bytes(b"divergent")
    divergent = rebuild_index([a, b], previous_index=identical)
    conflict = divergent["uids"][str(uid)]
    assert conflict["resolution_status"] == "hard_conflict"
    assert conflict["resolved_path"] is None
    assert any("MH_E_DIVERGENT_REVISIONS" in item for item in conflict["diagnostics"])


def test_fork_is_explicit_deep_copy_and_can_rewrite_selected_composite_links():
    original_uid = str(uuid.uuid4())
    new_uid = str(uuid.uuid4())
    old_dependency = str(uuid.uuid4())
    new_dependency = str(uuid.uuid4())
    untouched_dependency = str(uuid.uuid4())
    original = {
        "schema": "mh.composite",
        "schema_version": 2,
        "kind": "composite",
        "uid": original_uid,
        "nodes": [
            {"resource_uid": old_dependency, "children": []},
            {"resource_uid": untouched_dependency, "children": []},
        ],
    }

    forked = fork_document(
        original,
        new_resource_uid=new_uid,
        dependency_uid_replacements={old_dependency: new_dependency},
    )

    assert original["uid"] == original_uid
    assert original["nodes"][0]["resource_uid"] == old_dependency
    assert forked["uid"] == new_uid
    assert forked["nodes"][0]["resource_uid"] == new_dependency
    assert forked["nodes"][1]["resource_uid"] == untouched_dependency


@pytest.mark.parametrize("bad_uid", ["", "not-a-uuid"])
def test_local_index_path_rejects_non_uid_directory_names(bad_uid):
    with pytest.raises(ValueError, match="project_uid"):
        local_index_path(bad_uid)
