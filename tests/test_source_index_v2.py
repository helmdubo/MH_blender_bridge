from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import sys
import uuid

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core import source_index_v2 as index_v2  # noqa: E402


def _passport(uid, *, lod_levels=(0,), material_uids=(), extra=None):
    document = {
        "schema": "mh.fbx_passport",
        "schema_version": 1,
        "resource_uid": str(uid),
        "kind": "static_mesh",
        "name": "wall_a",
        "lod_levels": list(lod_levels),
        "lod_policy": "authored" if len(lod_levels) > 1 else "generated",
        "geometry_hash": "xxh3:0123456789abcdef",
        "material_slots": [{
            "slot_name": f"slot_{index}",
            "material_uid": str(material_uid),
            "material_name_hint": f"material_{index}",
        } for index, material_uid in enumerate(material_uids)],
        "properties": {},
        "exporter": "mh4blend test",
    }
    document.update(extra or {})
    return json.dumps(
        document, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _material(uid, name="wall"):
    return {
        "schema": "mh.material",
        "schema_version": 1,
        "uid": str(uid),
        "name": name,
        "shader_class": "rendinst_simple",
        "params": {},
        "textures": {},
    }


def _composite(uid, name="assembly", *, version=2):
    result = {
        "schema": "mh.composite",
        "schema_version": version,
        "uid": str(uid),
        "name": name,
        "nodes": [],
    }
    if version == 2:
        result["properties"] = {}
    return result


def _write_json(path, document):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document), encoding="utf-8")
    return path


def _write_fbx(path, payload=b"Kaydara FBX Binary probe"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    return path


def _extractor(passports_by_name):
    def extract(path):
        value = passports_by_name.get(path.name)
        if value is None:
            return ()
        return value if isinstance(value, tuple) else (value,)
    return extract


def test_project_uid_and_cache_path_are_deterministic_host_local(tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    first = index_v2.project_uid_for_source_root(root)
    second = index_v2.project_uid_for_source_root(root / ".")

    assert first == second == str(uuid.UUID(first))
    path = index_v2.default_index_path(
        first, platform_name="win32",
        local_appdata=tmp_path / "LocalAppData")
    assert path == (
        tmp_path / "LocalAppData" / "MimirHead" / "MHBridge" / first /
        "index.json")
    assert root not in path.parents


def test_source_inventory_token_rejects_payload_path_appearance(tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    _write_json(root / "wall.material", _material(uuid.uuid4()))
    token = index_v2.capture_source_inventory(root)
    index_v2.assert_source_inventory_stable(token)

    # Plain FBX is not an MH payload and does not invalidate the token.
    _write_fbx(root / "reference.fbx")
    index_v2.assert_source_inventory_stable(token)
    _write_json(root / "copy.material", _material(uuid.uuid4()))
    with pytest.raises(index_v2.SourceIndexV2Error) as caught:
        index_v2.assert_source_inventory_stable(token)
    assert caught.value.code == "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED"


def test_full_scan_indexes_fbx_material_and_composite_without_manifests(tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    mesh_uid, material_uid, composite_uid = (
        uuid.uuid4(), uuid.uuid4(), uuid.uuid4())
    fbx = _write_fbx(root / "meshes" / "wall.mesh.fbx")
    material = _write_json(
        root / "materials" / "wall.material", _material(material_uid))
    composite = _write_json(
        root / "composites" / "assembly.composite",
        _composite(composite_uid))
    # Plain FBX files are outside the MH v2 payload namespace.
    _write_fbx(root / "references" / "raw_export.fbx")
    # A hard-crash sibling temp is explicitly not source authority.
    _write_fbx(root / "meshes" / ".ignored.mesh.fbx.mh-tmp-12-deadbeef")

    index = index_v2.scan_source_root(
        root, fbx_passport_extractor=_extractor({
            fbx.name: _passport(
                mesh_uid, lod_levels=(0, 1),
                material_uids=(material_uid,)),
        }))

    assert set(index["uids"]) == {
        str(mesh_uid), str(material_uid), str(composite_uid)}
    assert len(index["paths"]) == 3
    assert index["uids"][str(mesh_uid)]["kind"] == "static_mesh"
    assert index["uids"][str(mesh_uid)]["material_status"] == "complete"
    assert index["uids"][str(material_uid)]["kind"] == "material"
    assert index["uids"][str(composite_uid)]["kind"] == "composite"
    for path in (fbx, material, composite):
        row = index["paths"][index_v2.canonical_payload_path(path)]
        assert row["payload_fingerprint"].startswith("sha256:")
        assert row["parsed_passport"]["resource_uid"] in index["uids"]


def test_carrier_copies_must_match_and_deprecated_lod_level_never_candidates(
        tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    disagree_uid = uuid.uuid4()
    deprecated_uid = uuid.uuid4()
    disagree = _write_fbx(root / "disagree.mesh.fbx", b"one")
    deprecated = _write_fbx(root / "deprecated.mesh.fbx", b"two")
    mixed = json.loads(_passport(deprecated_uid, lod_levels=(0, 1)))
    mixed["lod_level"] = 1
    extractor = _extractor({
        disagree.name: (
            _passport(disagree_uid), _passport(disagree_uid) + " "),
        deprecated.name: json.dumps(mixed, sort_keys=True, separators=(",", ":")),
    })

    index = index_v2.scan_source_root(
        root, fbx_passport_extractor=extractor)

    disagree_row = index["paths"][index_v2.canonical_payload_path(disagree)]
    deprecated_row = index["paths"][index_v2.canonical_payload_path(deprecated)]
    assert disagree_row["parse_status"] == "malformed"
    assert disagree_row["quarantined"] is True
    assert deprecated_row["parse_status"] == "deprecated_per_lod_passport"
    assert deprecated_row["parsed_passport"] is None
    assert index["uids"] == {}


def test_missing_material_dependency_is_index_diagnostic(tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    mesh_uid, material_uid = uuid.uuid4(), uuid.uuid4()
    fbx = _write_fbx(root / "wall.mesh.fbx")

    index = index_v2.scan_source_root(
        root, fbx_passport_extractor=_extractor({
            fbx.name: _passport(mesh_uid, material_uids=(material_uid,)),
        }))

    row = index["uids"][str(mesh_uid)]
    assert row["material_status"] == "missing"
    assert row["missing_material_uids"] == [str(material_uid)]
    assert any("MH_W_MATERIAL_NOT_FOUND" in item
               for item in row["diagnostics"])


def test_move_requires_old_path_missing_and_one_new_candidate(tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    uid = uuid.uuid4()
    passport = _passport(uid)
    old = _write_fbx(root / "old" / "wall.mesh.fbx", b"same")
    extractor = _extractor({"wall.mesh.fbx": passport})
    initial = index_v2.scan_source_root(
        root, fbx_passport_extractor=extractor)

    moved = root / "new" / "wall.mesh.fbx"
    moved.parent.mkdir()
    shutil.move(old, moved)
    rebuilt = index_v2.scan_source_root(
        root, fbx_passport_extractor=extractor, previous_index=initial)
    row = rebuilt["uids"][str(uid)]

    assert row["resolution_status"] == "moved"
    assert row["resolved_path"] == index_v2.canonical_payload_path(moved)


def test_existing_identical_duplicate_warns_but_divergence_is_hard(tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    uid = uuid.uuid4()
    passport = _passport(uid)
    a = _write_fbx(root / "a" / "wall_a.mesh.fbx", b"same")
    extractor = _extractor({"wall_a.mesh.fbx": passport, "wall_b.mesh.fbx": passport})
    initial = index_v2.scan_source_root(
        root, fbx_passport_extractor=extractor)
    b = _write_fbx(root / "b" / "wall_b.mesh.fbx", b"same")

    duplicate = index_v2.scan_source_root(
        root, fbx_passport_extractor=extractor, previous_index=initial)
    duplicate_row = duplicate["uids"][str(uid)]
    assert duplicate_row["resolution_status"] == "duplicate_copy_warning"
    assert duplicate_row["resolved_path"] == index_v2.canonical_payload_path(a)

    b.write_bytes(b"divergent revision")
    conflict = index_v2.scan_source_root(
        root, fbx_passport_extractor=extractor, previous_index=duplicate)
    conflict_row = conflict["uids"][str(uid)]
    assert conflict_row["resolution_status"] == "hard_conflict"
    assert conflict_row["resolved_path"] is None
    assert any("MH_E_DIVERGENT_REVISIONS" in item
               for item in conflict_row["diagnostics"])


def test_external_byte_change_with_same_descriptor_is_diagnosed(tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    uid = uuid.uuid4()
    fbx = _write_fbx(root / "wall.mesh.fbx", b"revision one")
    extractor = _extractor({fbx.name: _passport(uid)})
    initial = index_v2.scan_source_root(
        root, fbx_passport_extractor=extractor)
    fbx.write_bytes(b"revision two is longer")

    changed = index_v2.scan_source_root(
        root, fbx_passport_extractor=extractor, previous_index=initial)
    row = changed["paths"][index_v2.canonical_payload_path(fbx)]
    assert "MH_W_PAYLOAD_EXTERNAL_MODIFIED" in row["diagnostics"]
    assert changed["uids"][str(uid)]["resolution_status"] == \
        "external_modified"
    assert row["payload_fingerprint"] != initial["paths"][row["path"]][
        "payload_fingerprint"]

    cache = tmp_path / "cache" / "index.json"
    cache.parent.mkdir()
    cache.write_text(json.dumps(changed), encoding="utf-8")
    with pytest.raises(index_v2.SourceIndexV2Error) as caught:
        index_v2.resolve_resource_index_first(
            root, str(uid), fbx_passport_extractor=extractor,
            index_path=cache, publish_index=False)
    assert caught.value.code == \
        "MH_E_EXTERNAL_MODIFICATION_CONFIRMATION_REQUIRED"
    confirmed = index_v2.resolve_resource_index_first(
        root, str(uid), fbx_passport_extractor=extractor,
        index_path=cache, publish_index=False,
        confirm_external_modified=True)
    assert confirmed.resolution_status == "external_modified"
    assert "MH_W_PAYLOAD_EXTERNAL_MODIFIED" in confirmed.diagnostics


def test_changed_geometry_hash_is_authored_revision_not_external_edit(tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    uid = uuid.uuid4()
    fbx = _write_fbx(root / "wall.mesh.fbx", b"geometry revision one")
    passports = {fbx.name: _passport(uid)}
    extractor = _extractor(passports)
    initial = index_v2.scan_source_root(
        root, fbx_passport_extractor=extractor)

    fbx.write_bytes(b"geometry revision two")
    passports[fbx.name] = _passport(
        uid, extra={"geometry_hash": "xxh3:fedcba9876543210"})
    changed = index_v2.scan_source_root(
        root, fbx_passport_extractor=extractor, previous_index=initial)

    path = index_v2.canonical_payload_path(fbx)
    assert "MH_W_PAYLOAD_EXTERNAL_MODIFIED" not in \
        changed["paths"][path]["diagnostics"]
    assert changed["uids"][str(uid)]["resolution_status"] == "resolved"


def test_exact_fingerprint_confirmation_catches_same_size_and_mtime_edit(
        tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    uid = uuid.uuid4()
    fbx = _write_fbx(root / "wall.mesh.fbx", b"revision-one")
    extractor = _extractor({fbx.name: _passport(uid)})
    initial = index_v2.scan_source_root(
        root, fbx_passport_extractor=extractor)
    original_stat = fbx.stat()

    fbx.write_bytes(b"revision-two")
    assert fbx.stat().st_size == original_stat.st_size
    os.utime(
        fbx, ns=(original_stat.st_atime_ns, original_stat.st_mtime_ns))
    changed = index_v2.scan_source_root(
        root, fbx_passport_extractor=extractor, previous_index=initial)

    path = index_v2.canonical_payload_path(fbx)
    assert changed["paths"][path]["payload_fingerprint"] != \
        initial["paths"][path]["payload_fingerprint"]
    assert changed["paths"][path]["fingerprint_fast_path"] is False
    assert changed["uids"][str(uid)]["resolution_status"] == \
        "external_modified"


def test_index_first_verifies_hit_then_full_scan_falls_back_to_move(
        tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    cache = tmp_path / "cache" / "index.json"
    locks = tmp_path / "cache" / "locks"
    uid = uuid.uuid4()
    passport = _passport(uid)
    current = _write_fbx(root / "old" / "wall.mesh.fbx", b"payload")
    calls = []

    def extractor(path):
        calls.append(index_v2.canonical_payload_path(path))
        return (passport,)

    first = index_v2.resolve_resource_index_first(
        root, str(uid), fbx_passport_extractor=extractor,
        index_path=cache, lock_root=locks)
    assert first.resolution_source == "scan"
    assert cache.is_file()
    second = index_v2.resolve_resource_index_first(
        root, str(uid), fbx_passport_extractor=extractor,
        index_path=cache, lock_root=locks)
    assert second.resolution_source == "index"
    assert second.payload_path == index_v2.canonical_payload_path(current)

    moved = root / "new" / "wall.mesh.fbx"
    moved.parent.mkdir()
    shutil.move(current, moved)
    third = index_v2.resolve_resource_index_first(
        root, str(uid), fbx_passport_extractor=extractor,
        index_path=cache, lock_root=locks)
    assert third.resolution_source == "scan"
    assert third.resolution_status == "moved"
    assert third.payload_path == index_v2.canonical_payload_path(moved)
    assert len(calls) >= 3


def test_index_hit_reverifies_every_known_duplicate_candidate(tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    cache = tmp_path / "cache" / "index.json"
    uid = uuid.uuid4()
    passport = _passport(uid)
    a = _write_fbx(root / "a" / "wall_a.mesh.fbx", b"same")
    b = _write_fbx(root / "b" / "wall_b.mesh.fbx", b"same")
    extractor = _extractor({a.name: passport, b.name: passport})
    index_v2.rebuild_and_publish_index(
        root, fbx_passport_extractor=extractor, index_path=cache,
        lock_root=tmp_path / "cache" / "locks")

    b.write_bytes(b"a divergent revision")
    with pytest.raises(index_v2.SourceIndexV2Error) as caught:
        index_v2.resolve_resource_index_first(
            root, str(uid), fbx_passport_extractor=extractor,
            index_path=cache, publish_index=False)

    assert caught.value.code == "MH_E_DIVERGENT_REVISIONS"


def test_index_hit_detects_new_divergent_same_uid_candidate(tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    cache = tmp_path / "cache" / "index.json"
    uid = uuid.uuid4()
    original = _material(uid)
    _write_json(root / "a" / "wall.material", original)

    first = index_v2.resolve_resource_index_first(
        root, str(uid), index_path=cache,
        lock_root=tmp_path / "cache" / "locks")
    assert first.resolution_source == "scan"

    divergent = _material(uid)
    divergent["params"] = {"roughness": 0.25}
    _write_json(root / "b" / "wall_copy.material", divergent)

    with pytest.raises(index_v2.SourceIndexV2Error) as caught:
        index_v2.resolve_resource_index_first(
            root, str(uid), index_path=cache, publish_index=False)
    assert caught.value.code == "MH_E_DIVERGENT_REVISIONS"


def test_index_hit_rechecks_inventory_after_candidate_verification(
        tmp_path, monkeypatch):
    root = tmp_path / "source"
    root.mkdir()
    cache = tmp_path / "cache" / "index.json"
    uid = uuid.uuid4()
    original = _material(uid)
    _write_json(root / "a" / "wall.material", original)
    index_v2.resolve_resource_index_first(
        root, str(uid), index_path=cache,
        lock_root=tmp_path / "cache" / "locks")

    original_verify = index_v2._verify_cached_resolution
    raced = False

    def inject_copy(*args, **kwargs):
        nonlocal raced
        result = original_verify(*args, **kwargs)
        if not raced:
            raced = True
            divergent = _material(uid)
            divergent["params"] = {"roughness": 0.75}
            _write_json(root / "b" / "wall_copy.material", divergent)
        return result

    monkeypatch.setattr(
        index_v2, "_verify_cached_resolution", inject_copy)
    with pytest.raises(index_v2.SourceIndexV2Error) as caught:
        index_v2.resolve_resource_index_first(
            root, str(uid), index_path=cache, publish_index=False)
    assert caught.value.code == "MH_E_DIVERGENT_REVISIONS"


def test_rebuild_publish_and_per_resource_stability_guard(tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    cache = tmp_path / "cache" / "index.json"
    locks = tmp_path / "cache" / "locks"
    uid = uuid.uuid4()
    fbx = _write_fbx(root / "wall.mesh.fbx", b"stable payload")
    extractor = _extractor({fbx.name: _passport(uid)})

    rebuilt = index_v2.rebuild_and_publish_index(
        root, fbx_passport_extractor=extractor,
        index_path=cache, lock_root=locks)
    assert json.loads(cache.read_text(encoding="utf-8")) == rebuilt
    resolved = index_v2.resolve_resource_index_first(
        root, str(uid), fbx_passport_extractor=extractor,
        index_path=cache, publish_index=False)
    token = index_v2.capture_resource_stability_token(resolved)
    index_v2.assert_resource_stable(
        token, fbx_passport_extractor=extractor)

    fbx.write_bytes(b"changed after importer preflight")
    with pytest.raises(index_v2.SourceIndexV2Error) as caught:
        index_v2.assert_resource_stable(
            token, fbx_passport_extractor=extractor)
    assert caught.value.code == "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED"


def test_invalid_cache_falls_back_to_full_scan(tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    uid = uuid.uuid4()
    fbx = _write_fbx(root / "wall.mesh.fbx")
    cache = tmp_path / "cache" / "index.json"
    cache.parent.mkdir()
    cache.write_text("not json", encoding="utf-8")

    resolved = index_v2.resolve_resource_index_first(
        root, str(uid), fbx_passport_extractor=_extractor({
            fbx.name: _passport(uid)}), index_path=cache,
        lock_root=tmp_path / "cache" / "locks")

    assert resolved.resolution_source == "scan"
    assert json.loads(cache.read_text(encoding="utf-8"))["schema"] == \
        index_v2.INDEX_SCHEMA


def test_previously_proven_uid_becoming_malformed_is_hard(tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    cache = tmp_path / "cache" / "index.json"
    uid = uuid.uuid4()
    fbx = _write_fbx(root / "wall.mesh.fbx", b"valid revision")
    passport = _passport(uid)
    valid_extractor = _extractor({fbx.name: passport})
    index_v2.rebuild_and_publish_index(
        root, fbx_passport_extractor=valid_extractor,
        index_path=cache, lock_root=tmp_path / "locks")
    fbx.write_bytes(b"malformed replacement")

    with pytest.raises(index_v2.SourceIndexV2Error) as caught:
        index_v2.resolve_resource_index_first(
            root, str(uid),
            fbx_passport_extractor=_extractor({fbx.name: "{"}),
            index_path=cache, publish_index=False)

    assert caught.value.code == "MH_E_PASSPORT_INVALID"


def test_custom_index_path_inside_source_root_is_rejected(tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    uid = uuid.uuid4()
    _write_json(root / "wall.material", _material(uid))

    with pytest.raises(index_v2.SourceIndexV2Error) as caught:
        index_v2.resolve_resource_index_first(
            root, str(uid), index_path=root / ".cache" / "index.json")
    assert caught.value.code == "MH_E_SOURCE_INDEX_INVALID"
    assert not (root / ".cache").exists()


def test_carrier_extraction_file_race_is_fail_closed(tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    uid = uuid.uuid4()
    fbx = _write_fbx(root / "wall.mesh.fbx", b"before")

    def racing_extractor(path):
        path.write_bytes(b"changed while extracting carrier")
        return (_passport(uid),)

    with pytest.raises(index_v2.SourceIndexV2Error) as caught:
        index_v2.scan_source_root(
            root, fbx_passport_extractor=racing_extractor)
    assert caught.value.code == "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED"


def test_fork_is_deep_pure_and_rewrites_only_selected_composite_links():
    original_uid, fork_uid = str(uuid.uuid4()), str(uuid.uuid4())
    old_dependency, replacement, untouched = (
        str(uuid.uuid4()), str(uuid.uuid4()), str(uuid.uuid4()))
    original = _composite(original_uid)
    original["nodes"] = [
        {"resource_uid": old_dependency},
        {"resource_uid": untouched},
    ]

    forked = index_v2.fork_document(
        original, new_resource_uid=fork_uid,
        dependency_uid_replacements={old_dependency: replacement})

    assert original["uid"] == original_uid
    assert original["nodes"][0]["resource_uid"] == old_dependency
    assert forked["uid"] == fork_uid
    assert forked["nodes"][0]["resource_uid"] == replacement
    assert forked["nodes"][1]["resource_uid"] == untouched


def test_composite_v1_is_migration_only_and_malformed_json_is_quarantined(
        tmp_path):
    root = tmp_path / "source"
    root.mkdir()
    legacy_uid = uuid.uuid4()
    legacy = _write_json(
        root / "legacy.composite", _composite(legacy_uid, version=1))
    malformed = root / "broken.material"
    malformed.write_text("{", encoding="utf-8")

    index = index_v2.scan_source_root(root)

    legacy_row = index["paths"][index_v2.canonical_payload_path(legacy)]
    malformed_row = index["paths"][index_v2.canonical_payload_path(malformed)]
    assert legacy_row["parse_status"] == "legacy_composite_v1"
    assert legacy_row["parsed_passport"] is None
    assert str(legacy_uid) not in index["uids"]
    assert "MH_W_LEGACY_COMPOSITE_V1_MIGRATION_REQUIRED" in \
        legacy_row["diagnostics"]
    assert malformed_row["quarantined"] is True
    assert malformed_row["path"] in index["quarantine_paths"]
