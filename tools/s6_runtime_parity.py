"""Observe Python separately, then verify all four actual UE S6 report lanes.

This tool never writes a golden. The matrix observer wraps the existing
reference's TRS-composition seam: it records full parent-local products beside
the frozen result without drawing, selecting, or implementing another resolver.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
from typing import Any

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tools import mh_random_reference as reference


SEEDS = (0, 1, 2, 42, 123, 1024, 2147483647)
SCHEMA = "mh.runtime_parity_report:1"
DEFAULT_GOLDEN = Path(__file__).resolve().parent.parent / "golden/v5/random_stream_1_vectors.json"


def _read_json(path: Path) -> dict[str, Any]:
    def unique_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key {key!r} in {path}")
            result[key] = value
        return result

    return json.loads(path.read_bytes(), object_pairs_hook=unique_pairs,
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))


def _load_golden(path: Path) -> dict[str, Any]:
    raw = path.read_bytes()
    if b"\r" in raw:
        raise ValueError(f"hashed golden must be LF-clean: {path}")
    golden = _read_json(path)
    if not _valid_seed_set(golden["seed_set"]):
        raise ValueError("golden does not contain the ratified seven-seed set")
    if golden["stream"] != reference.RANDOM_STREAM_TAG or golden["resolver"] != reference.RESOLVER_TAG:
        raise ValueError("reference and frozen resolver/stream tags differ")
    return golden


def _fixture(golden):
    def node(value):
        return reference.Node(
            kind=value["kind"], resource=value.get("resource"),
            transform=reference.TRS(**value["trs"]), profile=value.get("profile"),
            options=tuple(reference.RandomOption(**option) for option in value.get("options", ())),
            children=tuple(node(child) for child in value.get("children", ())),
        )

    raw = golden["fixture"]
    composites = {
        item["name"]: reference.Composite(item["name"], tuple(node(value) for value in item["nodes"]))
        for item in raw["composites"]
    }
    profiles = {}
    for item in raw["profiles"]:
        values = {}
        for field in ("offset_cm", "rotation_deg"):
            if field in item:
                values[field] = tuple(reference.Range(*value) for value in item[field])
        for field in ("uniform_scale", "vertical_scale"):
            if field in item:
                values[field] = reference.Range(*item[field])
        profiles[item["name"]] = reference.PlacementProfile(item["name"], **values)
    hashes = {
        reference.ResourceKey(*item["resource"].split(":", 1)): item["hash"]
        for item in raw["raw_hashes"]
    }
    return raw["root"], composites, profiles, hashes


def _identity():
    return [[float(row == column) for column in range(4)] for row in range(4)]


def _local_matrix(trs):
    # Row-vector TRS, kept at full binary64 precision alongside the signature's
    # float32 WorldTrs. This is matrix observation, not TRS decomposition.
    x, y, z, w = trs.rotation_quat
    sx, sy, sz = trs.scale
    x2, y2, z2 = x + x, y + y, z + z
    xx2, yy2, zz2 = x * x2, y * y2, z * z2
    xy2, yz2, xz2 = x * y2, y * z2, x * z2
    wz2, wx2, wy2 = w * z2, w * x2, w * y2
    return [
        [(1.0 - (yy2 + zz2)) * sx, (xy2 + wz2) * sx, (xz2 - wy2) * sx, 0.0],
        [(xy2 - wz2) * sy, (1.0 - (xx2 + zz2)) * sy, (yz2 + wx2) * sy, 0.0],
        [(xz2 + wy2) * sz, (yz2 - wx2) * sz, (1.0 - (xx2 + yy2)) * sz, 0.0],
        [*trs.translation_cm, 1.0],
    ]


def _multiply(left, right):
    result = []
    for row in range(4):
        output_row = []
        for column in range(4):
            value = left[row][0] * right[0][column]
            value = left[row][1] * right[1][column] + value
            value = left[row][2] * right[2][column] + value
            value = left[row][3] * right[3][column] + value
            output_row.append(value)
        result.append(output_row)
    return result


def _observe_plan(seed, fixture):
    original = reference.compose_trs
    matrices = {id(reference.IDENTITY_TRS): _identity()}
    # Keep intermediate objects alive so an id cannot be reused while observing.
    retained = [reference.IDENTITY_TRS]

    def observe(parent, local):
        result = original(parent, local)
        matrices[id(result)] = _multiply(_local_matrix(local), matrices[id(parent)])
        retained.append(result)
        return result

    reference.compose_trs = observe
    try:
        root, composites, profiles, hashes = fixture
        plan = reference.resolve_composite(root, seed, composites, profiles, hashes)
    finally:
        reference.compose_trs = original
    return {
        "seed": seed,
        "decisions": [asdict(value) for value in plan.decisions],
        "draws": [asdict(value) for value in plan.draws],
        "leaves": [{
            "kind": leaf.kind,
            "resource": leaf.resource,
            "origin": leaf.origin,
            "world_trs": leaf.world_trs.signature_document(),
            "world_matrix": matrices[id(leaf.world_trs)],
        } for leaf in plan.leaves],
        "selected_dependencies": list(plan.selected_dependencies),
        "signature_preimage_utf8": plan.signature_preimage.decode("utf-8"),
        "resolved_signature": plan.resolved_signature,
        "closure_resources": [str(value) for value in plan.closure.resources],
        "closure_hash": plan.closure.closure_hash,
    }


def _matches_fields(actual, expected, path="root"):
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            raise ValueError(f"{path}: expected object")
        for key, value in expected.items():
            if key not in actual:
                raise ValueError(f"{path}.{key}: missing field")
            _matches_fields(actual[key], value, f"{path}.{key}")
    elif isinstance(expected, (list, tuple)):
        if not isinstance(actual, (list, tuple)) or len(actual) != len(expected):
            raise ValueError(f"{path}: array length/type differs")
        for index, (observed, wanted) in enumerate(zip(actual, expected)):
            _matches_fields(observed, wanted, f"{path}[{index}]")
    elif type(expected) in (int, float):
        # JSON has one number type, but booleans are not numbers. Python's
        # bool-is-int inheritance must not turn false into a valid option/seed 0.
        if type(actual) not in (int, float) or not math.isfinite(actual) or actual != expected:
            raise ValueError(f"{path}: {actual!r} != numeric {expected!r}")
    elif type(actual) is not type(expected) or actual != expected:
        raise ValueError(f"{path}: {actual!r} != {expected!r}")


def _valid_seed_set(value):
    return isinstance(value, list) and all(type(seed) is int for seed in value) and tuple(value) == SEEDS


def _safe_destination(saved_dir: Path):
    lexical = saved_dir.absolute()
    if lexical.name.casefold() != "saved":
        raise ValueError("--saved-dir must name the isolated host's Saved directory")
    destination = lexical / "Mimir/S6/python.json"
    for part in (destination, *destination.parents):
        if part.is_symlink() or (hasattr(part, "is_junction") and part.is_junction()):
            raise ValueError(f"report path contains a filesystem alias: {part}")
    if destination.resolve() != destination.absolute():
        raise ValueError("report physical destination differs from Saved/Mimir/S6")
    return destination


def observe(golden_path: Path, saved_dir: Path):
    golden = _load_golden(golden_path)
    fixture = _fixture(golden)
    plans = [_observe_plan(seed, fixture) for seed in SEEDS]
    # The observer must leave every frozen field exactly equal to the oracle.
    for observed, expected in zip(plans, golden["plan_vectors"]):
        _matches_fields(observed, expected, f"python.seed[{expected['seed']}]")
    report = {
        "schema": SCHEMA,
        "lane": "python",
        "stream": reference.RANDOM_STREAM_TAG,
        "resolver": reference.RESOLVER_TAG,
        "seed_set": list(SEEDS),
        "fixture_sha256": hashlib.sha256(golden_path.read_bytes()).hexdigest(),
        "plans": plans,
    }
    destination = _safe_destination(saved_dir)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination = _safe_destination(saved_dir)
    destination.write_bytes((json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8"))
    print(json.dumps({"lane": "python", "seed_set": SEEDS, "path": str(destination), "passed": len(plans)}))
    return report


def _f32(value):
    if type(value) not in (int, float):
        raise ValueError("materialization matrix entries must be numeric, not boolean or coerced values")
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _check_materialization(plan, lane):
    for leaf_index, leaf in enumerate(plan["leaves"]):
        expected = leaf["world_matrix"]
        actual = leaf["materialized_matrix"]
        if len(actual) != 4 or any(len(row) != 4 for row in actual):
            raise ValueError(f"{lane}: materialized matrix is not 4x4")
        for row in range(4):
            for column in range(4):
                source, restored = _f32(expected[row][column]), _f32(actual[row][column])
                tolerance = 8.0 * 2.0 ** -23 * max(1.0, abs(source), abs(restored))
                if not math.isfinite(source) or not math.isfinite(restored) or abs(source - restored) > tolerance:
                    raise ValueError(f"{lane}.seed[{plan['seed']}].leaf[{leaf_index}]: materialization violates section 13.5")


def verify(golden_path: Path, reports: dict[str, Path]):
    golden = _load_golden(golden_path)
    if len({path.resolve() for path in reports.values()}) != 5:
        raise ValueError("five lanes require five separate report files")
    python = _read_json(reports["python"])
    if python["fixture_sha256"] != hashlib.sha256(golden_path.read_bytes()).hexdigest():
        raise ValueError("Python lane observed different frozen fixture bytes")
    for observed, expected in zip(python["plans"], golden["plan_vectors"]):
        _matches_fields(observed, expected, f"python.seed[{expected['seed']}]")
    for lane, path in reports.items():
        report = _read_json(path)
        if report.get("schema") != SCHEMA or report.get("lane") != lane:
            raise ValueError(f"{path}: schema/lane does not identify {lane}")
        if not _valid_seed_set(report["seed_set"]) or len(report["plans"]) != len(SEEDS):
            raise ValueError(f"{lane}: seed set/count differs")
        if report["stream"] != reference.RANDOM_STREAM_TAG or report["resolver"] != reference.RESOLVER_TAG:
            raise ValueError(f"{lane}: resolver/stream tag differs")
        if lane == "pie" and report.get("world_type") != "PIE":
            raise ValueError("PIE report is not from an actual PIE world")
        if lane == "packaged" and (report.get("world_type") != "Game" or report.get("runtime_modules_only") is not True):
            raise ValueError("packaged report does not prove a runtime-only Game world")
        for observed, expected in zip(report["plans"], python["plans"]):
            # Exact comparisons include all choices/samples, frozen WorldTrs,
            # full observed matrix products, closure and signature/preimage.
            _matches_fields(observed, expected, f"{lane}.seed[{expected['seed']}]")
            if lane != "python":
                _check_materialization(observed, lane)
        print(json.dumps({"lane": lane, "seed_set": SEEDS, "passed": len(SEEDS), "path": str(path)}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--golden", type=Path, default=DEFAULT_GOLDEN)
    commands = parser.add_subparsers(dest="command", required=True)
    observation = commands.add_parser("observe", help="run the Python reference and write only Saved/Mimir/S6/python.json")
    observation.add_argument("--saved-dir", type=Path, required=True)
    comparison = commands.add_parser("verify", help="verify five independently produced lane reports")
    for lane in ("python", "automation", "editor_preview", "pie", "packaged"):
        comparison.add_argument("--" + lane.replace("_", "-"), type=Path, required=True)
    arguments = parser.parse_args()
    try:
        if arguments.command == "observe":
            observe(arguments.golden, arguments.saved_dir)
        else:
            verify(arguments.golden, {lane: getattr(arguments, lane)
                                      for lane in ("python", "automation", "editor_preview", "pie", "packaged")})
    except (ValueError, KeyError, OSError, TypeError, OverflowError) as error:
        parser.exit(1, f"S6 parity failed: {error}\n")


if __name__ == "__main__":
    main()
