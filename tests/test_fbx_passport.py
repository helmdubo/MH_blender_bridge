"""Pure contract tests for the production FBX passport codec/reader."""

from copy import deepcopy
from dataclasses import dataclass, field
import json
from pathlib import Path
import sys

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.fbx_passport import (  # noqa: E402
    PASSPORT_PROPERTY,
    canonical_passport,
    descriptor_hash,
    make_fbx_passport,
    parse_passport_text,
    read_fbx_passport,
)


RESOURCE_UID = "11111111-1111-4111-8111-111111111111"
MATERIAL_UID = "22222222-2222-4222-8222-222222222222"


def _document(**overrides):
    values = {
        "resource_uid": RESOURCE_UID,
        "name": "Wall A",
        "lod_levels": [0, 1],
        "lod_policy": "authored",
        "geometry_hash": "xxh3:0123456789abcdef",
        "material_slots": [{
            "slot_name": "surface",
            "material_uid": MATERIAL_UID,
            "material_name_hint": "M Stucco",
        }],
        "properties": {"role": "wall"},
        "exporter": "mh4blend 0.5.0",
    }
    values.update(overrides)
    return make_fbx_passport(**values)


def test_canonical_passport_exact_vector_and_roundtrip():
    document = _document()
    expected = (
        '{"exporter":"mh4blend 0.5.0","geometry_hash":'
        '"xxh3:0123456789abcdef","kind":"static_mesh","lod_levels":[0,1],'
        '"lod_policy":"authored","material_slots":[{"material_name_hint":'
        '"M Stucco","material_uid":"22222222-2222-4222-8222-222222222222",'
        '"slot_name":"surface"}],"name":"Wall A","properties":{"role":'
        '"wall"},"resource_uid":"11111111-1111-4111-8111-111111111111",'
        '"schema":"mh.fbx_passport","schema_version":1}'
    )
    assert canonical_passport(document) == expected
    assert parse_passport_text(expected) == document
    assert descriptor_hash(document) == (
        "sha256:814282c42e4b768d7622f97ee9d8faa95c17f21a99d4304a29a70fcf64576610"
    )


@pytest.mark.parametrize("field,value", [
    ("name", "Wall B"),
    ("lod_levels", [0]),
    ("lod_policy", "nanite"),
    ("properties", {"role": "roof"}),
    ("exporter", "mh4blend 0.5.1"),
])
def test_descriptor_hash_changes_for_every_metadata_field(field, value):
    base = _document()
    overrides = {field: value}
    if field == "lod_policy":
        overrides["lod_levels"] = [0]
    changed = _document(**overrides)
    assert descriptor_hash(changed) != descriptor_hash(base)


def test_descriptor_hash_changes_for_slot_uid_name_and_hint():
    base = _document()
    for slot in (
        {"slot_name": "detail", "material_uid": MATERIAL_UID,
         "material_name_hint": "M Stucco"},
        {"slot_name": "surface",
         "material_uid": "33333333-3333-4333-8333-333333333333",
         "material_name_hint": "M Stucco"},
        {"slot_name": "surface", "material_uid": MATERIAL_UID,
         "material_name_hint": "M Renamed"},
    ):
        assert descriptor_hash(_document(material_slots=[slot])) != \
            descriptor_hash(base)


def test_geometry_hash_is_not_part_of_descriptor_hash():
    assert descriptor_hash(_document()) == descriptor_hash(_document(
        geometry_hash="xxh3:fedcba9876543210"))


def test_material_slots_must_be_sorted_but_may_share_material_uid():
    slots = [
        {"slot_name": "a", "material_uid": MATERIAL_UID,
         "material_name_hint": ""},
        {"slot_name": "b", "material_uid": MATERIAL_UID,
         "material_name_hint": "M"},
    ]
    assert _document(material_slots=slots)["material_slots"] == slots
    with pytest.raises(ValueError, match="sorted by slot_name"):
        _document(material_slots=list(reversed(slots)))


@pytest.mark.parametrize("mutate", [
    lambda row: row.pop("exporter"),
    lambda row: row.__setitem__("unknown", 1),
    lambda row: row.__setitem__("schema_version", True),
    lambda row: row.__setitem__(
        "resource_uid", "AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA"),
    lambda row: row.__setitem__("kind", "mesh"),
    lambda row: row.__setitem__("lod_levels", [0, 2]),
    lambda row: row.__setitem__("lod_policy", "generated"),
    lambda row: row.__setitem__("geometry_hash", "xxh3:ABCDEF0123456789"),
    lambda row: row.__setitem__("material_slots", [{
        "slot_name": "surface", "material_uid": MATERIAL_UID,
    }]),
    lambda row: row.__setitem__("properties", []),
    lambda row: row.__setitem__("properties", {1: "not a JSON key"}),
    lambda row: row.__setitem__("properties", {"tuple": (1, 2)}),
    lambda row: row.__setitem__("properties", {"bad": float("nan")}),
    lambda row: row.__setitem__("exporter", ""),
])
def test_invalid_documents_fail_closed_with_diagnostic(mutate):
    row = _document()
    mutate(row)
    with pytest.raises(ValueError, match="^MH_E_PASSPORT_INVALID:"):
        canonical_passport(row)


def test_reader_rejects_noncanonical_json_text():
    pretty = json.dumps(_document(), ensure_ascii=False, indent=2)
    with pytest.raises(ValueError, match="not canonical"):
        parse_passport_text(pretty)


@dataclass
class _Element:
    id: bytes
    props: list = field(default_factory=list)
    props_type: list = field(default_factory=list)
    elems: list = field(default_factory=list)


def _property(text, *, malformed=False):
    return _Element(
        b"P",
        [PASSPORT_PROPERTY.encode(), b"KString", b"", b"U", text.encode()],
        [b"S", b"S", b"S", b"S", b"I" if malformed else b"S"],
    )


def _model(name, texts):
    return _Element(
        b"Model", [1, name.encode() + b"\x00\x01Model", b"Mesh"],
        elems=[_Element(b"Properties70", elems=[
            _property(text) for text in texts
        ])],
    )


def _parser(models):
    root = _Element(b"", elems=[
        _Element(b"Objects", elems=models),
    ])
    return lambda _path: (root, b"S")


def test_blender_tree_reader_requires_byte_consensus_on_every_mesh_model():
    text = canonical_passport(_document())
    result = read_fbx_passport(
        "unused.fbx", parser=_parser([
            _model("PartA", [text]), _model("PartB", [text]),
        ]))
    assert result.document == _document()
    assert result.copy_count == 2
    assert result.model_names == ("PartA", "PartB")


@pytest.mark.parametrize("models,match", [
    ([], "no MESH Models"),
    (lambda text: [_model("A", [])], "exactly one"),
    (lambda text: [_model("A", [text, text])], "exactly one"),
    (lambda text: [_model("A", [text]), _model(
        "B", [canonical_passport(_document(name="Wall B"))])],
     "copies differ"),
])
def test_blender_tree_reader_rejects_missing_duplicate_or_divergent_carriers(
        models, match):
    text = canonical_passport(_document())
    model_rows = models(text) if callable(models) else models
    with pytest.raises(ValueError, match=match):
        read_fbx_passport("unused.fbx", parser=_parser(model_rows))


def test_blender_tree_reader_rejects_malformed_property_type():
    text = canonical_passport(_document())
    model = _model("A", [])
    model.elems[0].elems.append(_property(text, malformed=True))
    with pytest.raises(ValueError, match="malformed passport property"):
        read_fbx_passport("unused.fbx", parser=_parser([model]))
