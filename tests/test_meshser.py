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


def empty_record():
    return MeshObjectRecord(
        transform=IDENTITY,
        positions=[],
        polygons=[],
        split_normals=None,
        use_smooth=[],
    )


def test_meshser2_multi_lod_immutable_cross_language_vector():
    items = [
        (0, "00000000-0000-4000-8000-000000000001", empty_record()),
        (2, "00000000-0000-4000-8000-000000000002", empty_record()),
    ]
    stream = serialize_mesh_resource(items)

    assert stream[:20] == (
        b"\x0c\x00\x00\x00mh.meshser:2\x02\x00\x00\x00")
    assert int.from_bytes(stream[20:24], "little") == 0
    assert int.from_bytes(stream[177:181], "little") == 2
    assert len(stream) == 334
    assert mesh_content_hash(items) == "xxh3:74365800b27bfad2"


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


def test_object_order_is_by_lod_then_uid_not_input_order():
    a, b = quad(), quad()
    b.positions[0] = (0.5, 0.0, 0.0)
    forward = serialize_mesh_resource([(0, "aaa", a), (1, "bbb", b)])
    backward = serialize_mesh_resource([(1, "bbb", b), (0, "aaa", a)])
    assert forward == backward
    same_level = serialize_mesh_resource([(0, "aaa", a), (0, "bbb", b)])
    swapped_uids = serialize_mesh_resource([(0, "bbb", a), (0, "aaa", b)])
    assert swapped_uids != same_level


def test_lod_level_is_encoded_and_two_tuple_means_lod0():
    record = quad()
    assert (serialize_mesh_resource([("uid", record)])
            == serialize_mesh_resource([(0, "uid", record)]))
    assert (serialize_mesh_resource([(1, "uid", record)])
            != serialize_mesh_resource([(0, "uid", record)]))


def test_lod_level_requires_uint32():
    import pytest

    for invalid in (-1, 2 ** 32, 1.0, True):
        with pytest.raises(ValueError, match="uint32"):
            serialize_mesh_resource([(invalid, "uid", quad())])


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
