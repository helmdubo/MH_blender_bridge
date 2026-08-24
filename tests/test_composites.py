"""Pure Source Protocol v4 composite codec gates."""

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
    iter_resource_references,
    parse_composite,
    read_composite_file,
    validate_composite_cycles,
)
from mh4blend.core.model import (  # noqa: E402
    Composite,
    CompositeTransform,
    Node,
)


def test_shared_composite_golden_vectors_are_byte_exact_and_roundtrip():
    fixture = json.loads(
        (REPO_ROOT / "golden" / "composite_v4_vectors.json").read_text(
            encoding="utf-8"))
    assert fixture["schema"] == "mh.composite_v4_vectors"
    for vector in fixture["vectors"]:
        expected = vector["canonical_utf8"].encode("utf-8")
        assert composite_json_bytes(vector["value"]) == expected, vector["name"]
        assert composite_json_bytes(parse_composite(expected)) == expected
    for vector in fixture["negative_vectors"]:
        with pytest.raises(CompositeValueError) as excinfo:
            parse_composite(vector["json"])
        assert excinfo.value.code == vector["error"], vector["name"]


def test_closed_root_node_and_transform_grammar():
    invalid = [
        {},
        {"nodes": [], "schema": 4},
        {"nodes": {}},
        {"nodes": [{"kind": "mesh", "resource": "body", "materials": []}]},
        {"nodes": [{"kind": "mesh", "resource": "body",
                    "transform": {"location": [0, 0, 0]}}]},
        {"nodes": [{"kind": "mesh"}]},
        {"nodes": [{"kind": "group", "resource": "forbidden"}]},
        {"nodes": [{"kind": "group", "name": ""}]},
        {"nodes": [{"kind": "group", "children": {}}]},
        {"nodes": [{"kind": "mesh", "resource": "Body"}]},
    ]
    for document in invalid:
        with pytest.raises(CompositeValueError) as excinfo:
            parse_composite(document)
        assert excinfo.value.code == "MH_E_COMPOSITE_GRAMMAR"


def test_unknown_kind_has_specific_code():
    with pytest.raises(CompositeValueError) as excinfo:
        parse_composite({"nodes": [{"kind": "light", "resource": "lamp"}]})
    assert excinfo.value.code == "MH_E_UNSUPPORTED_NODE_KIND"


def test_identity_defaults_empty_children_and_negative_identity_sign_are_omitted():
    source = {
        "nodes": [{
            "kind": "mesh",
            "resource": "body",
            "transform": {
                "translation_cm": [0, -0.0, 0],
                "rotation_quat": [0, 0, 0, -1],
                "scale": [1, 1, 1],
            },
            "children": [],
        }],
    }
    assert composite_document(parse_composite(source)) == {
        "nodes": [{"kind": "mesh", "resource": "body"}]}


def test_field_and_node_order_is_significant_and_preserved():
    resource = Composite("vehicle", [
        Node("mesh", resource="hood"),
        Node("group", name="lights", children=[
            Node("actor", resource="lamp_right"),
            Node("actor", resource="lamp_left"),
        ]),
    ])
    document = composite_document(resource)
    assert list(document) == ["nodes"]
    assert list(document["nodes"][0]) == ["kind", "resource"]
    assert [node.resource for node in resource.nodes[1].children] == [
        "lamp_right", "lamp_left"]
    assert list(iter_resource_references(resource)) == [
        "hood", "lamp_right", "lamp_left"]


def test_reader_detects_duplicate_keys_at_any_depth():
    with pytest.raises(CompositeValueError, match="duplicate JSON field"):
        parse_composite(
            '{"nodes":[{"kind":"mesh","resource":"a","resource":"b"}]}')


@pytest.mark.parametrize("value", [float("nan"), float("inf"), -float("inf")])
def test_nonfinite_values_have_stable_code(value):
    with pytest.raises(CompositeValueError) as excinfo:
        composite_json_bytes(Composite("bad", [Node(
            "group", transform=CompositeTransform(translation_cm=(value, 0, 0)))]))
    assert excinfo.value.code == "MH_E_NAN_INF_VALUE"


@pytest.mark.parametrize("scale", [[0, 1, 1], [-1, 1, 1]])
def test_nonpositive_scale_has_stable_code(scale):
    with pytest.raises(CompositeValueError) as excinfo:
        parse_composite({"nodes": [{
            "kind": "group", "transform": {"scale": scale}}]})
    assert excinfo.value.code == "MH_E_INVALID_SCALE"


def test_quaternion_reader_tolerance_and_writer_canonicalization():
    accepted = parse_composite({"nodes": [{
        "kind": "group", "transform": {"rotation_quat": [0, 0, 0, -1.0005]}}]})
    assert accepted.nodes[0].transform.rotation_quat == (0.0, 0.0, 0.0, 1.0)

    with pytest.raises(CompositeValueError) as excinfo:
        parse_composite({"nodes": [{
            "kind": "group", "transform": {"rotation_quat": [0, 0, 0, 1.002]}}]})
    assert excinfo.value.code == "MH_E_COMPOSITE_GRAMMAR"

    half_turn = Composite("half", [Node(
        "group", transform=CompositeTransform(
            rotation_quat=(-2.0, 0.0, 0.0, 0.0)))])
    assert composite_document(half_turn)["nodes"][0]["transform"][
        "rotation_quat"] == [1.0, 0.0, 0.0, 0.0]


def test_cycle_and_unresolved_composite_dependencies_fail_closed():
    graph = {
        "root": Composite("root", [Node("composite", resource="child")]),
        "child": Composite("child", [Node("composite", resource="root")]),
    }
    with pytest.raises(CompositeValueError) as excinfo:
        validate_composite_cycles("root", graph)
    assert excinfo.value.code == "MH_E_COMPOSITE_CYCLE"

    graph["child"] = Composite(
        "child", [Node("composite", resource="missing")])
    with pytest.raises(CompositeValueError) as excinfo:
        validate_composite_cycles("root", graph)
    assert excinfo.value.code == "MH_E_UNRESOLVED_COMPOSITE_REFERENCE"


def test_dependency_diamond_is_not_a_cycle():
    graph = {
        "root": Composite("root", [
            Node("composite", resource="left"),
            Node("composite", resource="right"),
        ]),
        "left": Composite("left", [Node("composite", resource="leaf")]),
        "right": Composite("right", [Node("composite", resource="leaf")]),
        "leaf": Composite("leaf", []),
    }
    validate_composite_cycles("root", graph)


def test_read_uses_filename_as_identity(tmp_path):
    path = tmp_path / "building.composite"
    path.write_bytes(b'{"nodes":[]}')
    assert read_composite_file(path) == Composite("building", [])


@pytest.mark.parametrize("filename", ["Building.composite", "building.COMPOSITE"])
def test_read_rejects_noncanonical_filename_identity(tmp_path, filename):
    path = tmp_path / filename
    path.write_bytes(b'{"nodes":[]}')
    with pytest.raises(CompositeValueError) as excinfo:
        read_composite_file(path)
    assert excinfo.value.code == "MH_E_NONCANONICAL_RESOURCE_NAME"
