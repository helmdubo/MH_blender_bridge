"""Synthetic fixture and golden renderer for the v5 random reference."""

from __future__ import annotations

import json
from pathlib import Path
import sys

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from addon.mh4blend.core.canonical_json import canonical_json_bytes
from tools.mh_random_reference import (
    APPEARANCE_TAG,
    Composite,
    MH_APPEARANCE_CHANNELS,
    Node,
    PlacementProfile,
    RANDOM_STREAM_TAG,
    RESOLVER_TAG,
    RandomOption,
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
    resolve_placement,
)


SEEDS = (0, 1, 2, 42, 123, 1024, 2147483647)
STREAM_DRAW_COUNT = 16
NODE_STREAM_DRAW_COUNT = 4
NODE_STREAM_PATHS = (
    "root_cmp:nodes[1]",
    "root_cmp:nodes[1]/children[0]",
    "root_cmp:nodes[1]/options[2]>variant_b_cmp:nodes[0]",
)
GOLDEN_SCHEMA = "mh.random_stream_vectors:1"
SYNTHETIC_PAYLOAD_DOMAIN = "mh.random_reference_fixture:1"

APPEARANCE_GOLDEN_SCHEMA = "mh.appearance_vectors:1"
APPEARANCE_SEEDS = SEEDS
# Boundary scenarios are topology fixtures, so one ratified layout seed is
# enough for them; the appearance seed is what the scenarios vary.
SCENARIO_LAYOUT_SEED = 42
APPEARANCE_PAYLOAD_DOMAIN = "mh.appearance_reference_fixture:1"


def synthetic_fixture():
    profiles = {
        "full_profile": PlacementProfile(
            "full_profile",
            offset_cm=(Range(10, 1), Range(20, 2), Range(30, 3)),
            rotation_deg=(Range(5, 10), Range(-5, 20), Range(15, 30)),
            uniform_scale=Range(1, 0.25),
            vertical_scale=Range(1, 0.1),
        ),
        "offset_only": PlacementProfile(
            "offset_only",
            offset_cm=(Range(0, 5), Range(0, 0), Range(0, 2)),
        ),
    }
    composites = {
        "root_cmp": Composite("root_cmp", (
            Node(
                "group",
                transform=TRS(translation_cm=(100, 0, 0)),
                children=(Node(
                    "mesh",
                    resource="anchor_mesh",
                    transform=TRS(translation_cm=(25, 0, 0)),
                ),),
            ),
            Node(
                "random",
                transform=TRS(translation_cm=(1, 2, 3)),
                profile="full_profile",
                options=(
                    RandomOption("empty", weight=0),
                    RandomOption("composite", resource="variant_a_cmp", weight=1),
                    RandomOption("composite", resource="variant_b_cmp", weight=3),
                ),
                children=(Node(
                    "random",
                    profile="offset_only",
                    transform=TRS(translation_cm=(0, 0, 2)),
                    options=(RandomOption("mesh", resource="tail_mesh", weight=1),),
                ),),
            ),
        )),
        "variant_a_cmp": Composite("variant_a_cmp", (
            Node(
                "mesh",
                resource="variant_a_mesh",
                transform=TRS(translation_cm=(10, 0, 0)),
            ),
        )),
        "variant_b_cmp": Composite("variant_b_cmp", (
            Node(
                "random",
                transform=TRS(translation_cm=(0, 5, 0)),
                options=(
                    RandomOption("mesh", resource="variant_b0_mesh", weight=1),
                    RandomOption("mesh", resource="variant_b1_mesh", weight=1),
                ),
            ),
        )),
    }
    keys = (
        ResourceKey("composite", "root_cmp"),
        ResourceKey("composite", "variant_a_cmp"),
        ResourceKey("composite", "variant_b_cmp"),
        ResourceKey("placement_profile", "full_profile"),
        ResourceKey("placement_profile", "offset_only"),
        ResourceKey("static_mesh", "anchor_mesh"),
        ResourceKey("static_mesh", "tail_mesh"),
        ResourceKey("static_mesh", "variant_a_mesh"),
        ResourceKey("static_mesh", "variant_b0_mesh"),
        ResourceKey("static_mesh", "variant_b1_mesh"),
    )
    raw_hashes = {
        key: raw_payload_hash(
            f"{SYNTHETIC_PAYLOAD_DOMAIN}\n{key}\n".encode("ascii"))
        for key in keys
    }
    return "root_cmp", composites, profiles, raw_hashes


def _scenario_hashes(scenario: str, keys):
    return {
        key: raw_payload_hash(
            f"{APPEARANCE_PAYLOAD_DOMAIN}\n{scenario}\n{key}\n".encode("ascii"))
        for key in keys
    }


def _house_shared():
    """Doc 12 §3 «дом с согласованными окнами»: not a single boundary."""
    composites = {
        "house_cmp": Composite("house_cmp", (
            Node(
                "group",
                transform=TRS(translation_cm=(100, 0, 0)),
                children=(
                    Node("mesh", resource="window_mesh",
                         transform=TRS(translation_cm=(0, 0, 50))),
                    Node("mesh", resource="window_mesh",
                         transform=TRS(translation_cm=(0, 0, 150))),
                ),
            ),
            Node("mesh", resource="door_mesh"),
        )),
    }
    keys = (
        ResourceKey("composite", "house_cmp"),
        ResourceKey("static_mesh", "door_mesh"),
        ResourceKey("static_mesh", "window_mesh"),
    )
    return "house_cmp", composites, {}, _scenario_hashes("house_shared", keys)


def _fabric_per_leaf():
    """Doc 12 §3 «магазин тканей»: a boundary on every fabric leaf."""
    composites = {
        "fabric_cmp": Composite("fabric_cmp", (
            Node("mesh", resource="bolt_a_mesh", appearance_seed_boundary=True),
            Node("mesh", resource="bolt_b_mesh", appearance_seed_boundary=True),
            Node("group", transform=TRS(translation_cm=(0, 40, 0)), children=(
                Node("mesh", resource="bolt_c_mesh",
                     appearance_seed_boundary=True),
            )),
        )),
    }
    keys = (
        ResourceKey("composite", "fabric_cmp"),
        ResourceKey("static_mesh", "bolt_a_mesh"),
        ResourceKey("static_mesh", "bolt_b_mesh"),
        ResourceKey("static_mesh", "bolt_c_mesh"),
    )
    return "fabric_cmp", composites, {}, _scenario_hashes("fabric_per_leaf", keys)


def _nested_boundary():
    """Doc 12 §3 «вложенный композит»: the boundary sits on the composite node."""
    composites = {
        "shop_cmp": Composite("shop_cmp", (
            Node("composite", resource="rack_cmp",
                 transform=TRS(translation_cm=(0, 0, 10)),
                 appearance_seed_boundary=True),
            Node("mesh", resource="counter_mesh"),
        )),
        "rack_cmp": Composite("rack_cmp", (
            Node("mesh", resource="shelf_a_mesh"),
            Node("group", transform=TRS(translation_cm=(0, 0, 60)), children=(
                Node("mesh", resource="shelf_b_mesh"),
            )),
        )),
    }
    keys = (
        ResourceKey("composite", "rack_cmp"),
        ResourceKey("composite", "shop_cmp"),
        ResourceKey("static_mesh", "counter_mesh"),
        ResourceKey("static_mesh", "shelf_a_mesh"),
        ResourceKey("static_mesh", "shelf_b_mesh"),
    )
    return "shop_cmp", composites, {}, _scenario_hashes("nested_boundary", keys)


def _mixed_endpoints():
    """Actor leaves draw; gameobj nodes and empty options produce no leaf."""
    composites = {
        "mixed_cmp": Composite("mixed_cmp", (
            Node("mesh", resource="crate_mesh"),
            Node("actor", resource="spawner_actor",
                 transform=TRS(translation_cm=(0, 20, 0))),
            Node("gameobj", resource="dummy_pivot"),
            Node("random", options=(RandomOption("empty", weight=1),)),
            Node(
                "random",
                transform=TRS(translation_cm=(0, 0, 200)),
                appearance_seed_boundary=True,
                options=(RandomOption("mesh", resource="lamp_mesh", weight=1),),
            ),
        )),
    }
    keys = (
        ResourceKey("composite", "mixed_cmp"),
        ResourceKey("static_mesh", "crate_mesh"),
        ResourceKey("static_mesh", "lamp_mesh"),
    )
    return "mixed_cmp", composites, {}, _scenario_hashes("mixed_endpoints", keys)


_BOUNDARY_SCENARIOS = {
    "house_shared": _house_shared,
    "fabric_per_leaf": _fabric_per_leaf,
    "nested_boundary": _nested_boundary,
    "mixed_endpoints": _mixed_endpoints,
}


def boundary_scenario_names() -> tuple[str, ...]:
    return tuple(_BOUNDARY_SCENARIOS)


def boundary_scenario(name: str):
    """Return ``(root, composites, profiles, raw_hashes)`` for one scenario."""
    try:
        build = _BOUNDARY_SCENARIOS[name]
    except KeyError:
        raise KeyError(f"unknown appearance boundary scenario {name!r}") from None
    return build()


def _range_document(value_range: Range) -> list[float]:
    return [value_range.base, value_range.deviation]


def _profile_document(profile: PlacementProfile) -> dict:
    document = {}
    if profile.offset_cm is not None:
        document["offset_cm"] = [_range_document(item) for item in profile.offset_cm]
    if profile.rotation_deg is not None:
        document["rotation_deg"] = [_range_document(item) for item in profile.rotation_deg]
    if profile.uniform_scale is not None:
        document["uniform_scale"] = _range_document(profile.uniform_scale)
    if profile.vertical_scale is not None:
        document["vertical_scale"] = _range_document(profile.vertical_scale)
    return document


def _trs_document(trs: TRS) -> dict:
    return trs.signature_document()


def _option_document(option: RandomOption) -> dict:
    document = {"kind": option.kind}
    if option.resource is not None:
        document["resource"] = option.resource
    document["weight"] = option.weight
    return document


def _node_document(node: Node) -> dict:
    document = {
        "kind": node.kind,
        "trs": _trs_document(node.transform),
    }
    if node.resource is not None:
        document["resource"] = node.resource
    if node.profile is not None:
        document["profile"] = node.profile
    if node.appearance_seed_boundary:
        # Omitted at the default, exactly like the ``.composite`` grammar, so
        # the layout golden stays byte-identical to the pre-slice bytes.
        document["appearance_seed_boundary"] = True
    if node.options:
        document["options"] = [_option_document(option) for option in node.options]
    if node.children:
        document["children"] = [_node_document(child) for child in node.children]
    return document


def _fixture_document(root, composites, profiles, raw_hashes) -> dict:
    return {
        "domain": SYNTHETIC_PAYLOAD_DOMAIN,
        "root": root,
        "composites": [
            {
                "name": name,
                "nodes": [_node_document(node) for node in composites[name].nodes],
            }
            for name in sorted(composites)
        ],
        "profiles": [
            {"name": name, **_profile_document(profiles[name])}
            for name in sorted(profiles)
        ],
        "raw_hashes": [
            {"resource": str(key), "hash": raw_hashes[key]}
            for key in sorted(raw_hashes, key=str)
        ],
    }


def _stream_vector(seed: int) -> dict:
    stream = RandomStream(seed)
    initial_state = stream.initial_state
    draws = []
    for index in range(STREAM_DRAW_COUNT):
        raw_u64 = stream.next_u64()
        raw_u32 = raw_u64 >> 32
        draws.append({
            "index": index,
            "u64": raw_u64,
            "u32": raw_u32,
            "unit": raw_u32 * 2.0 ** -32,
        })
    return {
        "seed": seed,
        "seed_u32": seed & 0xFFFFFFFF,
        "initial_state": initial_state,
        "draws": draws,
    }


def _node_stream_vector(seed: int, node_path: str) -> dict:
    stream = node_random_stream(seed, node_path)
    draws = []
    for index in range(NODE_STREAM_DRAW_COUNT):
        raw_u64 = stream.next_u64()
        raw_u32 = raw_u64 >> 32
        draws.append({
            "index": index,
            "u64": raw_u64,
            "u32": raw_u32,
            "unit": raw_u32 * 2.0 ** -32,
        })
    state = placement_state(seed)
    path_hash = path_hash64(node_path)
    return {
        "seed": seed,
        "path": node_path,
        "placement_state": state,
        "path_hash64": path_hash,
        "mixed_state": state ^ path_hash,
        "initial_state": stream.initial_state,
        "draws": draws,
    }


def _decision_document(decision) -> dict:
    return {
        "path": decision.path,
        "option": decision.option,
        "weights": list(decision.weights),
        "total": decision.total,
        "raw_u32": decision.raw_u32,
        "unit": decision.unit,
        "target": decision.target,
    }


def _draw_document(draw) -> dict:
    return {
        "path": draw.path,
        "role": draw.role,
        "raw_u32": draw.raw_u32,
        "unit": draw.unit,
        "sample": draw.sample,
    }


def _leaf_document(leaf) -> dict:
    return {
        "kind": leaf.kind,
        "resource": leaf.resource,
        "world_trs": _trs_document(leaf.world_trs),
        "origin": leaf.origin,
    }


def _plan_vector(seed, root, composites, profiles, raw_hashes) -> dict:
    plan = resolve_composite(root, seed, composites, profiles, raw_hashes)
    return {
        "seed": seed,
        "decisions": [_decision_document(value) for value in plan.decisions],
        "draws": [_draw_document(value) for value in plan.draws],
        "leaves": [_leaf_document(value) for value in plan.leaves],
        "selected_dependencies": list(plan.selected_dependencies),
        "signature_preimage_utf8": plan.signature_preimage.decode("utf-8"),
        "resolved_signature": plan.resolved_signature,
    }


def golden_document() -> dict:
    root, composites, profiles, raw_hashes = synthetic_fixture()
    closure = build_source_closure(root, composites, profiles, raw_hashes)
    return {
        "schema": GOLDEN_SCHEMA,
        "stream": RANDOM_STREAM_TAG,
        "resolver": RESOLVER_TAG,
        "seed_set": list(SEEDS),
        "stream_draw_count": STREAM_DRAW_COUNT,
        "node_stream_draw_count": NODE_STREAM_DRAW_COUNT,
        "fixture": _fixture_document(root, composites, profiles, raw_hashes),
        "closure": {
            "resources": [str(key) for key in closure.resources],
            "raw_hashes": [
                {"resource": str(key), "hash": value}
                for key, value in closure.raw_hashes
            ],
            "hash_preimage_ascii": closure.hash_preimage.decode("ascii"),
            "closure_hash": closure.closure_hash,
        },
        "stream_vectors": [_stream_vector(seed) for seed in SEEDS],
        "node_stream_vectors": [
            _node_stream_vector(seed, node_path)
            for seed in SEEDS
            for node_path in NODE_STREAM_PATHS
        ],
        "plan_vectors": [
            _plan_vector(seed, root, composites, profiles, raw_hashes)
            for seed in SEEDS
        ],
    }


def golden_bytes() -> bytes:
    return (json.dumps(
        golden_document(),
        ensure_ascii=False,
        indent=2,
    ) + "\n").encode("utf-8")


def write_golden(path: Path) -> None:
    payload = golden_bytes()
    if path.exists() and path.read_bytes() == payload:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)


def _appearance_leaf_document(leaf) -> dict:
    return {
        "index": leaf.index,
        "path": leaf.path,
        "boundary": leaf.boundary,
        "raw_u32": list(leaf.raw_u32),
        "unit": list(leaf.unit),
    }


def _appearance_draw_document(draw) -> dict:
    return {
        "leaf_index": draw.leaf_index,
        "path": draw.path,
        "boundary": draw.boundary,
        "channel": draw.channel,
        "raw_u32": draw.raw_u32,
        "unit": draw.unit,
    }


def _appearance_vector(
        seed, appearance_seed, root, composites, profiles, raw_hashes) -> dict:
    resolution = resolve_placement(
        root, seed, appearance_seed, composites, profiles, raw_hashes)
    appearance = resolution.appearance
    return {
        "seed": seed,
        "appearance_seed": appearance_seed,
        "root_boundary": appearance.root_boundary,
        "boundaries": list(appearance.boundaries),
        "layout_leaves": [leaf.origin for leaf in resolution.plan.leaves],
        "leaves": [_appearance_leaf_document(leaf) for leaf in appearance.leaves],
        "draws": [_appearance_draw_document(draw) for draw in appearance.draws],
        "appearance_signature_preimage_utf8":
            appearance.signature_preimage.decode("utf-8"),
        "resolved_signature": resolution.plan.resolved_signature,
        "appearance_signature": appearance.appearance_signature,
        "placement_signature": resolution.placement_signature,
    }


def appearance_golden_document() -> dict:
    root, composites, profiles, raw_hashes = synthetic_fixture()
    scenarios = []
    for name in boundary_scenario_names():
        (scenario_root, scenario_composites,
         scenario_profiles, scenario_hashes) = boundary_scenario(name)
        scenarios.append({
            "name": name,
            "layout_seed": SCENARIO_LAYOUT_SEED,
            "fixture": _fixture_document(
                scenario_root, scenario_composites,
                scenario_profiles, scenario_hashes),
            "vectors": [
                _appearance_vector(
                    SCENARIO_LAYOUT_SEED, appearance_seed, scenario_root,
                    scenario_composites, scenario_profiles, scenario_hashes)
                for appearance_seed in APPEARANCE_SEEDS
            ],
        })
    return {
        "schema": APPEARANCE_GOLDEN_SCHEMA,
        "stage": APPEARANCE_TAG,
        "stream": RANDOM_STREAM_TAG,
        "resolver": RESOLVER_TAG,
        "channels": MH_APPEARANCE_CHANNELS,
        "seed_set": list(SEEDS),
        "appearance_seed_set": list(APPEARANCE_SEEDS),
        "rules": {
            "boundary":
                "Boundary(leaf) = nearest ancestor node, the leaf's own owning "
                "node included, whose source declared appearance_seed_boundary "
                "= true; the placement root otherwise. A random option is never "
                "a boundary: the grammar forbids the field on options.",
            "root_boundary":
                "The placement root's NodePath is the bare root composite name, "
                "the same prefix its nodes[i] paths are built from.",
            "stream":
                "AppearanceStream(leaf) = MHMakeNodeRandomStream(AppearanceSeed, "
                "Boundary(leaf)), i.e. the unchanged mh.random_stream:1 node "
                "stream. The stream is reopened per leaf, so two leaves under "
                "one boundary get identical channel values.",
            "draw_order":
                "leaf-major, channels 0..MH_APPEARANCE_CHANNELS-1 ascending, one "
                "NextU32 per channel, no gaps and no skipped leaves. Leaves are "
                "the layout plan's leaves in plan order: mesh and actor leaves "
                "both draw; gameobj nodes and empty options are not leaves and "
                "draw nothing.",
            "authority":
                "raw_u32 is the only authority and the only value in the "
                "AppearanceSignature preimage.",
            "unit":
                "unit = IEEE binary32 nearest of raw_u32 * 2^-32, serialized "
                "float32-shortest. It is derived, never authority. Note the "
                "honest edge: raw_u32 >= 0xFFFFFF80 rounds to exactly 1.0f, so "
                "the stored float32 is not strictly below 1; consumers must key "
                "on raw_u32 and must not clamp or repair the float.",
            "appearance_signature":
                "blake3-160 of the canonical JSON preimage {v, stage, "
                "appearance_seed, channels, boundaries, leaves[{path, boundary, "
                "channels:[raw_u32]}]}; boundaries are the distinct boundary "
                "NodePaths sorted ascending by their UTF-8 bytes.",
            "placement_signature":
                "blake3-160 of the ASCII bytes of ResolvedSignature "
                "concatenated with the ASCII bytes of AppearanceSignature, no "
                "separator.",
            "layout":
                "The layout arrays and ResolvedSignature are produced by the "
                "unchanged mh.random_resolver:2 stage; golden/v5/"
                "random_stream_1_vectors.json is not regenerated by this slice.",
        },
        "synthetic": {
            "fixture": _fixture_document(root, composites, profiles, raw_hashes),
            "vectors": [
                _appearance_vector(
                    seed, appearance_seed, root, composites, profiles, raw_hashes)
                for seed in SEEDS
                for appearance_seed in APPEARANCE_SEEDS
            ],
        },
        "scenarios": scenarios,
    }


def appearance_golden_bytes() -> bytes:
    # Canonical renderer, so every derived unit is spelled float32-shortest.
    return canonical_json_bytes(appearance_golden_document())


def write_appearance_golden(path: Path) -> None:
    payload = appearance_golden_bytes()
    if path.exists() and path.read_bytes() == payload:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)


if __name__ == "__main__":
    repository_root = Path(__file__).resolve().parent.parent
    destination = repository_root / "golden" / "v5" / "random_stream_1_vectors.json"
    write_golden(destination)
    print(destination)
    appearance_destination = (
        repository_root / "golden" / "v5" / "appearance" /
        "appearance_1_vectors.json")
    write_appearance_golden(appearance_destination)
    print(appearance_destination)
