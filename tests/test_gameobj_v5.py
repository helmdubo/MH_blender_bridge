"""Owner doc13 amendment 1: named non-executable gameobj, not actor fallbacks."""

import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "addon"))
from mh4blend.core.composites import composite_json_bytes, parse_composite
from mh4blend.core.source_closure import build_composite_source_closure
from tools import mh_random_reference as reference


def test_gameobj_codec_and_seed_free_closure_do_not_require_endpoint_sources():
    source = {"v": 5, "nodes": [
        {"kind": "gameobj", "resource": "dummy_pivot",
         "transform": {"translation_cm": [100, 0, 0]}},
        {"kind": "random", "options": [
            {"kind": "gameobj", "resource": "loot_a", "weight": 1},
            {"kind": "gameobj", "resource": "loot_b", "weight": 0},
            {"kind": "empty", "weight": 1}]}]}
    parsed = parse_composite(source, name="gameobj_root")
    canonical = composite_json_bytes(parsed)
    assert json.loads(canonical) == source
    assert composite_json_bytes(parse_composite(canonical)) == canonical
    closure = build_composite_source_closure("gameobj_root", {"gameobj_root": parsed})
    assert tuple(map(str, closure.resources)) == ("composite:gameobj_root",)


def test_retired_marker_kind_is_not_admitted_by_either_codec_direction():
    with pytest.raises(ValueError, match="MH_E_UNSUPPORTED_NODE_KIND"):
        parse_composite({"v": 5, "nodes": [
            {"kind": "marker", "resource": "dummy_pivot"}]})
    with pytest.raises(ValueError, match="MH_E_UNSUPPORTED_NODE_KIND"):
        parse_composite({"v": 5, "nodes": [{"kind": "random", "options": [
            {"kind": "marker", "resource": "loot_a", "weight": 1}]}]})


@pytest.mark.parametrize("option", [False, True])
@pytest.mark.parametrize("resource", [None, "", "Bad Name", "x/y"])
def test_gameobj_requires_a_canonical_resource_token(option, resource):
    node = {"kind": "gameobj"}
    if resource is not None:
        node["resource"] = resource
    if option:
        node["weight"] = 1
        node = {"kind": "random", "options": [node]}
    with pytest.raises(ValueError, match="MH_E_COMPOSITE_GRAMMAR"):
        parse_composite({"v": 5, "nodes": [node]})


def test_gameobj_reference_preserves_named_nodes_without_leaves_or_dependencies():
    root = reference.Composite("gameobj_root", (
        reference.Node("gameobj", "dummy_pivot",
                       reference.TRS(translation_cm=(100, 0, 0)), children=(
                           reference.Node("group",
                               transform=reference.TRS(translation_cm=(25, 0, 0))),)),
        reference.Node("random", options=(
            reference.RandomOption("gameobj", 1, "loot_a"),
            reference.RandomOption("gameobj", 0, "loot_b"))),
    ))
    plan = reference.resolve_composite(
        "gameobj_root", 100, {"gameobj_root": root}, {},
        {reference.ResourceKey("composite", "gameobj_root"): "blake3-160:" + "1" * 40})
    assert plan.leaves == ()
    assert not any(item.startswith(("gameobj:", "actor:"))
                   for item in plan.selected_dependencies)
    assert plan.decisions[0].option == 0
    assert len(plan.draws) == 1
    named = {node.path: node for node in plan.nodes if node.kind == "gameobj"}
    assert named["gameobj_root:nodes[0]"].resource == "dummy_pivot"
    assert named["gameobj_root:nodes[0]"].world_trs.translation_cm == (100, 0, 0)
    assert named["gameobj_root:nodes[1]/options[0]"].resource == "loot_a"
    assert next(node for node in plan.nodes
                if node.path == "gameobj_root:nodes[0]/children[0]").world_trs.translation_cm == (125, 0, 0)
    assert "nodes" not in json.loads(plan.signature_preimage)
