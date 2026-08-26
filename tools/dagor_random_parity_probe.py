"""Reproducible Source Protocol v5 Dagor random parity probe.

The probe keeps three things distinct:

* the normative ``mh.random_stream:1`` reference;
* facts derived from one pinned public Dagor source revision;
* a real Dagor runtime observation, which must be supplied separately.

No Dagor source code is copied into this repository.  The small source-derived
model below is an independent executable description of the observable PRNG and
weighted-selection behavior named by the provenance record.
"""

from __future__ import annotations

from dataclasses import dataclass
import json
import math
from pathlib import Path
import re
import sys
from typing import Sequence

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from addon.mh4blend.core.canonical_json import narrow_float32
from tools.mh_random_fixture import SEEDS
from tools.mh_random_reference import (
    PlacementProfile,
    RandomOption,
    RandomStream,
    Range,
    raw_payload_hash,
    sample_placement_profile,
    select_weighted,
)


DAGOR_SOURCE_REPOSITORY = "https://github.com/GaijinEntertainment/DagorEngine"
DAGOR_SOURCE_COMMIT = "75723669297e48e200a0dc67b18c1629e0975daf"
DAGOR_PRNG_SOURCE = "prog/gameLibs/publicInclude/gameMath/objgenPrng.h"
DAGOR_COMPOSITE_SOURCE = (
    "prog/tools/sceneTools/daEditorX/services/compositMgr/"
    "compositMgrService.cpp"
)
PROBE_SCHEMA = "mh.dagor_random_parity_probe:1"
GAZ_SCHEMA = "mh.gaz53_random_baseline:1"

_UINT32_MASK = (1 << 32) - 1
_DAGOR_LCG_MUL = 0x41C64E6D
_DAGOR_LCG_ADD = 0x3039
_NUMBER = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"
_ENT_BLOCK = re.compile(r"\bent\s*\{", re.IGNORECASE)
_NAME = re.compile(r'\bname\s*:\s*t\s*=\s*"([^"]*)"', re.IGNORECASE)
_WEIGHT = re.compile(rf"\bweight\s*:\s*r\s*=\s*({_NUMBER})", re.IGNORECASE)
_P2 = re.compile(
    rf"\b(?P<name>offset_[xyz]|rot_[xyz]|scale|yScale)\s*:\s*p2\s*=\s*"
    rf"\[?\s*(?P<base>{_NUMBER})\s*,\s*(?P<deviation>{_NUMBER})\s*\]?",
    re.IGNORECASE,
)


class DagorProbeError(ValueError):
    """Fail-closed malformed probe/reference fixture."""


def _f32(value: int | float) -> float:
    return narrow_float32(value)


@dataclass(frozen=True)
class DagorOption:
    kind: str
    resource: str | None
    weight: float


@dataclass(frozen=True)
class DagorFixture:
    options: tuple[DagorOption, ...]
    ranges: tuple[tuple[str, Range], ...]


class DagorSourceStream:
    """Independent model of the pinned ``objgenerator`` 15-bit stream."""

    def __init__(self, seed: int):
        if isinstance(seed, bool) or not isinstance(seed, int):
            raise DagorProbeError("Dagor source seed must be an int32")
        if seed < -(1 << 31) or seed > (1 << 31) - 1:
            raise DagorProbeError("Dagor source seed must be an int32")
        self._state = seed & _UINT32_MASK

    def next_raw15(self) -> int:
        self._state = (
            self._state * _DAGOR_LCG_MUL + _DAGOR_LCG_ADD
        ) & _UINT32_MASK
        return (self._state >> 16) & 0x7FFF

    def next_unit(self) -> tuple[int, float]:
        raw = self.next_raw15()
        return raw, _f32(raw / 32768.0)


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//.*?$", "", text, flags=re.MULTILINE)


def _balanced_block(text: str, open_brace: int) -> str:
    depth = 0
    in_string = False
    escaped = False
    for index in range(open_brace, len(text)):
        character = text[index]
        if in_string:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                in_string = False
            continue
        if character == '"':
            in_string = True
        elif character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace + 1:index]
    raise DagorProbeError("unterminated ent block")


def _resource_identity(token: str) -> tuple[str, str | None]:
    if not token:
        return "empty", None
    if ":" not in token:
        raise DagorProbeError(f"Dagor option lacks an explicit asset type: {token!r}")
    resource, suffix = token.rsplit(":", 1)
    kind_by_suffix = {
        "composit": "composite",
        "rendinst": "mesh",
        "gameobj": "actor",
    }
    kind = kind_by_suffix.get(suffix.lower())
    if kind is None or not resource:
        raise DagorProbeError(f"unsupported Dagor option token: {token!r}")
    return kind, resource


def parse_dagor_random_fixture(payload: bytes) -> DagorFixture:
    try:
        text = payload.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise DagorProbeError("Dagor fixture must be UTF-8") from exc
    text = _strip_comments(text)
    options: list[DagorOption] = []
    for match in _ENT_BLOCK.finditer(text):
        block = _balanced_block(text, text.find("{", match.start()))
        name_match = _NAME.search(block)
        token = name_match.group(1) if name_match else ""
        weight_match = _WEIGHT.search(block)
        weight = _f32(float(weight_match.group(1))) if weight_match else 1.0
        if not math.isfinite(weight) or weight < 0.0:
            raise DagorProbeError("Dagor option weight must be finite and non-negative")
        kind, resource = _resource_identity(token)
        options.append(DagorOption(kind, resource, weight))
    if not options or not any(option.weight > 0.0 for option in options):
        raise DagorProbeError("Dagor random fixture requires a positive option")

    ranges: list[tuple[str, Range]] = []
    seen_ranges: set[str] = set()
    for match in _P2.finditer(text):
        name = match.group("name")
        canonical_name = "vertical_scale" if name.lower() == "yscale" else name.lower()
        if canonical_name in seen_ranges:
            raise DagorProbeError(f"duplicate Dagor range {canonical_name}")
        seen_ranges.add(canonical_name)
        ranges.append((
            canonical_name,
            Range(float(match.group("base")), float(match.group("deviation"))),
        ))
    return DagorFixture(tuple(options), tuple(ranges))


def parse_dagor_references(payload: bytes) -> tuple[tuple[str, str], ...]:
    """Return ordered unique typed references declared anywhere in one BLK."""
    try:
        text = _strip_comments(payload.decode("utf-8-sig"))
    except UnicodeDecodeError as exc:
        raise DagorProbeError("Dagor fixture must be UTF-8") from exc
    references: list[tuple[str, str]] = []
    seen: set[tuple[str, str]] = set()
    for match in _NAME.finditer(text):
        kind, resource = _resource_identity(match.group(1))
        if kind == "empty" or resource is None:
            continue
        reference = (kind, resource)
        if reference not in seen:
            seen.add(reference)
            references.append(reference)
    return tuple(references)


def _dagor_weighted_selection(
    stream: DagorSourceStream,
    options: Sequence[DagorOption],
) -> dict:
    total = 0.0
    for option in options:
        total = _f32(total + option.weight)
    if total <= 0.0 or not math.isfinite(total):
        raise DagorProbeError("Dagor source-derived total weight is invalid")
    normalized = tuple(_f32(option.weight / total) for option in options)
    raw, unit = stream.next_unit()
    remainder = unit
    for index, weight in enumerate(normalized):
        remainder = _f32(remainder - weight)
        if remainder <= 0.0:
            return {
                "raw15": raw,
                "unit_f32": unit,
                "normalized_weights_f32": list(normalized),
                "option": index,
            }
    return {
        "raw15": raw,
        "unit_f32": unit,
        "normalized_weights_f32": list(normalized),
        "option": len(options) - 1,
    }


def _dagor_sample(stream: DagorSourceStream, value_range: Range) -> dict:
    raw, unit = stream.next_unit()
    centered = _f32(_f32(unit * 2.0) - 1.0)
    sample = _f32(value_range.base + _f32(centered * value_range.deviation))
    return {"raw15": raw, "unit_f32": unit, "sample_f32": sample}


def _source_transform_candidates(fixture: DagorFixture, seed: int) -> dict:
    ranges = dict(fixture.ranges)
    axes = (
        ("rotation_x", ranges["rot_x"]),
        ("rotation_y", ranges["rot_y"]),
        ("rotation_z", ranges["rot_z"]),
        ("offset_x", ranges["offset_x"]),
        ("offset_y", ranges["offset_y"]),
        ("offset_z", ranges["offset_z"]),
    )

    def candidate(axis_order: tuple[tuple[str, Range], ...]) -> list[dict]:
        stream = DagorSourceStream(seed)
        result = [
            {"role": role, **_dagor_sample(stream, value_range)}
            for role, value_range in axis_order
        ]
        result.append({"role": "uniform_scale", **_dagor_sample(stream, ranges["scale"])})
        result.append({"role": "dagor_y_scale", **_dagor_sample(stream, ranges["vertical_scale"])})
        return result

    return {
        "binding_status": "runtime_observation_required",
        "candidate_scope": "illustrative_non_exhaustive",
        "reason": (
            "The pinned C++ source mutates one seed from function-call arguments "
            "whose evaluation order is not a portable axis-order contract."
        ),
        "left_to_right_candidate": candidate(axes),
        "right_to_left_axes_candidate": candidate((
            axes[2], axes[1], axes[0], axes[5], axes[4], axes[3],
        )),
    }


def _mh_profile(fixture: DagorFixture) -> PlacementProfile:
    ranges = dict(fixture.ranges)
    return PlacementProfile(
        "dagor_parity_probe_profile",
        offset_cm=(ranges["offset_x"], ranges["offset_y"], ranges["offset_z"]),
        rotation_deg=(ranges["rot_x"], ranges["rot_y"], ranges["rot_z"]),
        uniform_scale=ranges["scale"],
        vertical_scale=ranges["vertical_scale"],
    )


def _mh_vector(fixture: DagorFixture, seed: int) -> dict:
    options = tuple(
        RandomOption(option.kind, option.weight, option.resource)
        for option in fixture.options
    )
    stream = RandomStream(seed)
    decision = select_weighted(stream, "probe_cmp:nodes[0]", options)
    sample, trace = sample_placement_profile(
        stream,
        "probe_cmp:nodes[0]",
        _mh_profile(fixture),
    )
    return {
        "selection": {
            "raw_u32": decision.raw_u32,
            "unit_f64": decision.unit,
            "target_f64": decision.target,
            "option": decision.option,
        },
        "profile": {
            "offset_cm": list(sample.offset_cm),
            "rotation_deg": list(sample.rotation_deg),
            "uniform_scale": sample.uniform_scale,
            "vertical_scale": sample.vertical_scale,
        },
        "draws": [
            {
                "role": entry.role,
                "raw_u32": entry.raw_u32,
                "unit_f64": entry.unit,
                "sample_f32": entry.sample,
            }
            for entry in trace
        ],
    }


def parity_document(fixture_path: Path) -> dict:
    payload = fixture_path.read_bytes()
    fixture = parse_dagor_random_fixture(payload)
    vectors = []
    mismatches = []
    for seed in SEEDS:
        dagor_selection = _dagor_weighted_selection(
            DagorSourceStream(seed), fixture.options)
        mh = _mh_vector(fixture, seed)
        if dagor_selection["option"] != mh["selection"]["option"]:
            mismatches.append(seed)
        vectors.append({
            "seed": seed,
            "mh_random_stream_1": mh,
            "dagor_pinned_source": {
                "selection": dagor_selection,
                "transform_candidates": _source_transform_candidates(fixture, seed),
            },
        })
    return {
        "schema": PROBE_SCHEMA,
        "status": {
            "mh_normative": "frozen",
            "dagor_evidence": "pinned_public_source_derived",
            "dagor_runtime_observation": "not_run",
            "owner_a_or_b_decision": "pending",
        },
        "provenance": {
            "repository": DAGOR_SOURCE_REPOSITORY,
            "commit": DAGOR_SOURCE_COMMIT,
            "prng_source": DAGOR_PRNG_SOURCE,
            "prng_symbol": "objgenerator::rnd/frnd",
            "composite_source": DAGOR_COMPOSITE_SOURCE,
            "selection_symbol": "CompositEntityPool::Component::selectEnt",
            "transform_symbol": "CompositEntityPool::getTm/getRandom",
            "fixture": _repository_relative(fixture_path),
            "fixture_raw_hash": raw_payload_hash(payload),
            "regeneration_command": "python tools/dagor_random_parity_probe.py",
        },
        "fixture": {
            "options": [
                {
                    "kind": option.kind,
                    "resource": option.resource,
                    "weight": option.weight,
                }
                for option in fixture.options
            ],
            "ranges": {
                name: [value_range.base, value_range.deviation]
                for name, value_range in fixture.ranges
            },
        },
        "seed_set": list(SEEDS),
        "vectors": vectors,
        "comparison": {
            "selection_mismatch_seeds": mismatches,
            "selection_equal_for_all_seeds": not mismatches,
            "stream_bytes_equal": False,
            "stream_bytes_reason": (
                "Pinned Dagor source exposes a 32-bit LCG and 15-bit sample; "
                "mh.random_stream:1 is the owner-frozen uint64 splitmix stream."
            ),
            "transform_axis_binding": "requires_real_runtime_observation",
        },
    }


def gaz_baseline_document(source_directory: Path) -> dict:
    source_paths = tuple(sorted(source_directory.glob("*.composit.blk")))
    expected_names = {
        "gaz53_b_random_cmp.composit.blk",
        "gaz53_b_body_cmp.composit.blk",
        "gaz53_body_bc_random_cmp.composit.blk",
    }
    if {path.name for path in source_paths} != expected_names:
        raise DagorProbeError("GAZ oracle must contain exactly the three owner files")
    random_path = source_directory / "gaz53_body_bc_random_cmp.composit.blk"
    payload = random_path.read_bytes()
    fixture = parse_dagor_random_fixture(payload)
    source_records = []
    closure_by_kind: dict[str, set[str]] = {
        "composite": {
            path.name.removesuffix(".composit.blk")
            for path in source_paths
        },
        "static_mesh": set(),
        "actor": set(),
    }
    for path in source_paths:
        source_payload = path.read_bytes()
        references = parse_dagor_references(source_payload)
        for kind, resource in references:
            resource_kind = "static_mesh" if kind == "mesh" else kind
            closure_by_kind[resource_kind].add(resource)
        source_records.append({
            "path": _repository_relative(path),
            "raw_hash": raw_payload_hash(source_payload),
            "declared_references": [
                {"kind": kind, "resource": resource}
                for kind, resource in references
            ],
        })
    vectors = []
    options = tuple(
        RandomOption(option.kind, option.weight, option.resource)
        for option in fixture.options
    )
    for seed in SEEDS:
        decision = select_weighted(
            RandomStream(seed),
            "gaz53_body_bc_random_cmp:nodes[0]",
            options,
        )
        selected = fixture.options[decision.option]
        vectors.append({
            "seed": seed,
            "raw_u32": decision.raw_u32,
            "unit_f64": decision.unit,
            "target_f64": decision.target,
            "option": decision.option,
            "resource": selected.resource,
        })
    return {
        "schema": GAZ_SCHEMA,
        "status": {
            "source_oracle": "bound",
            "mh_normative_choices": "frozen",
            "dagor_runtime_observation": "not_run",
            "resolved_signatures": "blocked_on_option_payload_closure",
        },
        "sources": source_records,
        "source_declared_closure": {
            kind: sorted(resources)
            for kind, resources in closure_by_kind.items()
            if resources
        },
        "options": [
            {
                "kind": option.kind,
                "resource": option.resource,
                "weight": option.weight,
            }
            for option in fixture.options
        ],
        "seed_set": list(SEEDS),
        "mh_random_stream_1_choices": vectors,
    }


def _json_bytes(document: dict) -> bytes:
    return (json.dumps(document, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def _repository_relative(path: Path) -> str:
    for anchor in ("golden", "reference"):
        if anchor in path.parts:
            return Path(*path.parts[path.parts.index(anchor):]).as_posix()
    raise DagorProbeError(f"path is outside a known probe input tree: {path}")


def generated_outputs(repository_root: Path) -> tuple[tuple[Path, bytes], ...]:
    probe_fixture = (
        repository_root / "golden" / "v5" / "dagor_random_probe" /
        "random_parity_probe.composit.blk"
    )
    gaz_source_directory = (
        repository_root / "reference" / "dagor_fixtures" / "gaz53"
    )
    return (
        (
            repository_root / "golden" / "v5" / "dagor_random_probe" /
            "source_derived_vectors.json",
            _json_bytes(parity_document(probe_fixture)),
        ),
        (
            repository_root / "golden" / "v5" / "gaz53" /
            "baseline_rng_choices.json",
            _json_bytes(gaz_baseline_document(gaz_source_directory)),
        ),
    )


def write_outputs(repository_root: Path) -> None:
    for path, payload in generated_outputs(repository_root):
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists() or path.read_bytes() != payload:
            path.write_bytes(payload)


if __name__ == "__main__":
    root = Path(__file__).resolve().parent.parent
    write_outputs(root)
    for output_path, _ in generated_outputs(root):
        print(output_path)
