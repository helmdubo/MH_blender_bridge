"""Tests for mh4blend.core.diff — one scenario per §7.2 flag the reference
differ may emit, plus the structural rules (flag order, silent nodes on
composite CREATE/REMOVE, empty diff)."""

import copy
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))
sys.path.insert(0, str(REPO_ROOT / "tools"))

from golden_uids import UIDS  # noqa: E402
from diff_bundles import load_bundle  # noqa: E402
from mh4blend.core.diff import diff_bundles  # noqa: E402
from mh4blend.core.model import (  # noqa: E402
    IDENTITY_TRANSFORM,
    Composite,
    Manifest,
    MaterialResource,
    MeshResource,
    Node,
    QuantizedTransform,
    composite_disk_dict,
    composite_hash,
    manifest_disk_dict,
)

WS = UIDS["col/ca_windowset"]
W1 = UIDS["node/ca_windowset/window_1"]
W2 = UIDS["node/ca_windowset/window_2"]


def build(nodes, meshes=()):
    composite = Composite(WS, "ca_windowset", nodes)
    manifest = Manifest(
        bundle_uid=UIDS["col/ca_building"], bundle_name="Golden",
        blend_file="golden.blend", exporter_version="0.2.0",
        meshes=list(meshes), composites=[composite])
    doc = manifest_disk_dict(manifest, {WS: composite_hash(composite)})
    return doc, {WS: composite_disk_dict(composite)}


def node(uid, **overrides):
    base = dict(node_uid=uid, parent_uid=None, kind="mesh", display_name="n",
                resource_uid=UIDS["col/window_a"],
                local_transform=IDENTITY_TRANSFORM, properties={})
    base.update(overrides)
    return Node(**base)


def test_reference_loader_rejects_pending_manifest(tmp_path):
    (tmp_path / "export_manifest.json").write_text(
        '{"schema":"mh.bundle_manifest","resources":[]}',
        encoding="utf-8")
    (tmp_path / "export_manifest.json.tmp").write_text(
        '{"schema":"mh.bundle_manifest","resources":[]}',
        encoding="utf-8")

    with pytest.raises(SystemExit, match="export in progress"):
        load_bundle(str(tmp_path))


def run(old, new):
    return diff_bundles(old[0], new[0], old[1], new[1])


def test_identical_bundles_produce_empty_diff():
    old = build([node(W1)])
    new = build([node(W1)])
    report = run(old, new)
    assert report["resources"] == {} and report["nodes"] == {}


def test_node_flags_each_and_ordering():
    old = build([node(W1), node(W2)])
    moved = QuantizedTransform((1000, 0, 0), (0, 0, 0, 1000000), (1000000,) * 3)
    new = build([
        node(W1, display_name="renamed", local_transform=moved,
             parent_uid=W2, resource_uid=UIDS["col/window_a_unique"],
             kind="composite_ref", properties={"role": "decal"}),
        node(W2),
    ])
    report = run(old, new)
    assert report["nodes"][WS][W1] == [
        "RENAME", "UPDATE_TRANSFORM", "UPDATE_PROPERTIES", "REPARENT",
        "UPDATE_RESOURCE", "UPDATE_KIND"]
    assert report["resources"] == {}


def test_node_create_and_remove():
    old = build([node(W1)])
    new = build([node(W2)])
    report = run(old, new)
    assert report["nodes"][WS] == {W1: ["REMOVE"], W2: ["CREATE"]}


def test_composite_create_does_not_enumerate_nodes():
    empty_manifest = Manifest(
        bundle_uid=UIDS["col/ca_building"], bundle_name="Golden",
        blend_file="golden.blend", exporter_version="0.2.0")
    old = (manifest_disk_dict(empty_manifest, {}), {})
    new = build([node(W1), node(W2)])
    report = run(old, new)
    assert report["resources"] == {WS: ["CREATE"]}
    assert report["nodes"] == {}


def test_mesh_geometry_rename_and_move():
    mesh_old = MeshResource(UIDS["col/wall_a"], "wall_a", "xxh3:aaaaaaaaaaaaaaaa")
    mesh_new = MeshResource(UIDS["col/wall_a"], "wall_a_main", "xxh3:bbbbbbbbbbbbbbbb")
    old = build([node(W1)], meshes=[mesh_old])
    new = build([node(W1)], meshes=[mesh_new])
    report = diff_bundles(old[0], new[0], old[1], new[1],
                          old_rel_path="props/walls", new_rel_path="env/walls")
    assert report["resources"][UIDS["col/wall_a"]] == [
        "RENAME", "UPDATE_GEOMETRY", "MOVE"]
    # composite survived unchanged but also moved
    assert report["resources"][WS] == ["MOVE"]


def test_decimal_spelling_does_not_fabricate_transform_diffs():
    old = build([node(W1)])
    new = build([node(W1)])
    new_doc = copy.deepcopy(new[1][WS])
    transform = new_doc["nodes"][0]["local_transform"]
    transform["translation_cm"] = [0, 0, 0]        # ints vs 0.0 floats
    transform["scale"] = [1, 1.0, 1.00]
    report = diff_bundles(old[0], new[0], old[1], {WS: new_doc})
    assert report["nodes"] == {}


def test_sub_p6_material_edit_does_not_fabricate_structural_diff():
    def with_material(value):
        manifest, composites = build([node(W1)])
        material = MaterialResource(
            UIDS["mesh/wall_b"], "m_stone", "rendinst_simple",
            params={"roughness": value})
        semantic = Manifest(
            UIDS["col/ca_building"], "Golden", "golden.blend", "0.2.0",
            composites=[Composite(WS, "ca_windowset", [node(W1)])],
            materials=[material])
        return manifest_disk_dict(
            semantic, {WS: composite_hash(semantic.composites[0])}), composites

    report = run(with_material(0.25), with_material(0.2500004))
    assert report["resources"] == {}
    assert report["nodes"] == {}
