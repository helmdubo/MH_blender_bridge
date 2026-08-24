"""Shared canonical JSON primitives for material and composite payloads.

Source Protocol v4 deliberately uses one byte renderer for both bidirectional
JSON resource kinds.  Domain codecs remain responsible for field order and
validation; this module owns UTF-8/LF rendering and IEEE binary32 spelling.
"""

from __future__ import annotations

import json
import math
import struct
from typing import Any

__all__ = [
    "CanonicalJSONDuplicateKey",
    "CanonicalJSONNonFinite",
    "CanonicalJSONSyntaxError",
    "canonical_json_bytes",
    "narrow_float32",
    "parse_json",
    "render_json",
]


class CanonicalJSONDuplicateKey(ValueError):
    def __init__(self, key: str):
        self.key = key
        super().__init__(f"duplicate JSON field {key!r}")


class CanonicalJSONNonFinite(ValueError):
    def __init__(self, token: str):
        self.token = token
        super().__init__(f"invalid non-finite JSON number {token}")


class CanonicalJSONSyntaxError(ValueError):
    pass


def narrow_float32(value: int | float) -> float:
    """Return finite binary32 ``value`` with canonical positive zero."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TypeError("value must be a number, not bool")
    try:
        if not math.isfinite(value):
            raise ValueError("value must be finite")
        narrowed = struct.unpack("!f", struct.pack("!f", float(value)))[0]
    except (OverflowError, struct.error) as exc:
        raise ValueError("value must be representable as finite IEEE float32") from exc
    if not math.isfinite(narrowed):
        raise ValueError("value must be representable as finite IEEE float32")
    return 0.0 if narrowed == 0.0 else narrowed


def _float_chars(value: float) -> str:
    value = narrow_float32(value)

    def as_float32(candidate: str) -> float:
        return struct.unpack("!f", struct.pack("!f", float(candidate)))[0]

    # IEEE binary32 needs at most 9 significant decimal digits for round-trip.
    for precision in range(1, 10):
        candidate = format(value, f".{precision}g")
        if as_float32(candidate) == value:
            return candidate
    raise AssertionError("finite float32 did not round-trip in 9 digits")


def render_json(value: Any, depth: int = 0) -> str:
    """Render insertion-ordered JSON with two-space indentation."""
    indent = "  " * depth
    child_indent = "  " * (depth + 1)
    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return _float_chars(value)
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    if isinstance(value, list):
        if not value:
            return "[]"
        rows = [
            f"{child_indent}{render_json(item, depth + 1)}"
            for item in value
        ]
        return "[\n" + ",\n".join(rows) + f"\n{indent}]"
    if isinstance(value, dict):
        if not value:
            return "{}"
        rows = [
            f"{child_indent}{json.dumps(key, ensure_ascii=False)}: "
            f"{render_json(item, depth + 1)}"
            for key, item in value.items()
        ]
        return "{\n" + ",\n".join(rows) + f"\n{indent}}}"
    raise TypeError(f"unsupported canonical JSON value {type(value).__name__}")


def canonical_json_bytes(document: Any) -> bytes:
    """Return canonical UTF-8 bytes with LF and one final LF."""
    return (render_json(document) + "\n").encode("utf-8")


def _reject_duplicate_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise CanonicalJSONDuplicateKey(key)
        result[key] = value
    return result


def parse_json(value: str | bytes) -> Any:
    """Parse JSON while retaining the closed-grammar duplicate-key signal."""
    try:
        return json.loads(
            value,
            object_pairs_hook=_reject_duplicate_pairs,
            parse_constant=lambda token: (_ for _ in ()).throw(
                CanonicalJSONNonFinite(token)),
        )
    except (CanonicalJSONDuplicateKey, CanonicalJSONNonFinite):
        raise
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        raise CanonicalJSONSyntaxError(str(exc)) from exc
