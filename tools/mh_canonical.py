"""Canonical form, quantization and hashing for the Mimir composite pipeline.

This module is the *reference implementation* of docs/01_bundle_schema_v1.md,
sections 6.1 (machine error codes), 8 (canonical JSON, quantization, xxh3
hashing), 10 (name validation and resource file naming) and 11 (quaternion
canonicalization).

The spec document is normative. Where this module makes a choice the spec does
not pin down, the choice is marked with a `SPEC NOTE:` comment so the future
C++ implementation in the UE plugin can mirror it exactly.

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

import xxhash

__all__ = [
    "quantize",
    "canonicalize_quat",
    "canonical_json_bytes",
    "composite_canonical_form",
    "composite_content_hash",
    "hash_hex",
    "nfc",
    "validate_resource_name",
    "sanitize_name",
    "resource_filename",
    "ERROR_CODES",
    "P_TRANSLATION_CM",
    "P_ROTATION_QUAT",
    "P_SCALE",
    "P_WEIGHT",
    "P_PROPERTIES",
    "P_DEFAULT",
]

# --------------------------------------------------------------------------
# §6.1 - registry of machine-readable validation codes
# --------------------------------------------------------------------------

# The registry is an API of the validation reports (§6.2) and of the negative
# tests; codes are stable and only grow through a schema_version note.
# `MH_E_*` blocks the operation, `MH_W_*` only warns.
ERROR_CODES = frozenset(
    {
        # Blender export and/or UE import
        "MH_E_DUPLICATE_NODE_UID",
        "MH_E_DUPLICATE_RESOURCE_UID",
        "MH_E_COMPOSITE_CYCLE",
        "MH_E_DANGLING_PARENT",
        "MH_E_PARENT_CYCLE",
        # Blender export
        "MH_E_MISSING_COLLECTION_UID",
        "MH_E_EMPTY_RESOURCE_COLLECTION",
        "MH_E_NESTED_COMPOSITE_COLLECTION",
        "MH_E_NAN_INF_VALUE",
        "MH_E_INVALID_SCALE",
        "MH_E_UID8_COLLISION",
        "MH_E_NON_ASCII_RESOURCE_NAME",
        # Blender export (materials/textures, D23/D27)
        "MH_E_TEXTURE_OUTSIDE_ROOT",
        # UE import
        "MH_E_UNKNOWN_SCHEMA_VERSION",
        "MH_E_FOREIGN_UID_OWNER",
        "MH_E_NAME_MISMATCH",
        "MH_E_TARGET_NAME_COLLISION",
        # warnings
        "MH_W_RESOURCE_FAR_FROM_ORIGIN",
    }
)

# --------------------------------------------------------------------------
# Quantization exponents per field class (§8.2)
# --------------------------------------------------------------------------

P_TRANSLATION_CM = 3  # 0.001 cm
P_ROTATION_QUAT = 6  # 1e-6
P_SCALE = 6  # 1e-6
P_WEIGHT = 4  # 1e-4
P_PROPERTIES = 6  # 1e-6

# SPEC NOTE: §8.2 lists the quantized field classes but does not name a rule
# for numbers appearing anywhere else (unknown keys, hand-edited files). We use
# the `properties` exponent as the global default so that *every* such number in
# the canonical form is a scaled integer, and so that a value written as `2` and
# the same value written as `2.0` cannot ever produce two different hashes.
P_DEFAULT = P_PROPERTIES

# SPEC NOTE: the two known schema fields that carry a plain integer rather than
# a continuous quantity - §8.2 does not list them as a quantized class, and
# quantizing them would put a nonsensical `"schema_version":1000000` into the
# canonical form and would destroy `seed_salt` as an exact hash input (§5).
# They pass through unchanged when they are ints; anything else at those keys
# is invalid data and falls back to the default quantization, so that the
# canonical form still never contains a float.
_PASSTHROUGH_INT_KEYS = frozenset({"schema_version", "seed_salt"})


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
# §8 - composite canonical form
# --------------------------------------------------------------------------


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _canon_generic(value: Any, p: int) -> Any:
    """Recursively canonicalize an unspecified subtree.

    Every number found is quantized with exponent `p`; everything else (strings,
    bools, null, unknown keys and unknown nesting) passes through untouched -
    the broken_properties principle of §3: unknown data is transported as is.
    """
    if isinstance(value, dict):
        return {k: _canon_generic(v, p) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_canon_generic(v, p) for v in value]
    if _is_number(value):
        return quantize(value, p)
    return value


def _canon_passthrough_int(value: Any) -> Any:
    """Canonicalize a known plain-integer field (see `_PASSTHROUGH_INT_KEYS`)."""
    if isinstance(value, int) and not isinstance(value, bool):
        return value
    if isinstance(value, float):
        return quantize(value, P_DEFAULT)
    return _canon_generic(value, P_DEFAULT)


def _canon_vector(value: Any, p: int, length: int | None = None) -> Any:
    """Canonicalize a fixed-length numeric vector, falling back to generic."""
    if isinstance(value, (list, tuple)) and (length is None or len(value) == length):
        if all(_is_number(c) for c in value):
            return [quantize(c, p) for c in value]
    return _canon_generic(value, p)


def _canon_rotation_quat(value: Any) -> Any:
    """Canonicalize `local_transform.rotation_quat`.

    The on-disk value is already normalized and sign-canonicalized by the
    exporter (§11), so this only quantizes - but the sign canonicalization is
    re-applied defensively on the quantized integers, because a hand-edited or
    third-party file must not be able to produce two hashes for one rotation.
    """
    if isinstance(value, (list, tuple)) and len(value) == 4 and all(_is_number(c) for c in value):
        quantized = [quantize(c, P_ROTATION_QUAT) for c in value]
        return list(_canonicalize_quat_sign(quantized))
    return _canon_generic(value, P_ROTATION_QUAT)


def _canon_local_transform(value: Any) -> Any:
    if not isinstance(value, dict):
        return _canon_generic(value, P_DEFAULT)
    out: dict[str, Any] = {}
    for key, sub in value.items():
        if key == "translation_cm":
            out[key] = _canon_vector(sub, P_TRANSLATION_CM, 3)
        elif key == "rotation_quat":
            out[key] = _canon_rotation_quat(sub)
        elif key == "scale":
            out[key] = _canon_vector(sub, P_SCALE, 3)
        else:
            # Unknown key inside a known object: default exponent.
            out[key] = _canon_generic(sub, P_DEFAULT)
    return out


def _canon_variant(value: Any) -> Any:
    if not isinstance(value, dict):
        return _canon_generic(value, P_DEFAULT)
    out: dict[str, Any] = {}
    for key, sub in value.items():
        if key == "weight":
            out[key] = _canon_generic(sub, P_WEIGHT)
        else:
            out[key] = _canon_generic(sub, P_DEFAULT)
    return out


def _canon_node(node: Any) -> Any:
    if not isinstance(node, dict):
        return _canon_generic(node, P_DEFAULT)
    out: dict[str, Any] = {}
    for key, sub in node.items():
        if key == "local_transform":
            out[key] = _canon_local_transform(sub)
        elif key == "properties":
            out[key] = _canon_generic(sub, P_PROPERTIES)
        elif key == "variants" and isinstance(sub, (list, tuple)):
            out[key] = [_canon_variant(v) for v in sub]
        elif key in _PASSTHROUGH_INT_KEYS:
            out[key] = _canon_passthrough_int(sub)
        else:
            out[key] = _canon_generic(sub, P_DEFAULT)
    return out


def _node_sort_key(node: Any) -> bytes:
    if isinstance(node, dict):
        uid = node.get("node_uid")
        if isinstance(uid, str):
            return nfc(uid).encode("utf-8")
    return b""


def _nfc_tree(value: Any) -> Any:
    """Normalize every string and every object key of a tree to NFC (§8.4).

    `canonical_json_bytes` normalizes on its own, so this only matters for
    callers that inspect or compare the canonical value tree directly (the
    golden vectors do) - it makes that tree spelling-independent too.
    """
    if isinstance(value, dict):
        return {key: _nfc_tree(sub) for key, sub in _nfc_sorted_items(value)}
    if isinstance(value, (list, tuple)):
        return [_nfc_tree(sub) for sub in value]
    if isinstance(value, str):
        return nfc(value)
    return value


def composite_canonical_form(doc: dict) -> dict:
    """Build the canonical value tree of a parsed `*.composite` document (§8).

    Input is the document exactly as parsed from disk (numbers are decimal
    ints/floats). Output is a tree of dict / list / str / int / bool / None
    with every number replaced by its scaled integer, every string normalized
    to NFC, and the `nodes` array sorted by `node_uid` (bytewise). Key order in
    the returned dicts is irrelevant: `canonical_json_bytes` sorts keys itself.
    """
    if not isinstance(doc, dict):
        raise TypeError("composite document must be a JSON object")

    out: dict[str, Any] = {}
    for key, value in doc.items():
        if key == "nodes" and isinstance(value, (list, tuple)):
            nodes = [_canon_node(n) for n in value]
            nodes.sort(key=_node_sort_key)
            out[key] = nodes
        elif key in _PASSTHROUGH_INT_KEYS:
            out[key] = _canon_passthrough_int(value)
        else:
            # Unknown / reserved top-level values: default exponent (see the
            # P_DEFAULT note above).
            out[key] = _canon_generic(value, P_DEFAULT)
    return _nfc_tree(out)


# --------------------------------------------------------------------------
# §8.3 - hashing
# --------------------------------------------------------------------------


def hash_hex(data: bytes) -> str:
    """Return ``"xxh3:" + 16 lowercase hex chars`` of XXH3-64 over `data` (§8.3)."""
    return "xxh3:" + xxhash.xxh3_64(data).hexdigest()


def composite_content_hash(doc: dict) -> str:
    """content_hash of a parsed `*.composite` document (§2, §8)."""
    return hash_hex(canonical_json_bytes(composite_canonical_form(doc)))


# --------------------------------------------------------------------------
# §10 - name validation and file naming
# --------------------------------------------------------------------------

# Resource / composite / bundle names are validated BEFORE sanitization: they
# must be plain ASCII `[A-Za-z0-9_ -]`. There is no transliteration - a
# transliteration table would be a second eternal cross-implementation
# contract, and these names become UE package names. Non-ASCII stays legal in
# node `display_name` and in `properties` values, which never reach file names.
_RESOURCE_NAME_RE = re.compile(r"^[A-Za-z0-9_ -]+$")

_ALLOWED_NAME_CHARS = frozenset("abcdefghijklmnopqrstuvwxyz0123456789_")

_WINDOWS_RESERVED = frozenset(
    ["con", "prn", "aux", "nul"]
    + [f"com{i}" for i in range(1, 10)]
    + [f"lpt{i}" for i in range(1, 10)]
)

_HEX_LOWER = frozenset("0123456789abcdef")


def validate_resource_name(name: str) -> None:
    """Validate a resource / composite / bundle name before sanitization (§10).

    The name must be non-empty and consist of ``[A-Za-z0-9_ -]`` only.

    Raises:
        TypeError: if `name` is not a string.
        ValueError: whose message starts with the machine code
            ``MH_E_NON_ASCII_RESOURCE_NAME`` (§6.1) on any violation, the empty
            name included.
    """
    if not isinstance(name, str):
        raise TypeError("name must be a string")
    if not _RESOURCE_NAME_RE.match(name):
        raise ValueError(
            "MH_E_NON_ASCII_RESOURCE_NAME: resource, composite and bundle names must be "
            f"non-empty and match [A-Za-z0-9_ -]; rename the resource: {name!r}"
        )


def sanitize_name(name: str) -> str:
    """Sanitize an already validated name for use in a file name (§10).

    lowercase -> every char outside ``[a-z0-9_]`` becomes ``_`` (no
    transliteration) -> runs of ``_`` collapse to one -> empty result becomes
    ``unnamed`` -> Windows reserved base names get a ``_`` prefix (checked on
    the sanitized string, so ``con`` -> ``_con`` but ``con 1`` -> ``con_1``).

    On a validated name (§10) the only characters step 2 ever replaces are the
    space and the dash. The function stays total for arbitrary input so that it
    can be used on unvalidated strings for previews and diagnostics -
    `validate_resource_name` is what rejects them.
    """
    if not isinstance(name, str):
        raise TypeError("name must be a string")

    lowered = name.lower()
    chars: list[str] = []
    for ch in lowered:
        replacement = ch if ch in _ALLOWED_NAME_CHARS else "_"
        if replacement == "_" and chars and chars[-1] == "_":
            continue  # collapse runs of underscores
        chars.append(replacement)

    result = "".join(chars)
    if not result:
        return "unnamed"
    if result in _WINDOWS_RESERVED:
        return "_" + result
    return result


def resource_filename(name: str, uid: str, ext: str) -> str:
    """Build ``<sanitized_name>__<uid8><ext>`` for a bundle resource (§10).

    The name is validated first (`validate_resource_name`), then sanitized.
    `uid8` is the first 8 characters of the UUID string (the first block before
    the dash), which must already be lowercase hex.
    """
    validate_resource_name(name)
    if not isinstance(uid, str):
        raise TypeError("uid must be a string")
    if len(uid) < 8:
        raise ValueError(f"uid is too short to take uid8 from: {uid!r}")
    uid8 = uid[:8]
    if not all(c in _HEX_LOWER for c in uid8):
        raise ValueError(f"uid8 must be 8 lowercase hex characters, got {uid8!r}")
    return f"{sanitize_name(name)}__{uid8}{ext}"
