"""Pure Source Protocol v5 composite codec gates."""

import json
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.composites import (  # noqa: E402
    CompositeValueError,
    composite_document,
    composite_json_bytes,
    iter_profile_references,
    iter_resource_references,
    parse_composite,
    read_composite_file,
    validate_composite_cycles,
)
from mh4blend.core.model import (  # noqa: E402
    Composite,
    CompositeTransform,
    Node,
    RandomOption,
)
from tools.mh_v5_codec_fixture import golden_bytes  # noqa: E402


GOLDEN_PATH = REPO_ROOT / "golden" / "v5" / "source_protocol_v5_codec_vectors.json"


def _fixture():
    return json.loads(GOLDEN_PATH.read_text(encoding="utf-8"))


def test_shared_v5_codec_golden_is_byte_identical():
    assert GOLDEN_PATH.read_bytes() == golden_bytes()


def test_shared_composite_vectors_are_byte_exact_and_roundtrip():
    fixture = _fixture()
    assert fixture["schema"] == "mh.source_protocol_v5_codec_vectors:1"
    for vector in fixture["composite_vectors"]:
        expected = vector["canonical_utf8"].encode("utf-8")
        assert composite_json_bytes(parse_composite(expected)) == expected, vector["name"]
    for vector in fixture["composite_negative_vectors"]:
        with pytest.raises(CompositeValueError) as excinfo:
            parse_composite(vector["json"])
        assert excinfo.value.code == vector["error"], vector["name"]
        if "message_contains" in vector:
            assert vector["message_contains"] in str(excinfo.value)


def test_missing_version_is_legacy_generation_and_never_dual_reads():
    for legacy in (
        '{"nodes":[]}',
        '{"nodes":[{"kind":"group"}]}',
    ):
        with pytest.raises(CompositeValueError) as excinfo:
            parse_composite(legacy)
        assert excinfo.value.code == "MH_E_COMPOSITE_LEGACY_GENERATION"
        assert "файл прежнего поколения: удалите и переэкспортируйте" in str(excinfo.value)


def test_v5_field_order_and_identity_elision_are_canonical():
    resource = Composite("vehicle", [
        Node("mesh", resource="hood"),
        Node("random", profile="scatter", options=[
            RandomOption("empty", 0),
            RandomOption("composite", 2, "variant"),
        ]),
    ])
    document = composite_document(resource)
    assert list(document) == ["v", "nodes"]
    assert list(document["nodes"][0]) == ["kind", "resource"]
    assert list(document["nodes"][1]) == ["kind", "profile", "options"]
    assert list(document["nodes"][1]["options"][1]) == ["kind", "resource", "weight"]


def test_all_random_options_participate_in_dependency_and_cycle_traversal():
    root = Composite("root", [Node("random", options=[
        RandomOption("composite", 1, "selected"),
        RandomOption("composite", 0, "cycle"),
        RandomOption("mesh", 0, "unused_mesh"),
        RandomOption("empty", 0),
    ])])
    assert list(iter_resource_references(root)) == [
        "selected", "cycle", "unused_mesh"]
    graph = {
        "root": root,
        "selected": Composite("selected"),
        "cycle": Composite("cycle", [Node("composite", resource="root")]),
    }
    with pytest.raises(CompositeValueError) as excinfo:
        validate_composite_cycles("root", graph)
    assert excinfo.value.code == "MH_E_COMPOSITE_CYCLE"


def test_profile_and_resource_reference_order_is_dfs_and_significant():
    resource = Composite("root", [
        Node("group", profile="a", children=[
            Node("random", profile="b", options=[
                RandomOption("mesh", 1, "first"),
                RandomOption("mesh", 1, "second"),
            ]),
        ]),
    ])
    assert list(iter_profile_references(resource)) == ["a", "b"]
    assert list(iter_resource_references(resource, kind="mesh")) == ["first", "second"]


@pytest.mark.parametrize("value", [float("nan"), float("inf"), -float("inf")])
def test_nonfinite_values_have_stable_code(value):
    with pytest.raises(CompositeValueError) as excinfo:
        composite_json_bytes(Composite("bad", [Node(
            "group", transform=CompositeTransform(translation_cm=(value, 0, 0)))]))
    assert excinfo.value.code == "MH_E_NAN_INF_VALUE"


def test_negative_scale_is_valid_but_zero_is_not():
    parsed = parse_composite({
        "v": 5,
        "nodes": [{"kind": "group", "transform": {"scale": [-1, 1, 1]}}],
    })
    assert parsed.nodes[0].transform.scale == (-1.0, 1.0, 1.0)
    with pytest.raises(CompositeValueError) as excinfo:
        parse_composite({
            "v": 5,
            "nodes": [{"kind": "group", "transform": {"scale": [0, 1, 1]}}],
        })
    assert excinfo.value.code == "MH_E_INVALID_SCALE"


def test_quaternion_reader_tolerance_and_writer_canonicalization():
    accepted = parse_composite({"v": 5, "nodes": [{
        "kind": "group", "transform": {"rotation_quat": [0, 0, 0, -1.0005]}}]})
    assert accepted.nodes[0].transform.rotation_quat == (0.0, 0.0, 0.0, 1.0)
    with pytest.raises(CompositeValueError) as excinfo:
        parse_composite({"v": 5, "nodes": [{
            "kind": "group", "transform": {"rotation_quat": [0, 0, 0, 1.002]}}]})
    assert excinfo.value.code == "MH_E_COMPOSITE_GRAMMAR"


def test_quaternion_canonical_write_is_stable_after_read_back():
    resource = Composite("real_dagor_rotation", [Node(
        "group",
        transform=CompositeTransform(rotation_quat=(
            -0.12278767675161362,
            -0.12278766930103302,
            -0.696364164352417,
            0.696364164352417,
        )),
    )])
    canonical = composite_json_bytes(resource)

    assert composite_json_bytes(parse_composite(canonical)) == canonical


def test_dependency_diamond_is_not_a_cycle():
    graph = {
        "root": Composite("root", [
            Node("composite", resource="left"),
            Node("composite", resource="right"),
        ]),
        "left": Composite("left", [Node("composite", resource="leaf")]),
        "right": Composite("right", [Node("composite", resource="leaf")]),
        "leaf": Composite("leaf"),
    }
    validate_composite_cycles("root", graph)


def test_read_uses_filename_as_identity(tmp_path):
    path = tmp_path / "building.composite"
    path.write_bytes(b'{"v":5,"nodes":[]}')
    assert read_composite_file(path) == Composite("building", [])


@pytest.mark.parametrize("filename", ["Building.composite", "building.COMPOSITE"])
def test_read_rejects_noncanonical_filename_identity(tmp_path, filename):
    path = tmp_path / filename
    path.write_bytes(b'{"v":5,"nodes":[]}')
    with pytest.raises(CompositeValueError) as excinfo:
        read_composite_file(path)
    assert excinfo.value.code == "MH_E_NONCANONICAL_RESOURCE_NAME"


def test_place_type_and_seed_boundary_occupy_the_ratified_node_order():
    resource = Composite("vehicle", [
        Node("mesh", resource="hood", name="Hood",
             transform=CompositeTransform(translation_cm=(1.0, 0.0, 0.0)),
             profile="scatter", place_type=3, appearance_seed_boundary=True),
    ])
    document = composite_document(resource)
    assert list(document["nodes"][0]) == [
        "kind", "resource", "name", "transform", "profile", "place_type",
        "appearance_seed_boundary"]
    canonical = composite_json_bytes(resource)
    assert composite_json_bytes(parse_composite(canonical)) == canonical
    assert parse_composite(canonical, name="vehicle") == resource


def test_absent_carriers_are_not_zero_and_defaults_stay_byte_identical():
    plain = Composite("vehicle", [Node("group")])
    assert composite_json_bytes(plain) == (
        b'{\n  "v": 5,\n  "nodes": [\n    {\n      "kind": "group"\n    }\n  ]\n}\n')
    node = parse_composite(composite_json_bytes(plain)).nodes[0]
    assert node.place_type is None
    assert node.appearance_seed_boundary is False
    explicit_zero = Composite("vehicle", [Node("group", place_type=0)])
    assert composite_json_bytes(explicit_zero) == (
        b'{\n  "v": 5,\n  "nodes": [\n    {\n      "kind": "group",\n'
        b'      "place_type": 0\n    }\n  ]\n}\n')
    assert parse_composite(composite_json_bytes(explicit_zero)).nodes[0].place_type == 0
    assert composite_json_bytes(
        Composite("vehicle", [Node("group", appearance_seed_boundary=False)])
    ) == composite_json_bytes(plain)


@pytest.mark.parametrize("place_type", [7, 64, 2147483647])
def test_unknown_nonnegative_place_type_passes_as_provenance(place_type):
    document = {"v": 5, "nodes": [{"kind": "group", "place_type": place_type}]}
    parsed = parse_composite(document)
    assert parsed.nodes[0].place_type == place_type
    assert json.loads(composite_json_bytes(parsed)) == document


@pytest.mark.parametrize("place_type", [-1, 1.5, True, "3", None, 2147483648])
def test_negative_or_non_integer_place_type_is_grammar(place_type):
    with pytest.raises(CompositeValueError) as excinfo:
        parse_composite({"v": 5, "nodes": [
            {"kind": "group", "place_type": place_type}]})
    assert excinfo.value.code == "MH_E_COMPOSITE_GRAMMAR"
    if place_type is None:
        # An absent DTO carrier is the "not stated" case, not a rejected value.
        return
    with pytest.raises(CompositeValueError) as writer:
        composite_document(Composite("v", [Node("group", place_type=place_type)]))
    assert writer.value.code == "MH_E_COMPOSITE_GRAMMAR"


@pytest.mark.parametrize("value", [0, 1, "true", None])
def test_non_boolean_seed_boundary_is_grammar(value):
    with pytest.raises(CompositeValueError) as excinfo:
        parse_composite({"v": 5, "nodes": [
            {"kind": "group", "appearance_seed_boundary": value}]})
    assert excinfo.value.code == "MH_E_COMPOSITE_GRAMMAR"


@pytest.mark.parametrize("field", ["place_type", "appearance_seed_boundary"])
def test_random_options_never_carry_node_metadata_carriers(field):
    with pytest.raises(CompositeValueError) as excinfo:
        parse_composite({"v": 5, "nodes": [{"kind": "random", "options": [
            {"kind": "mesh", "resource": "hood", "weight": 1, field: 1}]}]})
    assert excinfo.value.code == "MH_E_COMPOSITE_GRAMMAR"
