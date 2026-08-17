"""Tests for mh4blend.core.model — on-disk writers (§1-§3, §8.1)."""

import json
import sys
import unicodedata
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))
sys.path.insert(0, str(REPO_ROOT / "tools"))

from golden_uids import UIDS  # noqa: E402
from mh4blend.core.canonical import composite_content_hash  # noqa: E402
from mh4blend.core.model import (  # noqa: E402
    IDENTITY_TRANSFORM,
    Composite,
    Manifest,
    MaterialResource,
    MaterialSlot,
    MeshResource,
    Node,
    composite_disk_dict,
    composite_hash,
    manifest_disk_dict,
)


def cycle_composite(name, uid_key, node_key, ref_display, ref_uid_key):
    return Composite(
        uid=UIDS[uid_key],
        name=name,
        nodes=[
            Node(
                node_uid=UIDS[node_key],
                parent_uid=None,
                kind="composite_ref",
                display_name=ref_display,
                resource_uid=UIDS[ref_uid_key],
                local_transform=IDENTITY_TRANSFORM,
            ),
        ],
    )


def render(doc: dict) -> str:
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def test_writer_reproduces_cycle_fixture_bytes():
    """The strongest writer test in the repo: the hand-authored stage-A
    fixture files must be byte-identical to what the model writer emits."""
    fixture_dir = REPO_ROOT / "golden" / "fixtures" / "composite_cycle"
    composites = {
        "cycle_a": cycle_composite(
            "cycle_a", "col/cycle_a", "node/cycle_a/ref_b", "ref_b", "col/cycle_b"),
        "cycle_b": cycle_composite(
            "cycle_b", "col/cycle_b", "node/cycle_b/ref_a", "ref_a", "col/cycle_a"),
    }
    for name, composite in composites.items():
        path = fixture_dir / composite.filename()
        assert path.exists(), path
        assert render(composite_disk_dict(composite)) == path.read_text()


def test_composite_hash_idempotent_over_json_roundtrip():
    composite = cycle_composite(
        "cycle_a", "col/cycle_a", "node/cycle_a/ref_b", "ref_b", "col/cycle_b")
    doc = composite_disk_dict(composite)
    reparsed = json.loads(render(doc))
    assert composite_hash(composite) == composite_content_hash(reparsed)


def test_nodes_sorted_by_uid_regardless_of_input_order():
    nodes = [
        Node(UIDS["node/ca_windowset/window_3"], None, "mesh", "w3",
             UIDS["col/window_a"], IDENTITY_TRANSFORM),
        Node(UIDS["node/ca_windowset/window_1"], None, "mesh", "w1",
             UIDS["col/window_a"], IDENTITY_TRANSFORM),
    ]
    a = Composite(UIDS["col/ca_windowset"], "ws", list(nodes))
    b = Composite(UIDS["col/ca_windowset"], "ws", list(reversed(nodes)))
    assert composite_disk_dict(a) == composite_disk_dict(b)
    assert composite_hash(a) == composite_hash(b)


def test_manifest_layout_and_ordering():
    mesh = MeshResource(
        uid=UIDS["col/wall_a"], name="wall_a", content_hash="xxh3:0011223344556677",
        material_slots=[MaterialSlot("stone", UIDS["mesh/wall_a"])])
    composite = cycle_composite(
        "cycle_a", "col/cycle_a", "node/cycle_a/ref_b", "ref_b", "col/cycle_b")
    material = MaterialResource(
        uid=UIDS["mesh/wall_b"], name="m_stone", shader_class="rendinst_simple",
        params={"gamma": 1.0}, textures={"tex0": "common/stone_tex_d.tif"})
    manifest = Manifest(
        bundle_uid=UIDS["col/ca_building"], bundle_name="Golden",
        blend_file="golden.blend", exporter_version="0.1.0",
        meshes=[mesh], composites=[composite], materials=[material],
        external_dependencies=[
            {"uid": UIDS["col/cycle_b"], "kind": "composite", "name": "cycle_b"}])

    doc = manifest_disk_dict(manifest, {composite.uid: composite_hash(composite)})

    assert doc["schema"] == "mh.bundle_manifest"
    assert list(doc) == ["schema", "schema_version", "exporter_version",
                         "bundle_uid", "bundle_name", "source", "resources",
                         "materials", "external_dependencies"]
    uids = [r["uid"] for r in doc["resources"]]
    assert uids == sorted(uids), "resources must be uid-sorted"
    mesh_entry = next(r for r in doc["resources"] if r["kind"] == "static_mesh")
    assert mesh_entry["source"] == f"meshes/wall_a__{UIDS['col/wall_a'][:8]}.mesh.fbx"
    assert mesh_entry["material_slots"] == [
        {"slot_name": "stone", "material_uid": UIDS["mesh/wall_a"]}]
    mat_entry = doc["materials"][0]
    assert mat_entry["kind"] == "material"
    assert mat_entry["textures"]["tex0"] == "common/stone_tex_d.tif"


def test_writer_normalizes_display_names_to_nfc():
    nfd_name = unicodedata.normalize("NFD", "окно́")
    node = Node(UIDS["node/ca_windowset/window_1"], None, "mesh", nfd_name,
                UIDS["col/window_a"], IDENTITY_TRANSFORM,
                properties={"role": unicodedata.normalize("NFD", "декаль")})
    doc = Node.disk_dict(node)
    assert doc["display_name"] == unicodedata.normalize("NFC", nfd_name)
    assert doc["properties"]["role"] == unicodedata.normalize("NFC", "декаль")
