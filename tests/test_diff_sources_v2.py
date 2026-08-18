"""Pure loader/CLI tests for Clean Sources v2 diff snapshots."""

from copy import deepcopy
import json
from pathlib import Path
from types import SimpleNamespace
import sys

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))
sys.path.insert(0, str(REPO_ROOT / "tools"))

import diff_bundles  # noqa: E402
from mh4blend.core.diff import diff_source_snapshots  # noqa: E402
from mh4blend.core.fbx_passport import (  # noqa: E402
    canonical_passport,
    descriptor_hash,
    make_fbx_passport,
)


MESH_UID = "10000000-0000-0000-0000-000000000001"
COMP_UID = "20000000-0000-0000-0000-000000000001"
MAT_UID = "30000000-0000-0000-0000-000000000001"


def write_json(path, document):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(document, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8")


def material(uid=MAT_UID, name="m_wall", **overrides):
    document = {
        "schema": "mh.material",
        "schema_version": 1,
        "uid": uid,
        "name": name,
        "shader_class": "rendinst_simple",
        "params": {},
        "textures": {},
    }
    document.update(overrides)
    return document


def composite(version=2):
    document = {
        "schema": "mh.composite",
        "schema_version": version,
        "uid": COMP_UID,
        "name": "assembly",
        "nodes": [],
    }
    if version == 2:
        document["properties"] = {}
    return document


def passport(*, geometry="xxh3:aaaaaaaaaaaaaaaa", name="wall"):
    return make_fbx_passport(
        resource_uid=MESH_UID,
        name=name,
        lod_levels=[0],
        lod_policy="generated",
        geometry_hash=geometry,
        material_slots=[],
        properties={},
        exporter="mh4blend test",
    )


def receipt(document):
    return SimpleNamespace(
        document=deepcopy(document),
        canonical_text=canonical_passport(document),
        descriptor_hash=descriptor_hash(document),
        copy_count=1,
        model_names=("wall",),
    )


def inventory(root):
    return sorted(
        path.relative_to(root).as_posix()
        for path in root.rglob("*") if path.is_file())


def test_snapshot_reads_only_v2_primary_payloads_without_writes(
        tmp_path, monkeypatch):
    (tmp_path / "meshes").mkdir()
    (tmp_path / "meshes" / "wall.mesh.fbx").write_bytes(b"fake-fbx")
    (tmp_path / "meshes" / "ignored.fbx").write_bytes(b"not-primary")
    write_json(tmp_path / "materials" / "m_wall.material", material())
    write_json(tmp_path / "composites" / "assembly.composite", composite())
    write_json(tmp_path / "export_manifest.json", {"not": "read"})
    write_json(tmp_path / "index.json", {"not": "written"})
    before = inventory(tmp_path)
    calls = []

    def fake_production_reader(path):
        calls.append(Path(path).name)
        return receipt(passport())

    monkeypatch.setattr(
        diff_bundles, "read_fbx_passport", fake_production_reader)
    snapshot = diff_bundles.load_source_snapshot(
        tmp_path, root_rel="library/a")

    assert calls == ["wall.mesh.fbx"]
    assert set(snapshot["resources"]) == {MESH_UID, MAT_UID, COMP_UID}
    assert snapshot["resources"][MESH_UID]["source_path"] == \
        "library/a/meshes/wall.mesh.fbx"
    assert snapshot["resources"][MAT_UID]["source_path"] == \
        "library/a/materials/m_wall.material"
    assert inventory(tmp_path) == before


def test_manifest_only_directory_is_empty_not_legacy_read(tmp_path):
    write_json(tmp_path / "export_manifest.json", {
        "schema": "mh.export_manifest",
        "schema_version": 1,
        "resources": [{
            "uid": MESH_UID,
            "kind": "static_mesh",
            "source": "missing.mesh.fbx",
        }],
    })
    snapshot = diff_bundles.load_source_snapshot(
        tmp_path, fbx_reader=lambda _path: pytest.fail("FBX reader called"))
    assert snapshot["resources"] == {}


def test_composite_v1_is_not_a_production_candidate(tmp_path):
    write_json(tmp_path / "legacy.composite", composite(version=1))
    with pytest.raises(
            diff_bundles.SourceSnapshotError,
            match="MH_W_LEGACY_COMPOSITE_V1_MIGRATION_REQUIRED") as caught:
        diff_bundles.load_source_snapshot(tmp_path)
    assert caught.value.code == "MH_E_UNKNOWN_SCHEMA_VERSION"


def test_duplicate_uid_snapshot_is_refused_without_implicit_winner(tmp_path):
    write_json(tmp_path / "a" / "m_wall.material", material(name="m_wall"))
    write_json(tmp_path / "b" / "m_other.material", material(name="m_other"))
    with pytest.raises(
            diff_bundles.SourceSnapshotError,
            match="divergent candidates") as caught:
        diff_bundles.load_source_snapshot(tmp_path)
    assert caught.value.code == "MH_E_DIVERGENT_REVISIONS"


def test_identical_duplicate_uid_is_deterministic_and_diagnostic(tmp_path):
    document = material()
    write_json(tmp_path / "b" / "m_wall.material", document)
    write_json(tmp_path / "a" / "m_wall.material", document)

    snapshot = diff_bundles.load_source_snapshot(tmp_path)

    assert snapshot["resources"][MAT_UID]["source_path"] == \
        "a/m_wall.material"
    assert snapshot["diagnostics"] == [
        "MH_W_DUPLICATE_IDENTICAL_PAYLOAD: "
        "a/m_wall.material, b/m_wall.material"]


def test_loader_rejects_invalid_material_and_unknown_composite_fields(tmp_path):
    invalid = material()
    invalid["textures"] = {"tex16": "bad.tif"}
    write_json(tmp_path / "bad.material", invalid)
    with pytest.raises(diff_bundles.SourceSnapshotError) as caught:
        diff_bundles.load_source_snapshot(tmp_path)
    assert caught.value.code == "MH_E_INVALID_MATERIAL_VALUE"

    (tmp_path / "bad.material").unlink()
    invalid_composite = composite()
    invalid_composite["legacy"] = True
    write_json(tmp_path / "bad.composite", invalid_composite)
    with pytest.raises(diff_bundles.SourceSnapshotError) as caught:
        diff_bundles.load_source_snapshot(tmp_path)
    assert caught.value.code == "MH_E_INVALID_COMPOSITE"


def test_loaded_mesh_geometry_and_path_changes_are_reader_side_diff(tmp_path):
    old_root = tmp_path / "old"
    new_root = tmp_path / "new"
    old_root.mkdir()
    new_root.mkdir()
    (old_root / "wall.mesh.fbx").write_bytes(b"old")
    (new_root / "moved").mkdir()
    (new_root / "moved" / "wall.mesh.fbx").write_bytes(b"new")

    def fake_reader(path):
        geometry = "xxh3:aaaaaaaaaaaaaaaa" \
            if Path(path).read_bytes() == b"old" \
            else "xxh3:bbbbbbbbbbbbbbbb"
        return receipt(passport(geometry=geometry))

    old = diff_bundles.load_source_snapshot(
        old_root, root_rel="project", fbx_reader=fake_reader)
    new = diff_bundles.load_source_snapshot(
        new_root, root_rel="project", fbx_reader=fake_reader)
    report = diff_source_snapshots(old, new)
    assert report["resources"][MESH_UID] == ["UPDATE_GEOMETRY", "MOVE"]


def test_cli_prints_diff_for_json_payload_snapshots(tmp_path, capsys):
    old_root = tmp_path / "old"
    new_root = tmp_path / "new"
    old_root.mkdir()
    new_root.mkdir()
    write_json(old_root / "m_wall.material", material(params={"value": 1}))
    write_json(new_root / "m_wall.material", material(params={"value": 2}))

    diff_bundles.main([str(old_root), str(new_root)])
    report = json.loads(capsys.readouterr().out)
    assert report["resources"] == {MAT_UID: ["UPDATE_PROPERTIES"]}
