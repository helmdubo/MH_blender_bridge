"""Tests for mh4blend.core.meshser (§9 byte layout invariants)."""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.meshser import (  # noqa: E402
    MeshObjectRecord,
    mesh_content_hash,
    serialize_mesh_resource,
)

IDENTITY = (1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0)


def quad(material_index=0):
    return MeshObjectRecord(
        transform=IDENTITY,
        positions=[(0.0, 0.0, 0.0), (1.0, 0.0, 0.0),
                   (1.0, 1.0, 0.0), (0.0, 1.0, 0.0)],
        polygons=[(material_index, (0, 1, 2, 3))],
        split_normals=None,
        use_smooth=[False],
        sharp_edges=[],
        uv_layers={"UVMap": [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]},
        material_slot_names=["stone"],
    )


def test_deterministic_and_sensitive_to_geometry():
    a = mesh_content_hash([("uid-a", quad())])
    assert a == mesh_content_hash([("uid-a", quad())])
    moved = quad()
    moved.positions[2] = (1.0, 1.0, 0.25)
    assert mesh_content_hash([("uid-a", moved)]) != a


def test_material_index_changes_hash():
    """The stage-B blocker check: repainting a face to another slot must
    change the hash even though vertices and slot names are unchanged."""
    base = quad(material_index=0)
    base.material_slot_names = ["stone", "wood"]
    repainted = quad(material_index=1)
    repainted.material_slot_names = ["stone", "wood"]
    assert (mesh_content_hash([("u", base)])
            != mesh_content_hash([("u", repainted)]))


def test_slot_rename_and_uv_layer_rename_change_hash():
    base = mesh_content_hash([("u", quad())])
    renamed_slot = quad()
    renamed_slot.material_slot_names = ["marble"]
    assert mesh_content_hash([("u", renamed_slot)]) != base
    renamed_uv = quad()
    renamed_uv.uv_layers = {"Lightmap": renamed_uv.uv_layers.pop("UVMap")}
    assert mesh_content_hash([("u", renamed_uv)]) != base


def test_object_order_is_by_uid_not_input_order():
    a, b = quad(), quad()
    b.positions[0] = (0.5, 0.0, 0.0)
    forward = serialize_mesh_resource([("aaa", a), ("bbb", b)])
    backward = serialize_mesh_resource([("bbb", b), ("aaa", a)])
    assert forward == backward
    swapped_uids = serialize_mesh_resource([("bbb", a), ("aaa", b)])
    assert swapped_uids != forward


def test_transform_participates_in_hash():
    shifted = quad()
    shifted.transform = IDENTITY[:3] + (2.5,) + IDENTITY[4:]
    assert (mesh_content_hash([("u", shifted)])
            != mesh_content_hash([("u", quad())]))


def test_sharp_edge_pairs_are_order_normalized():
    x, y = quad(), quad()
    x.sharp_edges = [(2, 1), (0, 3)]
    y.sharp_edges = [(3, 0), (1, 2)]
    assert (serialize_mesh_resource([("u", x)])
            == serialize_mesh_resource([("u", y)]))


def test_record_has_no_name_fields():
    """Anti-requirement §9.4 enforced structurally: nothing to hash."""
    fields = set(MeshObjectRecord.__dataclass_fields__)
    assert "name" not in fields and "object_name" not in fields
