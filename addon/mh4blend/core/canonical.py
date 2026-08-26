"""Shared bpy-free canonical primitives for Source Protocol v4.

Design constraints (§8):

* the canonical form contains **no floats at all** - every continuous value is
  a scaled integer `q = round_half_even(value * 10^p)`, because string
  representations of floats are not portable across languages and runtimes;
* every string is normalized to Unicode NFC before serialization (§8.4), so an
  NFD spelling coming out of a macOS pipeline cannot produce a phantom diff;
* the hash is computed from the *parsed* data, never from the file bytes, so
  prettifying, key order and node order in the on-disk file are irrelevant;
* all float arithmetic here is plain IEEE-754 double arithmetic (no Decimal),
  because the C++ side will do the same double math - cross-implementation
  equality comes from performing the *same* operations, not from performing
  "more exact" ones.

Pure stdlib + `xxhash`. No Blender (`bpy`) dependency.
"""

from __future__ import annotations

import math
import re
import unicodedata
from typing import Any, Iterable, Sequence

# Optional at import time so the addon can register inside a Blender whose
# python lacks the package; any hashing call then fails with an actionable
# message instead of the addon refusing to load.
try:
    import xxhash
except ModuleNotFoundError:  # pragma: no cover - exercised only in Blender
    xxhash = None

__all__ = [
    "quantize",
    "canonicalize_quat",
    "canonical_json_bytes",
    "hash_hex",
    "nfc",
    "validate_resource_name",
    "resource_filename",
    "ERROR_CODES",
    "P_TRANSLATION_CM",
    "P_ROTATION_QUAT",
    "P_SCALE",
    "P_PROPERTIES",
]

# --------------------------------------------------------------------------
# §6.1 - registry of machine-readable validation codes
# --------------------------------------------------------------------------

# The registry is an API of the validation reports (§6.2) and of the negative
# tests; codes are stable. Additive diagnostic-only growth is recorded by a
# dated post-freeze migration note and does not change Source Schema bytes.
# `MH_E_*` blocks the operation, `MH_W_*` only warns.
ERROR_CODES = frozenset(
    {
        # Blender export and/or UE import
        "MH_E_COMPOSITE_CYCLE",
        "MH_E_COMPOSITE_GRAMMAR",
        "MH_E_COMPOSITE_LEGACY_GENERATION",
        "MH_E_AMBIGUOUS_GENERATED_ASSET",
        "MH_E_AMBIGUOUS_RESOURCE_NAME",
        "MH_E_AMBIGUOUS_RESOURCE_OWNER",
        "MH_E_UNRESOLVED_EXTERNAL",
        "MH_E_DANGLING_PARENT",
        "MH_E_FBX_TRANSPORT_FAILED",
        "MH_E_PARENT_CYCLE",
        "MH_E_PLACEMENT_PROFILE_GRAMMAR",
        # Blender export
        "MH_E_EMPTY_RESOURCE_COLLECTION",
        "MH_E_NESTED_COMPOSITE_COLLECTION",
        "MH_E_INVALID_COLLECTION_OFFSET",
        "MH_E_INVALID_LOD_HIERARCHY",
        "MH_E_PARENT_OUTSIDE_RESOURCE",
        "MH_E_LOD_LEVELS_SPARSE",
        "MH_E_LOD_SLOT_NOT_IN_BASE",
        "MH_E_DEPRECATED_LOD_ROWS",
        "MH_E_DIVERGENT_REVISIONS",
        "MH_E_EXTERNAL_MODIFICATION_CONFIRMATION_REQUIRED",
        "MH_E_PAYLOAD_LOCK_TIMEOUT",
        "MH_E_RESOURCE_NOT_FOUND",
        "MH_E_SOURCE_INDEX_INVALID",
        "MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT",
        "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
        # Composite and mesh-source import preflight
        "MH_E_INVALID_NODE_MARKERS",
        "MH_E_UNRESOLVED_COMPOSITE_REFERENCE",
        "MH_E_UNREPRESENTABLE_SCENE_OBJECT",
        "MH_E_UNREPRESENTABLE_TRANSFORM",
        "MH_E_UNSUPPORTED_NODE_KIND",
        "MH_E_INVALID_RESOURCE_SOURCE",
        "MH_E_INVALID_EXPORT_MANIFEST",
        "MH_E_RESOURCE_KIND_MISMATCH",
        "MH_E_NAN_INF_VALUE",
        "MH_E_INVALID_SCALE",
        "MH_E_NONCANONICAL_RESOURCE_NAME",
        "MH_E_IMPORT_TARGET_OCCUPIED",
        "MH_E_UNRESOLVED_MATERIAL_REFERENCE",
        # Blender export (materials/textures, D23/D27)
        "MH_E_EMPTY_MATERIAL_SLOT",
        "MH_E_INVALID_MATERIAL_VALUE",
        "MH_E_MATERIAL_GRAMMAR",
        "MH_E_MATERIAL_NOT_ROUNDTRIPPABLE",
        "MH_E_MATERIAL_SLOT_CONFLICT",
        "MH_E_NONCANONICAL_TEXTURE_REFERENCE",
        "MH_E_TEXTURE_OUTSIDE_ROOT",
        "MH_E_UNRESOLVED_TEXTURE_REFERENCE",
        # UE import
        "MH_E_UNKNOWN_SCHEMA_VERSION",
        "MH_E_NAME_MISMATCH",
        "MH_E_TARGET_NAME_COLLISION",
        # warnings
        "MH_W_REGISTRY_INVALID",
        "MH_W_REGISTRY_STALE",
        "MH_W_DUPLICATE_RESOURCE_NAME",
        "MH_W_ORPHAN_REBOUND_CONTENT_DIVERGED",
        "MH_W_PROBABLE_RESOURCE_RENAME",
        "MH_W_LOD_AUX_NODE_IGNORED",
        "MH_W_MANAGED_ASSET_LOCALLY_MODIFIED",
        "MH_W_MANAGED_STATIC_MESH_LOCALLY_MODIFIED",
        "MH_W_MATERIAL_PAYLOAD_FALLBACK",
        "MH_W_MATERIAL_SLOT_NOT_FOUND",
        "MH_W_MATERIAL_SLOT_UNMAPPED",
        "MH_W_RESOURCE_FAR_FROM_ORIGIN",
        "MH_W_PAYLOAD_EXTERNAL_MODIFIED",
        "MH_W_UNRESOLVED_PLACEMENT",
    }
)

# --------------------------------------------------------------------------
# Quantization exponents per field class (§8.2)
# --------------------------------------------------------------------------

P_TRANSLATION_CM = 3  # 0.001 cm
P_ROTATION_QUAT = 6  # 1e-6
P_SCALE = 6  # 1e-6
P_PROPERTIES = 6  # 1e-6


# --------------------------------------------------------------------------
# §8.2 - quantization
# --------------------------------------------------------------------------


def quantize(value: float, p: int) -> int:
    """Return ``round_half_even(value * 10**p)`` as an int (§8.2).

    The multiplication is performed in IEEE-754 double precision and the
    rounding is banker's rounding on the *scaled* double.
    Deliberately no `decimal.Decimal`: a "more exact" Python answer that the
    C++ side cannot reproduce would be worse than useless.

    IMPLEMENTATION NOTE for the C++ port: do **not** rely on floating-point
    environment state. `std::nearbyint` rounds half-to-even only while the
    rounding mode is FE_TONEAREST; any code in the process (a third-party
    library, a plugin, an SSE control-word change) can leave another mode in
    place and silently turn every hash in the bundle into a different value.
    Either implement fenv-independent integer banker's rounding, or set and
    restore FE_TONEAREST explicitly around the call. The cross-implementation
    vectors in `golden/canonical_vectors.json` contain exact ties in both
    directions precisely so that such a divergence fails a test instead of
    corrupting a re-import.

    Raises:
        TypeError: if `value` is a bool or not a real number.
        ValueError: if `value` is NaN or infinite (§6 export validation error),
            or if the scaled value is not finite.
    """
    if isinstance(value, bool):
        raise TypeError("bool is not a quantizable number")
    if not isinstance(value, (int, float)):
        raise TypeError(f"cannot quantize value of type {type(value).__name__}")
    if not isinstance(p, int) or isinstance(p, bool):
        raise TypeError("p must be an int")

    # float() first: an int operand would make Python use exact big-integer
    # arithmetic below, which the C++ double path could not reproduce.
    v = float(value)
    if math.isnan(v) or math.isinf(v):
        raise ValueError(
            f"MH_E_NAN_INF_VALUE: NaN/Inf is not a valid value for a quantized field: {value!r}"
        )

    scaled = v * float(10**p)
    if math.isnan(scaled) or math.isinf(scaled):
        raise ValueError(f"value overflows when scaled by 10^{p}: {value!r}")

    # round() on a float without ndigits is round-half-even and yields an int;
    # an int cannot carry a negative zero, so `-0` normalization (§8.4) is free.
    return int(round(scaled))


# --------------------------------------------------------------------------
# §11 - quaternion canonicalization
# --------------------------------------------------------------------------


def _canonicalize_quat_sign(q: Sequence[int]) -> tuple[int, int, int, int]:
    """Sign-canonicalize an already quantized quaternion (§11).

    Negate all four components if ``w < 0``, or if ``w == 0`` and the first
    nonzero component of ``(x, y, z)`` is negative. Without this, ``q`` and
    ``-q`` - the same rotation - would hash differently.
    """
    x, y, z, w = (int(c) for c in q)
    negate = False
    if w < 0:
        negate = True
    elif w == 0:
        for c in (x, y, z):
            if c != 0:
                negate = c < 0
                break
    if negate:
        return (-x, -y, -z, -w)
    return (x, y, z, w)


def canonicalize_quat(q: Iterable[float]) -> tuple[int, int, int, int]:
    """Normalize, quantize (p=6) and sign-canonicalize a quaternion (§11).

    Args:
        q: sequence of four numbers in ``(x, y, z, w)`` order.

    Returns:
        the four quantized integer components in ``(x, y, z, w)`` order.

    Raises:
        ValueError: on a wrong component count, on NaN/Inf components, or on a
            zero-length (degenerate) quaternion.
    """
    comps = [c for c in q]
    if len(comps) != 4:
        raise ValueError(f"quaternion must have 4 components, got {len(comps)}")

    values: list[float] = []
    for c in comps:
        if isinstance(c, bool) or not isinstance(c, (int, float)):
            raise TypeError("quaternion components must be real numbers")
        f = float(c)
        if math.isnan(f) or math.isinf(f):
            raise ValueError(f"NaN/Inf quaternion component: {comps!r}")
        values.append(f)

    norm = math.sqrt(sum(v * v for v in values))
    if math.isnan(norm) or math.isinf(norm) or norm == 0.0:
        raise ValueError(f"degenerate quaternion (norm={norm!r}): {comps!r}")

    quantized = [quantize(v / norm, P_ROTATION_QUAT) for v in values]
    return _canonicalize_quat_sign(quantized)


# --------------------------------------------------------------------------
# §8.4 - canonical JSON
# --------------------------------------------------------------------------

_ESCAPES = {
    0x22: b'\\"',
    0x5C: b"\\\\",
}


def nfc(text: str) -> str:
    """Normalize a string to Unicode NFC (§8.4).

    Every string entering the canonical form - values *and* object keys - goes
    through this, and the exporter applies it when writing on-disk files too.
    Without it the same visible name spelled NFD (macOS file dialogs) and NFC
    (keyboard input) would hash differently.

    Note that NFC is not "compose everything": a combining mark with no
    precomposed form (for example Cyrillic 'о' + U+0301) is transported as is.
    """
    if not isinstance(text, str):
        raise TypeError("nfc() expects a string")
    return unicodedata.normalize("NFC", text)


def _nfc_sorted_items(mapping: dict) -> list[tuple[str, Any]]:
    """Return an object's items as NFC keys sorted by their UTF-8 bytes (§8.4).

    Keys are normalized *before* sorting: normalizing afterwards would leave
    the resulting order dependent on the input spelling.
    """
    items: list[tuple[bytes, str, Any]] = []
    seen: dict[bytes, str] = {}
    for key, value in mapping.items():
        if not isinstance(key, str):
            raise TypeError(f"object keys must be strings, got {type(key).__name__}")
        normalized = nfc(key)
        key_bytes = normalized.encode("utf-8")
        # SPEC NOTE: §8.4 does not say what happens when two distinct keys of
        # one object collapse onto the same string under NFC. Silently dropping
        # one of them would lose data from the `properties` bag, so this is an
        # error: the source data has to be fixed.
        if key_bytes in seen:
            raise ValueError(f"keys collide after NFC normalization: {seen[key_bytes]!r} and {key!r}")
        seen[key_bytes] = key
        items.append((key_bytes, normalized, value))
    items.sort(key=lambda item: item[0])
    return [(key, value) for _, key, value in items]


def _encode_string(s: str, out: list[bytes]) -> None:
    s = nfc(s)
    out.append(b'"')
    chunk: list[str] = []
    for ch in s:
        code = ord(ch)
        escape = _ESCAPES.get(code)
        if escape is None and code >= 0x20:
            # Non-ASCII characters are emitted raw as UTF-8 (§8.4).
            chunk.append(ch)
            continue
        if chunk:
            out.append("".join(chunk).encode("utf-8"))
            chunk = []
        if escape is not None:
            out.append(escape)
        else:
            # Control characters below 0x20 as \u00xx, hex lowercase.
            out.append(b"\\u%04x" % code)
    if chunk:
        out.append("".join(chunk).encode("utf-8"))
    out.append(b'"')


def _encode_value(value: Any, out: list[bytes]) -> None:
    # Order matters: bool is a subclass of int, so it must be handled first.
    if value is None:
        out.append(b"null")
    elif value is True:
        out.append(b"true")
    elif value is False:
        out.append(b"false")
    elif isinstance(value, str):
        _encode_string(value, out)
    elif isinstance(value, float):
        raise TypeError(
            "float in canonical form: every number must already be a quantized "
            f"integer (§8.4), got {value!r}"
        )
    elif isinstance(value, int):
        # Decimal, no '+', no leading zeros; Python's str() already does that,
        # and an int cannot carry a negative zero.
        out.append(str(value).encode("ascii"))
    elif isinstance(value, dict):
        out.append(b"{")
        first = True
        for key, sub in _nfc_sorted_items(value):
            if not first:
                out.append(b",")
            first = False
            _encode_string(key, out)
            out.append(b":")
            _encode_value(sub, out)
        out.append(b"}")
    elif isinstance(value, (list, tuple)):
        out.append(b"[")
        first = True
        for sub in value:
            if not first:
                out.append(b",")
            first = False
            _encode_value(sub, out)
        out.append(b"]")
    else:
        raise TypeError(f"value of type {type(value).__name__} is not JSON-canonicalizable")


def canonical_json_bytes(value: Any) -> bytes:
    """Serialize an already-normalized value tree to canonical JSON (§8.4).

    Accepts dict / list / tuple / str / int / bool / None. Floats raise
    TypeError: their presence means the caller skipped quantization, which is
    an implementation bug, not a data problem.
    """
    out: list[bytes] = []
    _encode_value(value, out)
    return b"".join(out)


# --------------------------------------------------------------------------
# canonical hashing
# --------------------------------------------------------------------------


def hash_hex(data: bytes) -> str:
    """Return ``"xxh3:" + 16 lowercase hex chars`` of XXH3-64 over `data` (§8.3)."""
    if xxhash is None:
        raise RuntimeError(
            "MH: the bundled 'xxhash' runtime is unavailable. Reinstall the "
            "MH Blender Bridge Extension ZIP with Get Extensions > Install "
            "from Disk")
    return "xxh3:" + xxhash.xxh3_64(data).hexdigest()


# --------------------------------------------------------------------------
# §10 - name validation and file naming
# --------------------------------------------------------------------------

# Resource identity is the exact logical filename stem. There is no lowercase,
# sanitization, transliteration, or other writer repair.
_RESOURCE_NAME_RE = re.compile(r"^[a-z0-9_]+$")

def validate_resource_name(name: str) -> None:
    """Validate a Source Protocol v4 logical resource name.

    The name must be non-empty and consist of ``[a-z0-9_]`` only. Readers and
    writers never lowercase or otherwise repair a noncanonical name.

    Raises:
        TypeError: if `name` is not a string.
        ValueError: whose message starts with the machine code
            ``MH_E_NONCANONICAL_RESOURCE_NAME`` (§2) on any violation, the empty
            name included.
    """
    if not isinstance(name, str):
        raise TypeError("name must be a string")
    if not _RESOURCE_NAME_RE.match(name):
        raise ValueError(
            "MH_E_NONCANONICAL_RESOURCE_NAME: logical resource names must be "
            f"non-empty and match [a-z0-9_]+ exactly: {name!r}"
        )


def resource_filename(name: str, ext: str) -> str:
    """Build the v4 ``<logical_name><compound_extension>`` filename."""
    validate_resource_name(name)
    if not isinstance(ext, str) or not ext.startswith("."):
        raise ValueError("ext must be a dot-prefixed string")
    return f"{name}{ext}"
