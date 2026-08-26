"""Bpy-free gates for Source Protocol v5 ``mh.random_stream:1``."""

from dataclasses import FrozenInstanceError
import inspect
from pathlib import Path

import pytest

from tools.mh_random_fixture import golden_bytes, synthetic_fixture
from tools.mh_random_reference import (
    Composite,
    Node,
    PlacementProfile,
    RESOLVER_TAG,
    RandomOption,
    RandomReferenceError,
    RandomStream,
    Range,
    ResourceKey,
    TRS,
    build_source_closure,
    node_random_stream,
    path_hash64,
    placement_state,
    raw_payload_hash,
    resolve_composite,
    select_weighted,
)


REPO_ROOT = Path(__file__).resolve().parent.parent
GOLDEN_PATH = REPO_ROOT / "golden" / "v5" / "random_stream_1_vectors.json"


@pytest.mark.parametrize(
    ("seed", "initial_state", "draws"),
    [
        (-2147483648, 0x25493CC63225736C,
         (0xE3365903, 0x1B296251, 0x248A9F8D, 0x45DD9925)),
        (-1, 0x73B13BA2AFF181C0,
         (0xE91B2C10, 0xB1AC7401, 0x217E31FF, 0xD88C39F9)),
        (0, 0xE220A8397B1DCDAF,
         (0xA706DD2F, 0xB382A305, 0x631A9154, 0xA80ABA8C)),
        (1, 0x910A2DEC89025CC1,
         (0x5E41AB08, 0xF18D6CE9, 0x0B95F66D, 0xC7061B1B)),
        (2147483647, 0x61FA36A6261A4BE7,
         (0x0AD6C884, 0x1255E21E, 0xBECB6AC6, 0xAA00D3E6)),
    ],
)
def test_splitmix64_seed_bitcast_and_high_u32_are_exact(seed, initial_state, draws):
    stream = RandomStream(seed)
    assert stream.initial_state == initial_state
    assert tuple(stream.next_u32() for _ in draws) == draws


def test_seed_domain_is_exact_int32():
    for invalid in (True, 1.0, -(1 << 31) - 1, 1 << 31):
        with pytest.raises(RandomReferenceError, match="int32"):
            RandomStream(invalid)


def test_path_derived_stream_contract_has_frozen_hash_state_and_draws():
    node_path = "root_cmp:nodes[1]/options[2]>variant_b_cmp:nodes[0]"
    assert RESOLVER_TAG == "mh.random_resolver:2"
    assert placement_state(42) == 13679457532755275413
    assert path_hash64(node_path) == 17679296295052330425
    stream = node_random_stream(42, node_path)
    assert stream.initial_state == 6646601583332992347
    assert tuple(stream.next_u32() for _ in range(4)) == (
        868233470,
        2386588500,
        4091317401,
        72510459,
    )


def test_quaternion_sign_uses_v4_w_then_xyz_canonical_rule():
    trs = TRS(rotation_quat=(1, 0, 0, -1))
    assert trs.rotation_quat[0] < 0.0
    assert trs.rotation_quat[3] > 0.0
    half_turn = TRS(rotation_quat=(-1, 0, 0, 0))
    assert half_turn.rotation_quat == (1.0, 0.0, 0.0, 0.0)


def test_blake3_160_known_empty_vector():
    assert raw_payload_hash(b"") == (
        "blake3-160:af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9")


class _FixedU32Stream:
    def __init__(self, value):
        self.value = value
        self.calls = 0

    def next_u32(self):
        self.calls += 1
        return self.value


def test_weighted_selection_uses_float64_order_strict_boundary_and_one_draw():
    options = (
        RandomOption("empty", weight=0),
        RandomOption("empty", weight=1),
        RandomOption("empty", weight=1),
    )
    at_zero = _FixedU32Stream(0)
    assert select_weighted(at_zero, "root:nodes[0]", options).option == 1
    assert at_zero.calls == 1

    at_exact_half = _FixedU32Stream(1 << 31)
    decision = select_weighted(at_exact_half, "root:nodes[0]", options)
    assert decision.target == 1.0
    assert decision.option == 2
    assert at_exact_half.calls == 1

    single = _FixedU32Stream(0xFFFFFFFF)
    assert select_weighted(
        single,
        "root:nodes[1]",
        (RandomOption("empty", weight=7),),
    ).option == 0
    assert single.calls == 1


def test_source_closure_is_seed_free_sorted_and_contains_every_option():
    root, composites, profiles, hashes = synthetic_fixture()
    assert "seed" not in inspect.signature(build_source_closure).parameters
    closure = build_source_closure(root, composites, profiles, hashes)
    resources = tuple(str(key) for key in closure.resources)
    assert resources == tuple(sorted(resources))
    assert "composite:variant_a_cmp" in resources
    assert "composite:variant_b_cmp" in resources
    assert "static_mesh:variant_b0_mesh" in resources
    assert "static_mesh:variant_b1_mesh" in resources
    assert "placement_profile:full_profile" in resources
    assert closure.closure_hash == "blake3-160:cac2440c1b7f2e6d412ee362bd1be10eeb3f79fe"


def test_source_and_selected_closures_include_transitive_leaf_dependencies():
    root = "root"
    composites = {
        root: Composite(root, (Node("mesh", resource="mesh_a"),)),
    }
    mesh = ResourceKey("static_mesh", "mesh_a")
    material = ResourceKey("material", "material_a")
    texture = ResourceKey("texture", "texture_a")
    dependency_graph = {
        mesh: (material,),
        material: (texture,),
    }
    keys = (ResourceKey("composite", root), mesh, material, texture)
    hashes = {key: raw_payload_hash(str(key).encode("ascii")) for key in keys}

    closure = build_source_closure(
        root,
        composites,
        {},
        hashes,
        dependency_graph,
    )
    assert tuple(str(key) for key in closure.resources) == (
        "composite:root",
        "material:material_a",
        "static_mesh:mesh_a",
        "texture:texture_a",
    )
    plan = resolve_composite(
        root,
        42,
        composites,
        {},
        hashes,
        dependency_graph,
    )
    assert plan.selected_dependencies == (
        "static_mesh:mesh_a",
        "material:material_a",
        "texture:texture_a",
    )


def test_cycle_in_zero_weight_unselected_option_blocks_before_resolution():
    composites = {
        "root": Composite("root", (Node("random", options=(
            RandomOption("composite", resource="leaf", weight=1),
            RandomOption("composite", resource="cycle", weight=0),
        )),)),
        "leaf": Composite("leaf"),
        "cycle": Composite("cycle", (
            Node("composite", resource="root"),
        )),
    }
    with pytest.raises(RandomReferenceError, match="composite cycle"):
        build_source_closure("root", composites, {}, {})


def test_draw_order_absent_fields_and_single_positive_selection_are_frozen():
    root, composites, profiles, hashes = synthetic_fixture()
    plan_b = resolve_composite(root, 2147483647, composites, profiles, hashes)
    assert [entry.role for entry in plan_b.draws] == [
        "selection",
        "offset_x", "offset_y", "offset_z",
        "rotation_x", "rotation_y", "rotation_z",
        "uniform_scale", "vertical_scale",
        "selection",
        "selection",
        "offset_x", "offset_y", "offset_z",
    ]
    plan_a = resolve_composite(root, 0, composites, profiles, hashes)
    assert [entry.role for entry in plan_a.draws] == [
        "selection",
        "offset_x", "offset_y", "offset_z",
        "rotation_x", "rotation_y", "rotation_z",
        "uniform_scale", "vertical_scale",
        "selection",
        "offset_x", "offset_y", "offset_z",
    ]
    assert plan_a.decisions[-1].path == "root_cmp:nodes[1]/children[0]"
    assert plan_a.decisions[-1].option == 0


def _path_locality_fixture(
    *,
    extra_left=False,
    left_without_profile=False,
    insert_before_right=False,
):
    profile = PlacementProfile(
        "jitter",
        offset_cm=(Range(0, 5), Range(0, 3), Range(0, 1)),
    )

    def random_node(*, with_profile=True):
        return Node(
            "random",
            profile=profile.name if with_profile else None,
            options=(
                RandomOption("empty", weight=1),
                RandomOption("empty", weight=1),
            ),
        )

    left_children = [random_node(with_profile=not left_without_profile)]
    if extra_left:
        left_children.append(random_node())
    nodes = [Node("group", children=tuple(left_children))]
    if insert_before_right:
        nodes.append(Node("group"))
    nodes.append(Node("group", children=(random_node(),)))
    composites = {"locality": Composite("locality", tuple(nodes))}
    profiles = {profile.name: profile}
    keys = (
        ResourceKey("composite", "locality"),
        ResourceKey("placement_profile", profile.name),
    )
    hashes = {
        key: raw_payload_hash(f"locality\n{key}\n".encode("ascii"))
        for key in keys
    }
    return composites, profiles, hashes


def _path_decision(plan, path):
    return next(decision for decision in plan.decisions if decision.path == path)


def _path_draws(plan, path):
    return tuple(draw for draw in plan.draws if draw.path == path)


def test_edit_or_addition_in_one_branch_does_not_reshuffle_other_branch():
    base = _path_locality_fixture()
    edited = _path_locality_fixture(left_without_profile=True)
    added = _path_locality_fixture(extra_left=True)
    base_plan = resolve_composite("locality", 42, *base)
    edited_plan = resolve_composite("locality", 42, *edited)
    added_plan = resolve_composite("locality", 42, *added)
    right_path = "locality:nodes[1]/children[0]"

    for candidate in (edited_plan, added_plan):
        assert _path_decision(base_plan, right_path) == _path_decision(
            candidate, right_path)
        assert _path_draws(base_plan, right_path) == _path_draws(
            candidate, right_path)
    left_path = "locality:nodes[0]/children[0]"
    assert _path_draws(base_plan, left_path) != _path_draws(
        edited_plan, left_path)
    assert any(
        draw.path == "locality:nodes[0]/children[1]"
        for draw in added_plan.draws
    )
    assert base_plan.resolved_signature != added_plan.resolved_signature


def test_insertion_before_sibling_changes_shifted_subtree_path_and_samples():
    base = _path_locality_fixture()
    shifted = _path_locality_fixture(insert_before_right=True)
    base_plan = resolve_composite("locality", 42, *base)
    shifted_plan = resolve_composite("locality", 42, *shifted)
    old_path = "locality:nodes[1]/children[0]"
    shifted_path = "locality:nodes[2]/children[0]"

    assert _path_decision(base_plan, old_path).raw_u32 != _path_decision(
        shifted_plan, shifted_path).raw_u32
    assert _path_draws(base_plan, old_path) != _path_draws(
        shifted_plan, shifted_path)
    stable_left = "locality:nodes[0]/children[0]"
    assert _path_draws(base_plan, stable_left) == _path_draws(
        shifted_plan, stable_left)


def test_profile_only_node_opens_its_own_path_derived_stream():
    profile = PlacementProfile(
        "profile_only",
        offset_cm=(Range(0, 1), Range(0, 2), Range(0, 3)),
    )
    composites = {
        "root": Composite("root", (
            Node("mesh", resource="mesh_a", profile=profile.name),
        )),
    }
    profiles = {profile.name: profile}
    keys = (
        ResourceKey("composite", "root"),
        ResourceKey("placement_profile", profile.name),
        ResourceKey("static_mesh", "mesh_a"),
    )
    hashes = {
        key: raw_payload_hash(f"profile-only\n{key}\n".encode("ascii"))
        for key in keys
    }
    plan = resolve_composite("root", 123, composites, profiles, hashes)
    node_path = "root:nodes[0]"
    expected_stream = node_random_stream(123, node_path)

    assert plan.decisions == ()
    assert [draw.role for draw in plan.draws] == [
        "offset_x", "offset_y", "offset_z",
    ]
    assert plan.draws[0].raw_u32 == expected_stream.next_u32()


def test_parent_local_world_trs_nodepaths_selected_dependencies_and_signature():
    root, composites, profiles, hashes = synthetic_fixture()
    plan = resolve_composite(root, 0, composites, profiles, hashes)
    anchor = next(leaf for leaf in plan.leaves if leaf.resource == "anchor_mesh")
    assert anchor.world_trs.translation_cm == (125.0, 0.0, 0.0)
    assert anchor.origin == "root_cmp:nodes[0]/children[0]"

    variant = next(leaf for leaf in plan.leaves if leaf.resource == "variant_a_mesh")
    assert variant.origin == (
        "root_cmp:nodes[1]/options[1]>variant_a_cmp:nodes[0]")
    assert "composite:variant_a_cmp" in plan.selected_dependencies
    assert "composite:variant_b_cmp" not in plan.selected_dependencies
    assert "placement_profile:offset_only" in plan.selected_dependencies
    assert plan.resolved_signature == (
        "blake3-160:80a8cbbfa943eb6e3eb2f276cda128f3ec946596")
    assert plan.signature_preimage.endswith(b"\n")
    assert b'"resolver": "mh.random_resolver:2"' in plan.signature_preimage
    assert b'"draw": 472868437' in plan.signature_preimage


def test_plan_is_immutable_and_mapping_iteration_cannot_change_result():
    root, composites, profiles, hashes = synthetic_fixture()
    forward = resolve_composite(root, 42, composites, profiles, hashes)
    reverse = resolve_composite(
        root,
        42,
        dict(reversed(tuple(composites.items()))),
        dict(reversed(tuple(profiles.items()))),
        dict(reversed(tuple(hashes.items()))),
    )
    assert reverse == forward
    with pytest.raises(FrozenInstanceError):
        forward.seed = 7


def test_option_order_changes_result_without_changing_closure_membership():
    root, composites, profiles, hashes = synthetic_fixture()
    source_node = composites[root].nodes[1]
    swapped_root = Composite(root, (
        composites[root].nodes[0],
        Node(
            "random",
            transform=source_node.transform,
            profile=source_node.profile,
            options=(
                source_node.options[0],
                source_node.options[2],
                source_node.options[1],
            ),
            children=source_node.children,
        ),
    ))
    swapped = dict(composites)
    swapped[root] = swapped_root
    original_plan = resolve_composite(root, 2147483647, composites, profiles, hashes)
    swapped_plan = resolve_composite(root, 2147483647, swapped, profiles, hashes)
    assert original_plan.closure.closure_hash == swapped_plan.closure.closure_hash
    assert original_plan.decisions[0].option == swapped_plan.decisions[0].option == 2
    assert original_plan.leaves != swapped_plan.leaves
    assert original_plan.resolved_signature != swapped_plan.resolved_signature


def test_profile_validation_rejects_invalid_scale_domain_before_sampling():
    with pytest.raises(RandomReferenceError, match="strictly positive"):
        PlacementProfile("bad", uniform_scale=Range(0.5, 0.5))
    with pytest.raises(RandomReferenceError, match="non-negative"):
        Range(1, -0.1)


def test_shared_random_golden_is_byte_identical():
    assert GOLDEN_PATH.read_bytes() == golden_bytes()
