"""Blender field tests for stateless clean-name composite export."""

from __future__ import annotations

import json
from pathlib import Path
import sys

import pytest

bpy = pytest.importorskip("bpy")

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.scene import export_composite as module  # noqa: E402


UID = "11111111-1111-4111-8111-111111111111"
OTHER_UID = "22222222-2222-4222-8222-222222222222"


def _collection(name="Garage Block A", uid=UID):
    collection = bpy.data.collections.new(name)
    collection["mh_uid"] = uid
    bpy.context.scene.collection.children.link(collection)
    return collection


def test_explicit_export_always_writes_clean_self_contained_payload(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = _collection()
    calls = []
    original = module.atomic_publish_json

    def tracked(path, document, **kwargs):
        calls.append(str(path))
        return original(path, document, **kwargs)

    monkeypatch.setattr(module, "atomic_publish_json", tracked)
    first = module.export_composite_collection(collection, tmp_path)
    second = module.export_composite_collection(collection, tmp_path)

    path = tmp_path / "garage_block_a.composite"
    assert first["path"] == str(path)
    assert second["path"] == str(path)
    assert first["written"] is True and second["written"] is True
    assert calls == [str(path), str(path)]
    document = json.loads(path.read_text(encoding="utf-8"))
    assert document["schema"] == "mh.composite"
    assert document["schema_version"] == 2
    assert document["uid"] == UID
    assert document["properties"] == {}
    assert sorted(item.name for item in tmp_path.iterdir()) == [path.name]


def test_clean_name_collision_with_other_embedded_uid_blocks(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    existing = _collection(uid=OTHER_UID)
    module.export_composite_collection(existing, tmp_path)
    bpy.data.collections.remove(existing)
    ours = _collection(uid=UID)

    with pytest.raises(
            ValueError, match="MH_E_NAME_COLLISION_DIFFERENT_UID"):
        module.export_composite_collection(ours, tmp_path)

    document = json.loads(
        (tmp_path / "garage_block_a.composite").read_text(encoding="utf-8"))
    assert document["uid"] == OTHER_UID


def test_collision_appearing_after_preflight_is_rejected_under_lock(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    ours = _collection(uid=UID)
    original = module.atomic_publish_json

    def raced(path, document, **kwargs):
        foreign = dict(document, uid=OTHER_UID)
        Path(path).write_text(
            json.dumps(foreign), encoding="utf-8")
        return original(path, document, **kwargs)

    monkeypatch.setattr(module, "atomic_publish_json", raced)
    with pytest.raises(
            ValueError, match="MH_E_NAME_COLLISION_DIFFERENT_UID"):
        module.export_composite_collection(ours, tmp_path)
    assert json.loads(
        (tmp_path / "garage_block_a.composite").read_text(
            encoding="utf-8"))["uid"] == OTHER_UID


def test_unresolved_target_does_not_trigger_reader_scan_or_block_export(
        tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    composite = _collection()
    target = bpy.data.collections.new("Missing Mesh Source")
    target["mh_uid"] = "33333333-3333-4333-8333-333333333333"
    mesh = bpy.data.meshes.new("Mesh")
    mesh.from_pydata(
        [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)],
        [], [(0, 1, 2)])
    target.objects.link(bpy.data.objects.new("Mesh", mesh))
    node = bpy.data.objects.new("Missing Placement", None)
    node.instance_type = "COLLECTION"
    node.instance_collection = target
    composite.objects.link(node)

    report = module.export_composite_collection(
        composite, tmp_path, source_root=tmp_path)

    assert report["ok"] is True
    document = json.loads(Path(report["path"]).read_text(encoding="utf-8"))
    assert document["nodes"][0]["resource_uid"] == target["mh_uid"]
    assert sorted(path.name for path in tmp_path.iterdir()) == [
        "garage_block_a.composite"]


def test_reachable_two_resource_authoring_cycle_blocks_export(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    first = _collection("First", UID)
    second = _collection("Second", OTHER_UID)
    first["mh_kind"] = "composite"
    second["mh_kind"] = "composite"

    first_to_second = bpy.data.objects.new("First to Second", None)
    first_to_second.instance_type = "COLLECTION"
    first_to_second.instance_collection = second
    first.objects.link(first_to_second)
    second_to_first = bpy.data.objects.new("Second to First", None)
    second_to_first.instance_type = "COLLECTION"
    second_to_first.instance_collection = first
    second.objects.link(second_to_first)

    try:
        with pytest.raises(ValueError, match="MH_E_COMPOSITE_CYCLE"):
            module.export_composite_collection(
                first, tmp_path, composite_collections=(first, second),
                source_root=tmp_path)
        assert list(tmp_path.iterdir()) == []
    finally:
        # Break the deliberately invalid Blender instance graph before the
        # next factory-reset/depsgraph evaluation.
        second_to_first.instance_collection = None
        second_to_first.instance_type = "NONE"
