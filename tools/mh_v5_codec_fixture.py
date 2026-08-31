"""Render the shared Python/C++ Source Protocol v5 codec vectors."""

from __future__ import annotations

import json
from pathlib import Path
import sys

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from addon.mh4blend.core.composites import composite_json_bytes
from addon.mh4blend.core.model import (
    Composite,
    CompositeTransform,
    Node,
    PlacementProfile,
    PlacementRange,
    RandomOption,
)
from addon.mh4blend.core.placements import placement_json_bytes


def _canonical_text(value) -> str:
    return value.decode("utf-8")


def golden_document() -> dict:
    composites = [
        ("empty", Composite("empty")),
        ("parent_local_random_profile", Composite("root", [
            Node(
                "group",
                name="parent",
                transform=CompositeTransform(translation_cm=(100, 0, 0)),
                children=[Node(
                    "random",
                    name="choice",
                    profile="scatter_profile",
                    transform=CompositeTransform(translation_cm=(25, 0, 0)),
                    options=[
                        RandomOption("composite", 1, "variant_a_cmp"),
                        RandomOption("empty", 0),
                        RandomOption("mesh", 3, "variant_b_mesh"),
                    ],
                    children=[Node("actor", resource="marker_actor")],
                )],
            ),
        ])),
        ("float32_negative_scale", Composite("float_probe", [
            Node(
                "group",
                transform=CompositeTransform(
                    translation_cm=(-0.0, 0.1, 1.0e-7),
                    rotation_quat=(0, 0, -0.70710678, -0.70710678),
                    scale=(-0.1, 1, 1),
                ),
            ),
        ])),
        # Appended by the 2026-08-31 owner revision of OPEN-V5-15: a node may
        # carry its placement-v1 body inline instead of referencing a derived
        # external `.placement` resource.
        ("inline_placement", Composite("inline_probe", [
            Node(
                "mesh",
                resource="table_mug",
                transform=CompositeTransform(translation_cm=(50, 0, 90)),
                placement=PlacementProfile(
                    "",
                    offset_cm=(
                        PlacementRange(0, 1),
                        PlacementRange(0, 1),
                        PlacementRange(0, 0),
                    ),
                    rotation_deg=(
                        PlacementRange(0, 0),
                        PlacementRange(0, 15),
                        PlacementRange(0, 15),
                    ),
                ),
            ),
        ])),
    ]
    profiles = [
        ("empty", PlacementProfile("empty")),
        ("full", PlacementProfile(
            "full",
            offset_cm=(
                PlacementRange(0, 10),
                PlacementRange(0, 10),
                PlacementRange(0, 0),
            ),
            rotation_deg=(
                PlacementRange(0, 0),
                PlacementRange(0, 0),
                PlacementRange(0, 180),
            ),
            uniform_scale=PlacementRange(1, 0.1),
            vertical_scale=PlacementRange(1, 0.2),
        )),
    ]
    return {
        "schema": "mh.source_protocol_v5_codec_vectors:1",
        "composite_vectors": [
            {"name": name, "canonical_utf8": _canonical_text(composite_json_bytes(value))}
            for name, value in composites
        ],
        "composite_negative_vectors": [
            {
                "name": "legacy_missing_v",
                "json": '{"nodes":[]}',
                "error": "MH_E_COMPOSITE_LEGACY_GENERATION",
                "message_contains": "файл прежнего поколения: удалите и переэкспортируйте",
            },
            {"name": "wrong_version", "json": '{"v":4,"nodes":[]}', "error": "MH_E_UNKNOWN_SCHEMA_VERSION"},
            {"name": "noninteger_version", "json": '{"v":5.0,"nodes":[]}', "error": "MH_E_UNKNOWN_SCHEMA_VERSION"},
            {"name": "version_not_first", "json": '{"nodes":[],"v":5}', "error": "MH_E_COMPOSITE_GRAMMAR"},
            {"name": "duplicate_key", "json": '{"v":5,"nodes":[],"nodes":[]}', "error": "MH_E_COMPOSITE_GRAMMAR"},
            {"name": "unknown_kind", "json": '{"v":5,"nodes":[{"kind":"light","resource":"lamp"}]}', "error": "MH_E_UNSUPPORTED_NODE_KIND"},
            {"name": "random_resource", "json": '{"v":5,"nodes":[{"kind":"random","resource":"bad","options":[{"kind":"empty","weight":1}]}]}', "error": "MH_E_COMPOSITE_GRAMMAR"},
            {"name": "random_empty_options", "json": '{"v":5,"nodes":[{"kind":"random","options":[]}]}', "error": "MH_E_COMPOSITE_GRAMMAR"},
            {"name": "random_all_zero", "json": '{"v":5,"nodes":[{"kind":"random","options":[{"kind":"empty","weight":0}]}]}', "error": "MH_E_COMPOSITE_GRAMMAR"},
            {"name": "random_missing_weight", "json": '{"v":5,"nodes":[{"kind":"random","options":[{"kind":"empty"}]}]}', "error": "MH_E_COMPOSITE_GRAMMAR"},
            {"name": "random_negative_weight", "json": '{"v":5,"nodes":[{"kind":"random","options":[{"kind":"empty","weight":-1}]}]}', "error": "MH_E_COMPOSITE_GRAMMAR"},
            {"name": "option_transform", "json": '{"v":5,"nodes":[{"kind":"random","options":[{"kind":"empty","weight":1,"transform":{}}]}]}', "error": "MH_E_COMPOSITE_GRAMMAR"},
            {"name": "empty_option_resource", "json": '{"v":5,"nodes":[{"kind":"random","options":[{"kind":"empty","resource":"bad","weight":1}]}]}', "error": "MH_E_COMPOSITE_GRAMMAR"},
            {"name": "nonrandom_options", "json": '{"v":5,"nodes":[{"kind":"group","options":[{"kind":"empty","weight":1}]}]}', "error": "MH_E_COMPOSITE_GRAMMAR"},
            {"name": "zero_scale", "json": '{"v":5,"nodes":[{"kind":"group","transform":{"scale":[1,0,1]}}]}', "error": "MH_E_INVALID_SCALE"},
            {"name": "nonfinite_weight", "json": '{"v":5,"nodes":[{"kind":"random","options":[{"kind":"empty","weight":NaN}]}]}', "error": "MH_E_NAN_INF_VALUE"},
            {"name": "profile_and_placement_conflict", "json": '{"v":5,"nodes":[{"kind":"mesh","resource":"mug","profile":"scatter","placement":{"v":1,"kind":"placement_profile"}}]}', "error": "MH_E_COMPOSITE_GRAMMAR"},
            {"name": "inline_placement_negative_deviation", "json": '{"v":5,"nodes":[{"kind":"mesh","resource":"mug","placement":{"v":1,"kind":"placement_profile","offset_cm":[[0,-1],[0,0],[0,0]]}}]}', "error": "MH_E_PLACEMENT_PROFILE_GRAMMAR"},
            {"name": "inline_placement_wrong_version", "json": '{"v":5,"nodes":[{"kind":"mesh","resource":"mug","placement":{"v":2,"kind":"placement_profile"}}]}', "error": "MH_E_UNKNOWN_SCHEMA_VERSION"},
        ],
        "placement_vectors": [
            {"name": name, "canonical_utf8": _canonical_text(placement_json_bytes(value))}
            for name, value in profiles
        ],
        "placement_negative_vectors": [
            {"name": "duplicate_key", "json": '{"v":1,"kind":"placement_profile","kind":"placement_profile"}', "error": "MH_E_PLACEMENT_PROFILE_GRAMMAR"},
            {"name": "missing_v", "json": '{"kind":"placement_profile"}', "error": "MH_E_PLACEMENT_PROFILE_GRAMMAR"},
            {"name": "wrong_version", "json": '{"v":2,"kind":"placement_profile"}', "error": "MH_E_UNKNOWN_SCHEMA_VERSION"},
            {"name": "noninteger_version", "json": '{"v":1.0,"kind":"placement_profile"}', "error": "MH_E_UNKNOWN_SCHEMA_VERSION"},
            {"name": "wrong_kind", "json": '{"v":1,"kind":"text_include"}', "error": "MH_E_PLACEMENT_PROFILE_GRAMMAR"},
            {"name": "negative_deviation", "json": '{"v":1,"kind":"placement_profile","offset_cm":[[0,-1],[0,0],[0,0]]}', "error": "MH_E_PLACEMENT_PROFILE_GRAMMAR"},
            {"name": "scale_crosses_zero", "json": '{"v":1,"kind":"placement_profile","uniform_scale":[0.5,0.5]}', "error": "MH_E_PLACEMENT_PROFILE_GRAMMAR"},
            {"name": "wrong_arity", "json": '{"v":1,"kind":"placement_profile","rotation_deg":[[0,1],[0,1]]}', "error": "MH_E_PLACEMENT_PROFILE_GRAMMAR"},
            {"name": "nonfinite", "json": '{"v":1,"kind":"placement_profile","vertical_scale":[NaN,0]}', "error": "MH_E_NAN_INF_VALUE"},
        ],
        "transform_representability_vectors": [
            {
                "name": "identity",
                "matrix": [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]],
                "reconstructed": [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]],
                "representable": True,
            },
            {
                "name": "within_8_ulp",
                "matrix": [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]],
                "reconstructed": [[1.0000009536743164, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]],
                "representable": True,
            },
            {
                "name": "outside_8_ulp",
                "matrix": [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]],
                "reconstructed": [[1.000001072883606, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]],
                "representable": False,
            },
            {
                "name": "shear",
                "matrix": [[1, 0.25, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]],
                "reconstructed": [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]],
                "representable": False,
                "error": "MH_E_UNREPRESENTABLE_TRANSFORM",
            },
        ],
    }


def golden_bytes() -> bytes:
    return (json.dumps(golden_document(), ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def write_golden(path: Path) -> None:
    payload = golden_bytes()
    if path.exists() and path.read_bytes() == payload:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)


if __name__ == "__main__":
    root = Path(__file__).resolve().parent.parent
    destination = root / "golden" / "v5" / "source_protocol_v5_codec_vectors.json"
    write_golden(destination)
    print(destination)
