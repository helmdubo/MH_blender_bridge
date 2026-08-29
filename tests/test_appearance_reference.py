"""V5-S6.3 gates for the ``mh.appearance:1`` stage and the dual seed.

Acceptance 4 (seed independence), acceptance 6 (boundary scenarios) and the
reference-against-golden parity of ``golden/v5/appearance/``.  The layout stage
is only ever asserted to be untouched here; its own gates stay in
``test_random_reference.py``.
"""

from pathlib import Path

import pytest

from addon.mh4blend.core.canonical_json import narrow_float32
from tools.mh_random_fixture import (
    appearance_golden_bytes,
    boundary_scenario,
    boundary_scenario_names,
    synthetic_fixture,
)
from tools.mh_random_reference import (
    APPEARANCE_TAG,
    MH_APPEARANCE_CHANNELS,
    RESOLVER_TAG,
    Composite,
    Node,
    RandomOption,
    RandomReferenceError,
    ResourceKey,
    TRS,
    node_random_stream,
    placement_signature,
    raw_payload_hash,
    resolve_appearance,
    resolve_composite,
)


REPO_ROOT = Path(__file__).resolve().parent.parent
APPEARANCE_GOLDEN_PATH = (
    REPO_ROOT / "golden" / "v5" / "appearance" / "appearance_1_vectors.json")
LAYOUT_GOLDEN_PATH = REPO_ROOT / "golden" / "v5" / "random_stream_1_vectors.json"


def _plan(seed=42):
    root, composites, profiles, hashes = synthetic_fixture()
    return resolve_composite(root, seed, composites, profiles, hashes)


def _channels(appearance, path):
    return next(leaf.raw_u32 for leaf in appearance.leaves if leaf.path == path)


def test_ratified_constants_and_tags_are_frozen():
    assert MH_APPEARANCE_CHANNELS == 4
    assert APPEARANCE_TAG == "mh.appearance:1"
    # The layout resolver tag must NOT move: the layout stream did not change.
    assert RESOLVER_TAG == "mh.random_resolver:2"


def test_appearance_seed_domain_is_exact_int32():
    plan = _plan()
    for invalid in (True, 1.0, -(1 << 31) - 1, 1 << 31):
        with pytest.raises(RandomReferenceError, match="int32"):
            resolve_appearance(plan, invalid)


def test_every_leaf_draws_exactly_the_ratified_channel_count_in_order():
    plan = _plan()
    appearance = resolve_appearance(plan, 7)
    assert appearance.channels == MH_APPEARANCE_CHANNELS
    assert len(appearance.leaves) == len(plan.leaves)
    assert len(appearance.draws) == len(plan.leaves) * MH_APPEARANCE_CHANNELS
    expected = [
        (index, channel)
        for index in range(len(plan.leaves))
        for channel in range(MH_APPEARANCE_CHANNELS)
    ]
    assert [(draw.leaf_index, draw.channel) for draw in appearance.draws] == expected
    for leaf in appearance.leaves:
        assert len(leaf.raw_u32) == MH_APPEARANCE_CHANNELS
        assert all(0 <= value <= 0xFFFFFFFF for value in leaf.raw_u32)


def test_channel_unit_is_the_float32_derivative_of_the_authoritative_raw():
    appearance = resolve_appearance(_plan(), 11)
    for draw in appearance.draws:
        assert draw.unit == narrow_float32(draw.raw_u32 * 2.0 ** -32)


def test_leaf_stream_is_the_boundary_path_stream_reopened_per_leaf():
    plan = _plan()
    appearance = resolve_appearance(plan, 2024)
    for leaf in appearance.leaves:
        stream = node_random_stream(2024, leaf.boundary)
        assert leaf.raw_u32 == tuple(
            stream.next_u32() for _ in range(MH_APPEARANCE_CHANNELS))


def test_appearance_stage_leaves_every_layout_array_byte_identical():
    root, composites, profiles, hashes = synthetic_fixture()
    before = resolve_composite(root, 42, composites, profiles, hashes)
    appearance = resolve_appearance(before, 99)
    after = resolve_composite(root, 42, composites, profiles, hashes)
    assert before.draws == after.draws
    assert before.decisions == after.decisions
    assert before.leaves == after.leaves
    assert before.signature_preimage == after.signature_preimage
    assert before.resolved_signature == after.resolved_signature
    assert appearance.draws  # the appearance draws live in their own array
    assert not set(id(draw) for draw in appearance.draws) & set(
        id(draw) for draw in before.draws)


def test_appearance_signature_preimage_carries_every_ratified_term():
    plan = _plan()
    appearance = resolve_appearance(plan, 123)
    preimage = appearance.signature_preimage
    assert preimage.endswith(b"\n")
    assert b'"stage": "mh.appearance:1"' in preimage
    assert b'"appearance_seed": 123' in preimage
    assert b'"channels": 4' in preimage
    assert b'"boundaries"' in preimage
    for leaf in appearance.leaves:
        assert f'"{leaf.boundary}"'.encode("utf-8") in preimage
        for raw in leaf.raw_u32:
            assert str(raw).encode("ascii") in preimage
    assert appearance.appearance_signature.startswith("blake3-160:")
    assert len(appearance.appearance_signature) == len("blake3-160:") + 40


def test_channel_count_constant_is_inside_the_appearance_preimage():
    plan = _plan()
    four = resolve_appearance(plan, 5)
    three = resolve_appearance(plan, 5, channels=3)
    assert three.appearance_signature != four.appearance_signature


# --- acceptance 4: dual seed independence -----------------------------------

def test_appearance_seed_reroll_keeps_resolved_signature_and_moves_appearance():
    root, composites, profiles, hashes = synthetic_fixture()
    plan = resolve_composite(root, 42, composites, profiles, hashes)
    first = resolve_appearance(plan, 1)
    second = resolve_appearance(plan, 2)
    # The layout side is not merely equal, it is the same immutable object.
    assert first.appearance_signature != second.appearance_signature
    assert plan.resolved_signature == resolve_composite(
        root, 42, composites, profiles, hashes).resolved_signature
    assert placement_signature(
        plan.resolved_signature, first.appearance_signature) != placement_signature(
        plan.resolved_signature, second.appearance_signature)


def test_layout_seed_reroll_moves_resolved_signature():
    root, composites, profiles, hashes = synthetic_fixture()
    first = resolve_composite(root, 0, composites, profiles, hashes)
    second = resolve_composite(root, 2147483647, composites, profiles, hashes)
    assert first.resolved_signature != second.resolved_signature


def test_appearance_signature_moves_only_with_the_leaf_set():
    root, composites, profiles, hashes = synthetic_fixture()
    stable = resolve_composite(root, 1, composites, profiles, hashes)
    same_paths = resolve_composite(root, 2, composites, profiles, hashes)
    changed = resolve_composite(root, 0, composites, profiles, hashes)
    assert [leaf.origin for leaf in stable.leaves] == [
        leaf.origin for leaf in same_paths.leaves]
    assert [leaf.origin for leaf in stable.leaves] != [
        leaf.origin for leaf in changed.leaves]
    assert resolve_appearance(stable, 77).appearance_signature == (
        resolve_appearance(same_paths, 77).appearance_signature)
    assert resolve_appearance(stable, 77).appearance_signature != (
        resolve_appearance(changed, 77).appearance_signature)


def _survivor_fixture():
    """A boundary-keyed leaf that survives every random choice above it."""
    composites = {
        "survivor_cmp": Composite("survivor_cmp", (
            Node(
                "mesh",
                resource="survivor_mesh",
                transform=TRS(translation_cm=(1, 0, 0)),
                appearance_seed_boundary=True,
            ),
            Node("random", options=(
                RandomOption("mesh", resource="pick_a_mesh", weight=1),
                RandomOption("mesh", resource="pick_b_mesh", weight=3),
            )),
        )),
    }
    keys = (
        ResourceKey("composite", "survivor_cmp"),
        ResourceKey("static_mesh", "survivor_mesh"),
        ResourceKey("static_mesh", "pick_a_mesh"),
        ResourceKey("static_mesh", "pick_b_mesh"),
    )
    hashes = {
        key: raw_payload_hash(f"survivor\n{key}\n".encode("ascii"))
        for key in keys
    }
    return "survivor_cmp", composites, {}, hashes


def test_leaf_that_survives_a_topology_change_keeps_its_channels_by_nodepath():
    root, composites, profiles, hashes = _survivor_fixture()
    left = resolve_composite(root, 0, composites, profiles, hashes)
    right = resolve_composite(root, 42, composites, profiles, hashes)
    left_choice = left.decisions[0].option
    right_choice = right.decisions[0].option
    assert left_choice != right_choice, "fixture must actually change topology"

    survivor = "survivor_cmp:nodes[0]"
    left_appearance = resolve_appearance(left, 4242)
    right_appearance = resolve_appearance(right, 4242)
    assert _channels(left_appearance, survivor) == _channels(
        right_appearance, survivor)


# --- acceptance 6: the three ratified boundary scenarios ---------------------

def test_house_without_boundaries_shares_one_stream_for_the_whole_subtree():
    root, composites, profiles, hashes = boundary_scenario("house_shared")
    plan = resolve_composite(root, 42, composites, profiles, hashes)
    appearance = resolve_appearance(plan, 1234)
    assert len(appearance.leaves) >= 3
    assert {leaf.boundary for leaf in appearance.leaves} == {root}
    assert appearance.boundaries == (root,)
    unique = {leaf.raw_u32 for leaf in appearance.leaves}
    assert len(unique) == 1


def test_boundary_on_every_leaf_gives_every_leaf_its_own_stream():
    root, composites, profiles, hashes = boundary_scenario("fabric_per_leaf")
    plan = resolve_composite(root, 42, composites, profiles, hashes)
    appearance = resolve_appearance(plan, 1234)
    assert len(appearance.leaves) >= 3
    assert {leaf.boundary for leaf in appearance.leaves} == {
        leaf.path for leaf in appearance.leaves}
    unique = {leaf.raw_u32 for leaf in appearance.leaves}
    assert len(unique) == len(appearance.leaves)


def test_boundary_on_a_composite_node_shares_its_whole_subtree():
    root, composites, profiles, hashes = boundary_scenario("nested_boundary")
    plan = resolve_composite(root, 42, composites, profiles, hashes)
    appearance = resolve_appearance(plan, 1234)
    boundary_path = "shop_cmp:nodes[0]"
    inside = [leaf for leaf in appearance.leaves if leaf.boundary == boundary_path]
    outside = [leaf for leaf in appearance.leaves if leaf.boundary == root]
    assert len(inside) >= 2 and len(outside) >= 1
    assert len({leaf.raw_u32 for leaf in inside}) == 1
    assert inside[0].raw_u32 != outside[0].raw_u32
    assert appearance.boundaries == tuple(sorted({root, boundary_path}))


def test_actor_leaves_draw_and_gameobj_or_empty_options_do_not():
    root, composites, profiles, hashes = boundary_scenario("mixed_endpoints")
    plan = resolve_composite(root, 42, composites, profiles, hashes)
    appearance = resolve_appearance(plan, 5)
    kinds = {leaf.kind for leaf in plan.leaves}
    assert "actor" in kinds
    assert len(appearance.leaves) == len(plan.leaves)
    assert [leaf.path for leaf in appearance.leaves] == [
        leaf.origin for leaf in plan.leaves]
    assert any(node.kind == "gameobj" for node in plan.nodes)
    assert not any(leaf.kind == "gameobj" for leaf in plan.leaves)


# --- PlacementSignature -----------------------------------------------------

def test_placement_signature_is_the_hash_of_the_two_signature_byte_strings():
    from tools.mh_random_reference import raw_payload_hash as blake3_160

    plan = _plan()
    appearance = resolve_appearance(plan, 8)
    expected = blake3_160(
        plan.resolved_signature.encode("ascii")
        + appearance.appearance_signature.encode("ascii"))
    assert placement_signature(
        plan.resolved_signature, appearance.appearance_signature) == expected


def test_placement_signature_rejects_anything_that_is_not_a_signature():
    with pytest.raises(RandomReferenceError, match="BLAKE3-160"):
        placement_signature("nope", "blake3-160:" + "0" * 40)


# --- golden parity ----------------------------------------------------------

def test_appearance_scenario_set_is_the_ratified_one():
    assert boundary_scenario_names() == (
        "house_shared",
        "fabric_per_leaf",
        "nested_boundary",
        "mixed_endpoints",
    )


def test_appearance_golden_is_byte_identical_to_the_reference():
    assert APPEARANCE_GOLDEN_PATH.read_bytes() == appearance_golden_bytes()


def test_layout_golden_file_is_not_regenerated_by_this_slice():
    # Byte-level proof that adding the appearance stage did not move layout.
    from tools.mh_random_fixture import golden_bytes

    assert LAYOUT_GOLDEN_PATH.read_bytes() == golden_bytes()
