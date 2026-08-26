"""Pure Source Protocol v5 codec for ``.placement`` profile resources."""

from __future__ import annotations

import math
from pathlib import Path
import re
from typing import Any

from .canonical_json import (
    CanonicalJSONDuplicateKey,
    CanonicalJSONNonFinite,
    CanonicalJSONSyntaxError,
    canonical_json_bytes,
    narrow_float32,
    parse_json,
)
from .model import PlacementProfile, PlacementRange

__all__ = [
    "PlacementValueError",
    "parse_placement_profile",
    "placement_document",
    "placement_json_bytes",
    "read_placement_file",
]


_TOKEN_RE = re.compile(r"^[a-z0-9_]+$")
_ROOT_FIELDS = frozenset({
    "v", "kind", "offset_cm", "rotation_deg", "uniform_scale", "vertical_scale",
})


class PlacementValueError(ValueError):
    """A placement profile violates its closed v1 grammar."""

    def __init__(self, path: str, message: str, *, code: str | None = None):
        self.path = path
        self.code = code or "MH_E_PLACEMENT_PROFILE_GRAMMAR"
        super().__init__(f"{self.code}: {path}: {message}")


def _error(path: str, message: str) -> PlacementValueError:
    return PlacementValueError(path, message)


def _number(value: Any, path: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise _error(path, "must be a number")
    if not math.isfinite(value):
        raise PlacementValueError(
            path, "NaN/Inf is not a placement value", code="MH_E_NAN_INF_VALUE")
    try:
        return narrow_float32(value)
    except (TypeError, ValueError) as exc:
        raise _error(path, "must be representable as finite IEEE float32") from exc


def _range(value: Any, path: str, *, scale: bool) -> PlacementRange:
    if not isinstance(value, list) or len(value) != 2:
        raise _error(path, "must be exactly [base, deviation]")
    base = _number(value[0], f"{path}[0]")
    deviation = _number(value[1], f"{path}[1]")
    if deviation < 0.0:
        raise _error(f"{path}[1]", "deviation must be greater than or equal to zero")
    if scale and base - deviation <= 0.0:
        raise _error(path, "scale range base - deviation must be greater than zero")
    return PlacementRange(base, deviation)


def _triple(value: Any, path: str) -> tuple[PlacementRange, PlacementRange, PlacementRange]:
    if not isinstance(value, list) or len(value) != 3:
        raise _error(path, "must contain exactly three [base, deviation] pairs")
    return tuple(  # type: ignore[return-value]
        _range(item, f"{path}[{index}]", scale=False)
        for index, item in enumerate(value)
    )


def _parse_json(value: str | bytes) -> Any:
    try:
        return parse_json(value)
    except CanonicalJSONDuplicateKey as exc:
        raise _error(exc.key, "duplicate JSON field") from exc
    except CanonicalJSONNonFinite as exc:
        raise PlacementValueError(
            "$", f"invalid number {exc.token}", code="MH_E_NAN_INF_VALUE") from exc
    except CanonicalJSONSyntaxError as exc:
        raise _error("$", f"invalid JSON: {exc}") from exc


def parse_placement_profile(
    value: dict[str, Any] | str | bytes,
    *,
    name: str = "",
) -> PlacementProfile:
    document = _parse_json(value) if isinstance(value, (str, bytes)) else value
    if not isinstance(document, dict):
        raise _error("$", "placement payload must be an object")
    unknown = set(document) - _ROOT_FIELDS
    if unknown:
        raise _error("$", f"unknown field(s): {', '.join(sorted(unknown))}")
    if "v" not in document or "kind" not in document:
        raise _error("$", "root requires fields 'v' and 'kind'")
    version = document["v"]
    if isinstance(version, bool) or not isinstance(version, int) or version != 1:
        raise PlacementValueError(
            "$.v",
            f"placement schema version must be integer 1, got {version!r}",
            code="MH_E_UNKNOWN_SCHEMA_VERSION",
        )
    if document["kind"] != "placement_profile":
        raise _error("$.kind", "must equal 'placement_profile'")
    return PlacementProfile(
        name=name,
        offset_cm=(
            _triple(document["offset_cm"], "$.offset_cm")
            if "offset_cm" in document else None),
        rotation_deg=(
            _triple(document["rotation_deg"], "$.rotation_deg")
            if "rotation_deg" in document else None),
        uniform_scale=(
            _range(document["uniform_scale"], "$.uniform_scale", scale=True)
            if "uniform_scale" in document else None),
        vertical_scale=(
            _range(document["vertical_scale"], "$.vertical_scale", scale=True)
            if "vertical_scale" in document else None),
    )


def _range_document(value: PlacementRange, path: str, *, scale: bool) -> list[float]:
    if not isinstance(value, PlacementRange):
        raise _error(path, "must be a PlacementRange")
    validated = _range([value.base, value.deviation], path, scale=scale)
    return [validated.base, validated.deviation]


def placement_document(profile: PlacementProfile) -> dict[str, Any]:
    if not isinstance(profile, PlacementProfile):
        raise TypeError("profile must be PlacementProfile")
    document: dict[str, Any] = {"v": 1, "kind": "placement_profile"}
    if profile.offset_cm is not None:
        if len(profile.offset_cm) != 3:
            raise _error("$.offset_cm", "must contain exactly three ranges")
        document["offset_cm"] = [
            _range_document(item, f"$.offset_cm[{index}]", scale=False)
            for index, item in enumerate(profile.offset_cm)]
    if profile.rotation_deg is not None:
        if len(profile.rotation_deg) != 3:
            raise _error("$.rotation_deg", "must contain exactly three ranges")
        document["rotation_deg"] = [
            _range_document(item, f"$.rotation_deg[{index}]", scale=False)
            for index, item in enumerate(profile.rotation_deg)]
    if profile.uniform_scale is not None:
        document["uniform_scale"] = _range_document(
            profile.uniform_scale, "$.uniform_scale", scale=True)
    if profile.vertical_scale is not None:
        document["vertical_scale"] = _range_document(
            profile.vertical_scale, "$.vertical_scale", scale=True)
    return document


def placement_json_bytes(profile: PlacementProfile | dict[str, Any]) -> bytes:
    if isinstance(profile, dict):
        profile = parse_placement_profile(profile)
    return canonical_json_bytes(placement_document(profile))


def read_placement_file(path: str | Path) -> PlacementProfile:
    source = Path(path)
    if source.suffix != ".placement":
        raise PlacementValueError(
            str(source),
            "placement filename must end with exact lowercase .placement",
            code="MH_E_NONCANONICAL_RESOURCE_NAME",
        )
    if _TOKEN_RE.fullmatch(source.stem) is None:
        raise PlacementValueError(
            str(source),
            "placement filename stem must match [a-z0-9_]+ exactly",
            code="MH_E_NONCANONICAL_RESOURCE_NAME",
        )
    return parse_placement_profile(source.read_bytes(), name=source.stem)
