"""Pure tests for the v5 all-options composite source-closure graph."""

from dataclasses import FrozenInstanceError
import inspect
from pathlib import Path
import sys

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.composites import CompositeValueError  # noqa: E402
from mh4blend.core.model import Composite, Node, RandomOption  # noqa: E402
from mh4blend.core.source_closure import (  # noqa: E402
    CompositeSourceClosure,
    ResourceKey,
    build_composite_source_closure,
)


def test_closure_api_has_no_seed_surface_and_values_are_immutable():
    assert "seed" not in inspect.signature(
        build_composite_source_closure).parameters
    closure = build_composite_source_closure("root", {
        "root": Composite("root"),
    })
    with pytest.raises(FrozenInstanceError):
        closure.root = ResourceKey("composite", "other")
    with pytest.raises(FrozenInstanceError):
        closure.root.name = "other"


def test_every_random_option_and_nested_child_enters_source_closure():
    graph = {
        "root": Composite("root", [
            Node("random", profile="root_scatter", options=[
                RandomOption("composite", 1, "selected"),
                RandomOption("composite", 0, "zero_weight"),
                RandomOption("mesh", 0, "unused_mesh"),
                RandomOption("empty", 0),
            ], children=[
                Node("mesh", resource="nested_mesh", profile="nested_profile"),
            ]),
        ]),
        "selected": Composite("selected", [
            Node("mesh", resource="selected_mesh"),
        ]),
        "zero_weight": Composite("zero_weight", [
            Node("mesh", resource="zero_weight_mesh"),
        ]),
    }

    closure = build_composite_source_closure("root", graph)

    assert closure.placement_profiles == (
        ResourceKey("placement_profile", "root_scatter"),
        ResourceKey("placement_profile", "nested_profile"),
    )
    assert closure.static_meshes == (
        ResourceKey("static_mesh", "unused_mesh"),
        ResourceKey("static_mesh", "nested_mesh"),
        ResourceKey("static_mesh", "selected_mesh"),
        ResourceKey("static_mesh", "zero_weight_mesh"),
    )
    assert closure.composites_postorder == (
        ResourceKey("composite", "selected"),
        ResourceKey("composite", "zero_weight"),
        ResourceKey("composite", "root"),
    )
    assert closure.resources == (
        *closure.placement_profiles,
        *closure.static_meshes,
        *closure.composites_postorder,
    )
    assert closure.referrers_for(
        ResourceKey("static_mesh", "unused_mesh")) == (
            ResourceKey("composite", "root"),)
    assert closure.referrers_for(
        ResourceKey("static_mesh", "zero_weight_mesh")) == (
            ResourceKey("composite", "zero_weight"),)


def test_dependency_postorder_is_root_last_and_diamond_is_deduplicated():
    graph = {
        "root": Composite("root", [
            Node("composite", resource="left"),
            Node("composite", resource="right"),
        ]),
        "left": Composite("left", [Node("composite", resource="leaf")]),
        "right": Composite("right", [Node("composite", resource="leaf")]),
        "leaf": Composite("leaf"),
    }
    closure = build_composite_source_closure("root", graph)
    assert tuple(str(key) for key in closure.composites_postorder) == (
        "composite:leaf",
        "composite:left",
        "composite:right",
        "composite:root",
    )
    assert closure.composites_postorder.count(
        ResourceKey("composite", "leaf")) == 1
    assert closure.composites_postorder[-1] == closure.root
    assert closure.referrers_for(ResourceKey("composite", "leaf")) == (
        ResourceKey("composite", "left"),
        ResourceKey("composite", "right"),
    )


def test_zero_weight_cycle_fails_closed_with_existing_code():
    graph = {
        "root": Composite("root", [Node("random", options=[
            RandomOption("empty", 1),
            RandomOption("composite", 0, "cycle"),
        ])]),
        "cycle": Composite("cycle", [Node("composite", resource="root")]),
    }
    with pytest.raises(CompositeValueError) as caught:
        build_composite_source_closure("root", graph)
    assert caught.value.code == "MH_E_COMPOSITE_CYCLE"
    assert "root -> cycle -> root" in str(caught.value)


def test_missing_nested_composite_fails_closed_with_existing_code():
    calls = []

    def resolver(name):
        calls.append(name)
        if name == "root":
            return Composite("root", [Node("composite", resource="missing")])
        return None

    with pytest.raises(CompositeValueError) as caught:
        build_composite_source_closure("root", resolver)
    assert caught.value.code == "MH_E_UNRESOLVED_COMPOSITE_REFERENCE"
    assert caught.value.path == "missing"
    assert calls == ["root", "missing"]


def test_resolver_is_called_once_per_diamond_member():
    graph = {
        "root": Composite("root", [
            Node("composite", resource="left"),
            Node("composite", resource="right"),
        ]),
        "left": Composite("left", [Node("composite", resource="leaf")]),
        "right": Composite("right", [Node("composite", resource="leaf")]),
        "leaf": Composite("leaf"),
    }
    calls = []

    def resolver(name):
        calls.append(name)
        return graph.get(name)

    closure = build_composite_source_closure("root", resolver)
    assert isinstance(closure, CompositeSourceClosure)
    assert calls == ["root", "left", "leaf", "right"]


@pytest.mark.parametrize("kind", ["actor", "mesh", "bogus"])
def test_resource_key_rejects_non_protocol_or_wire_node_kind(kind):
    with pytest.raises(ValueError):
        ResourceKey(kind, "thing")


def test_resource_key_rejects_noncanonical_logical_name_without_repair():
    with pytest.raises(ValueError, match="MH_E_NONCANONICAL_RESOURCE_NAME"):
        ResourceKey("composite", "Bad Name")
