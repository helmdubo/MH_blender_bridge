"""Tests for the canonical-form reference library (docs/01_bundle_schema_v1.md).

Run from the repository root:

    python -m pytest tests/ -q
"""

from __future__ import annotations

import copy
import json
import math
import random
import re
import sys
import unicodedata
from pathlib import Path
from typing import Any

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))

from gen_canonical_vectors import SAMPLE_COMPOSITE  # noqa: E402
from mh_canonical import (  # noqa: E402
    ERROR_CODES,
    P_PROPERTIES,
    P_ROTATION_QUAT,
    P_SCALE,
    P_TRANSLATION_CM,
    P_WEIGHT,
    canonical_json_bytes,
    canonicalize_quat,
    composite_canonical_form,
    composite_content_hash,
    hash_hex,
    nfc,
    quantize,
    resource_filename,
    sanitize_name,
    validate_resource_name,
)
from mh4blend.core.materials import (  # noqa: E402
    material_canonical_form,
    material_content_hash,
    material_disk_payload,
)

VECTORS_PATH = REPO_ROOT / "golden" / "canonical_vectors.json"


# ==========================================================================
# §6.1 machine error code registry
# ==========================================================================


def test_error_codes_registry_matches_the_spec_table() -> None:
    assert ERROR_CODES == frozenset(
        {
            "MH_E_DUPLICATE_NODE_UID",
            "MH_E_DUPLICATE_RESOURCE_UID",
            "MH_E_COMPOSITE_CYCLE",
            "MH_E_AMBIGUOUS_RESOURCE_OWNER",
            "MH_E_UNRESOLVED_EXTERNAL",
            "MH_E_DANGLING_PARENT",
            "MH_E_PARENT_CYCLE",
            "MH_E_MISSING_COLLECTION_UID",
            "MH_E_EMPTY_RESOURCE_COLLECTION",
            "MH_E_NESTED_COMPOSITE_COLLECTION",
            "MH_E_NAN_INF_VALUE",
            "MH_E_INVALID_SCALE",
            "MH_E_INVALID_COLLECTION_OFFSET",
            "MH_E_INVALID_COMPOSITE",
            "MH_E_UNSUPPORTED_NODE_KIND",
            "MH_E_INVALID_RESOURCE_SOURCE",
            "MH_E_INVALID_EXPORT_MANIFEST",
            "MH_E_RESOURCE_KIND_MISMATCH",
            "MH_E_RESOURCE_UID_MISMATCH",
            "MH_E_UID8_COLLISION",
            "MH_E_NON_ASCII_RESOURCE_NAME",
            "MH_E_EMPTY_MATERIAL_SLOT",
            "MH_E_INVALID_MATERIAL_VALUE",
            "MH_E_MATERIAL_SLOT_CONFLICT",
            "MH_E_TEXTURE_OUTSIDE_ROOT",
            "MH_E_UNKNOWN_SCHEMA_VERSION",
            "MH_E_FOREIGN_UID_OWNER",
            "MH_E_NAME_MISMATCH",
            "MH_E_TARGET_NAME_COLLISION",
            "MH_W_REGISTRY_INVALID",
            "MH_W_REGISTRY_STALE",
            "MH_W_COMPOSITE_CYCLE",
            "MH_W_UNRESOLVED_RESOURCE",
            "MH_W_TEXTURE_OUTSIDE_ROOT",
            "MH_W_MATERIAL_NOT_FOUND",
            "MH_W_MATERIAL_PAYLOAD_FALLBACK",
            "MH_W_MATERIAL_SLOT_NOT_FOUND",
            "MH_W_MATERIAL_SLOT_UNMAPPED",
            "MH_W_RESOURCE_FAR_FROM_ORIGIN",
            "MH_W_UNKNOWN_SHADER_CLASS",
        }
    )
    assert sum(1 for code in ERROR_CODES if code.startswith("MH_E_")) == 29
    assert sum(1 for code in ERROR_CODES if code.startswith("MH_W_")) == 11


def test_error_codes_shape() -> None:
    assert "MH_E_NON_ASCII_RESOURCE_NAME" in ERROR_CODES
    assert "MH_E_NAN_INF_VALUE" in ERROR_CODES
    pattern = re.compile(r"^MH_[EW]_[A-Z0-9_]+$")
    for code in ERROR_CODES:
        assert pattern.match(code), code


def test_quantize_nan_error_carries_its_registry_code() -> None:
    with pytest.raises(ValueError) as excinfo:
        quantize(float("nan"), 3)
    assert str(excinfo.value).startswith("MH_E_NAN_INF_VALUE")


# ==========================================================================
# §8.2 quantization
# ==========================================================================


@pytest.mark.parametrize(
    "value,p,expected",
    [
        # exact ties -> round half to even, in both directions
        (0.0005, 3, 0),  # 0.5 -> 0
        (0.0015, 3, 2),  # 1.5 -> 2
        (0.0025, 3, 2),  # 2.5 -> 2
        (0.0035, 3, 4),  # 3.5 -> 4
        (-0.0005, 3, 0),
        (-0.0015, 3, -2),
        (-0.0025, 3, -2),
        # ordinary rounding
        (-12.3456, 3, -12346),
        (0.1234565, 6, 123456),  # 123456.5 -> 123456 (even)
        # already integral after scaling
        (45.0, 3, 45000),
        (210.125, 3, 210125),
        (0.0, 6, 0),
        (-0.0, 6, 0),
        (1.0, 6, 1000000),
        # int input behaves like the same value as a float
        (2, 6, 2000000),
        # weight class
        (0.33335, 4, 3334),
        (1.0, 4, 10000),
    ],
)
def test_quantize_values(value: float, p: int, expected: int) -> None:
    assert quantize(value, p) == expected


def test_quantize_returns_int_and_never_negative_zero() -> None:
    q = quantize(-0.0000001, 3)
    assert isinstance(q, int)
    assert q == 0
    assert str(q) == "0"


def test_quantize_int_and_float_spellings_agree() -> None:
    # An on-disk `2` and an on-disk `2.0` are the same value and must quantize
    # identically - otherwise a JSON writer's choice would change the hash.
    assert quantize(2, 3) == quantize(2.0, 3)


@pytest.mark.parametrize("bad", [float("nan"), float("inf"), float("-inf")])
def test_quantize_rejects_nan_inf(bad: float) -> None:
    with pytest.raises(ValueError):
        quantize(bad, 3)


def test_quantize_rejects_bool_and_non_numbers() -> None:
    with pytest.raises(TypeError):
        quantize(True, 3)  # type: ignore[arg-type]
    with pytest.raises(TypeError):
        quantize("1.5", 3)  # type: ignore[arg-type]


# ==========================================================================
# §11 quaternion canonicalization
# ==========================================================================


@pytest.mark.parametrize(
    "q",
    [
        [0.0, 0.0, 0.0, 1.0],
        [0.0, 0.0, math.sin(math.pi / 8.0), math.cos(math.pi / 8.0)],
        [0.5, -0.5, 0.5, 0.5],
        [0.1, 0.2, 0.3, 0.9],
    ],
)
def test_canonicalize_quat_q_and_minus_q_are_identical(q: list[float]) -> None:
    assert canonicalize_quat(q) == canonicalize_quat([-c for c in q])


def test_canonicalize_quat_identity() -> None:
    assert canonicalize_quat([0.0, 0.0, 0.0, 1.0]) == (0, 0, 0, 1000000)


def test_canonicalize_quat_negates_when_w_negative() -> None:
    assert canonicalize_quat([0.5, -0.5, 0.5, -0.5]) == (-500000, 500000, -500000, 500000)


@pytest.mark.parametrize(
    "q,expected",
    [
        # w == 0 -> decided by the first nonzero of (x, y, z)
        ([-1.0, 0.0, 0.0, 0.0], (1000000, 0, 0, 0)),
        ([0.0, -1.0, 0.0, 0.0], (0, 1000000, 0, 0)),
        ([0.0, 0.0, -1.0, 0.0], (0, 0, 1000000, 0)),
        ([1.0, 0.0, 0.0, 0.0], (1000000, 0, 0, 0)),
        # first nonzero is positive -> untouched even though a later one is negative
        (
            [math.sqrt(0.5), -math.sqrt(0.5), 0.0, 0.0],
            (707107, -707107, 0, 0),
        ),
    ],
)
def test_canonicalize_quat_w_zero_sign_rule(q: list[float], expected: tuple) -> None:
    assert canonicalize_quat(q) == expected


def test_canonicalize_quat_normalizes_non_unit_input() -> None:
    assert canonicalize_quat([0.0, 0.0, 0.0, 2.0]) == (0, 0, 0, 1000000)
    assert canonicalize_quat([1.0, 1.0, 1.0, 1.0]) == (500000, 500000, 500000, 500000)
    # scaling the input does not change the canonical result
    assert canonicalize_quat([0.0, 0.0, 3.0, 3.0]) == canonicalize_quat([0.0, 0.0, 1.0, 1.0])


def test_canonicalize_quat_rejects_degenerate_and_bad_input() -> None:
    with pytest.raises(ValueError):
        canonicalize_quat([0.0, 0.0, 0.0, 0.0])
    with pytest.raises(ValueError):
        canonicalize_quat([0.0, 0.0, 0.0, float("nan")])
    with pytest.raises(ValueError):
        canonicalize_quat([0.0, 0.0, 1.0, float("inf")])
    with pytest.raises(ValueError):
        canonicalize_quat([0.0, 0.0, 1.0])


# ==========================================================================
# §8.4 canonical JSON
# ==========================================================================


def test_canonical_json_no_whitespace() -> None:
    assert canonical_json_bytes({"a": 1, "b": [2, 3]}) == b'{"a":1,"b":[2,3]}'


def test_canonical_json_keys_sorted_bytewise_utf8() -> None:
    keys = ["z", "a", "A", "é", "я", "10", "2", "", "a_", "ab"]
    data = canonical_json_bytes({k: 0 for k in keys})
    order = [k.encode("utf-8") for k in keys]
    order.sort()
    positions = [data.index(b'"' + k + b'":') for k in order]
    assert positions == sorted(positions)
    assert data.startswith(b'{"":0,"10":0,"2":0,"A":0,"a":0,')


def test_canonical_json_key_sort_is_insertion_order_independent() -> None:
    keys = ["z", "a", "A", "é", "я", "10", "2"]
    shuffled = list(keys)
    random.Random(1234).shuffle(shuffled)
    assert canonical_json_bytes({k: 1 for k in keys}) == canonical_json_bytes(
        {k: 1 for k in shuffled}
    )


def test_canonical_json_minimal_escaping() -> None:
    assert canonical_json_bytes('he said "hi" \\ ok') == b'"he said \\"hi\\" \\\\ ok"'


def test_canonical_json_control_chars_lowercase_u00xx() -> None:
    data = canonical_json_bytes("a\x00b\tc\nd\x1fe")
    assert data == b'"a\\u0000b\\u0009c\\u000ad\\u001fe"'
    # no shorthand escapes such as \n or \t (§8.4 lists only \" and \\)
    assert b"\\n" not in data
    assert b"\\t" not in data


def test_canonical_json_non_ascii_raw_utf8() -> None:
    assert canonical_json_bytes("окно") == '"окно"'.encode("utf-8")
    assert canonical_json_bytes("\U0001f9f1") == '"\U0001f9f1"'.encode("utf-8")
    # 0x7f is not a control char below 0x20 -> emitted raw
    assert canonical_json_bytes("\x7f") == b'"\x7f"'


# --- §8.4 Unicode NFC normalization ---------------------------------------

# Spelled with explicit escapes on purpose: the point of these fixtures is the
# exact code point sequence, which a raw literal could lose to a normalizing
# editor.
LATIN_NFD = "cafe\u0301"  # e + combining acute
LATIN_NFC = "caf\u00e9"  # precomposed é
CYRILLIC_NFD = "\u0447\u0430\u0438\u0306\u043d\u0438\u043a"  # и + combining breve
CYRILLIC_NFC = "\u0447\u0430\u0439\u043d\u0438\u043a"  # precomposed й
# Cyrillic 'о' + combining acute has no precomposed form: NFC leaves it alone.
NO_PRECOMPOSED = "\u043e\u043a\u043d\u043e\u0301"


def test_fixtures_really_are_nfd_and_nfc() -> None:
    assert LATIN_NFD != LATIN_NFC
    assert CYRILLIC_NFD != CYRILLIC_NFC
    assert unicodedata.normalize("NFC", LATIN_NFD) == LATIN_NFC
    assert unicodedata.normalize("NFC", CYRILLIC_NFD) == CYRILLIC_NFC
    assert unicodedata.normalize("NFC", NO_PRECOMPOSED) == NO_PRECOMPOSED


@pytest.mark.parametrize(
    "decomposed,composed",
    [(LATIN_NFD, LATIN_NFC), (CYRILLIC_NFD, CYRILLIC_NFC)],
)
def test_canonical_json_normalizes_nfd_strings(decomposed: str, composed: str) -> None:
    assert canonical_json_bytes(decomposed) == canonical_json_bytes(composed)
    assert canonical_json_bytes(decomposed) == f'"{composed}"'.encode("utf-8")
    assert hash_hex(canonical_json_bytes(decomposed)) == hash_hex(canonical_json_bytes(composed))


def test_canonical_json_normalizes_object_keys() -> None:
    assert canonical_json_bytes({LATIN_NFD: 1}) == canonical_json_bytes({LATIN_NFC: 1})
    # keys nested anywhere, not just at the top level
    assert canonical_json_bytes({"a": [{CYRILLIC_NFD: 1}]}) == canonical_json_bytes(
        {"a": [{CYRILLIC_NFC: 1}]}
    )


def test_canonical_json_key_sort_uses_nfc_bytes() -> None:
    # NFD "é" starts with 'e' (0x65) and would sort between "a" and "z";
    # its NFC form "é" is 0xc3 0xa9 and must sort after "z".
    data = canonical_json_bytes({LATIN_NFD[3:]: 1, "z": 2, "a": 3})
    assert data == b'{"a":3,"z":2,"\xc3\xa9":1}'


def test_canonical_json_keeps_marks_without_precomposed_form() -> None:
    assert canonical_json_bytes(NO_PRECOMPOSED) == f'"{NO_PRECOMPOSED}"'.encode("utf-8")


def test_canonical_json_rejects_keys_colliding_under_nfc() -> None:
    with pytest.raises(ValueError, match="NFC"):
        canonical_json_bytes({LATIN_NFD: 1, LATIN_NFC: 2})


def test_nfc_helper() -> None:
    assert nfc(LATIN_NFD) == LATIN_NFC
    assert nfc(nfc(CYRILLIC_NFD)) == CYRILLIC_NFC
    assert nfc("ascii-1") == "ascii-1"
    with pytest.raises(TypeError):
        nfc(1)  # type: ignore[arg-type]


def test_composite_hash_is_nfc_spelling_independent() -> None:
    nfd_doc = copy.deepcopy(SAMPLE_COMPOSITE)
    nfd_doc["nodes"][0]["display_name"] = CYRILLIC_NFD
    nfd_doc["nodes"][0]["properties"] = {CYRILLIC_NFD: CYRILLIC_NFD}

    nfc_doc = copy.deepcopy(SAMPLE_COMPOSITE)
    nfc_doc["nodes"][0]["display_name"] = CYRILLIC_NFC
    nfc_doc["nodes"][0]["properties"] = {CYRILLIC_NFC: CYRILLIC_NFC}

    assert composite_content_hash(nfd_doc) == composite_content_hash(nfc_doc)
    # the canonical value tree itself is normalized too, not only its bytes
    canonical = composite_canonical_form(nfd_doc)
    node = next(n for n in canonical["nodes"] if n["display_name"] == CYRILLIC_NFC)
    assert list(node["properties"]) == [CYRILLIC_NFC]


def test_canonical_json_scalars() -> None:
    assert canonical_json_bytes(None) == b"null"
    assert canonical_json_bytes(True) == b"true"
    assert canonical_json_bytes(False) == b"false"
    assert canonical_json_bytes([True, False, None]) == b"[true,false,null]"
    assert canonical_json_bytes({"f": False}) == b'{"f":false}'
    assert canonical_json_bytes(-5) == b"-5"
    assert canonical_json_bytes(0) == b"0"


def test_canonical_json_rejects_floats() -> None:
    with pytest.raises(TypeError):
        canonical_json_bytes(1.5)
    with pytest.raises(TypeError):
        canonical_json_bytes({"a": [1, 2.0]})


def test_canonical_json_rejects_non_string_keys_and_unknown_types() -> None:
    with pytest.raises(TypeError):
        canonical_json_bytes({1: "a"})
    with pytest.raises(TypeError):
        canonical_json_bytes({"a": object()})


def test_canonical_json_empty_containers() -> None:
    assert canonical_json_bytes({"a": {}, "b": [], "c": ""}) == b'{"a":{},"b":[],"c":""}'


# ==========================================================================
# §8 composite canonical form and hashing invariants
# ==========================================================================


def _shuffled_keys(value: Any, rng: random.Random) -> Any:
    """Deep copy with every object's key insertion order shuffled."""
    if isinstance(value, dict):
        items = list(value.items())
        rng.shuffle(items)
        return {k: _shuffled_keys(v, rng) for k, v in items}
    if isinstance(value, list):
        return [_shuffled_keys(v, rng) for v in value]
    return value


def test_hash_hex_format() -> None:
    h = hash_hex(b"")
    assert h.startswith("xxh3:")
    body = h.split(":", 1)[1]
    assert len(body) == 16
    assert body == body.lower()
    assert all(c in "0123456789abcdef" for c in body)


def test_node_order_does_not_change_hash() -> None:
    doc = copy.deepcopy(SAMPLE_COMPOSITE)
    reversed_doc = copy.deepcopy(SAMPLE_COMPOSITE)
    reversed_doc["nodes"] = list(reversed(reversed_doc["nodes"]))
    assert composite_content_hash(doc) == composite_content_hash(reversed_doc)
    rng = random.Random(7)
    for _ in range(5):
        shuffled = copy.deepcopy(SAMPLE_COMPOSITE)
        rng.shuffle(shuffled["nodes"])
        assert composite_content_hash(shuffled) == composite_content_hash(doc)


def test_key_order_does_not_change_hash() -> None:
    doc = copy.deepcopy(SAMPLE_COMPOSITE)
    shuffled = _shuffled_keys(copy.deepcopy(SAMPLE_COMPOSITE), random.Random(99))
    assert list(shuffled.keys()) != list(doc.keys())
    assert composite_content_hash(shuffled) == composite_content_hash(doc)


def test_pretty_vs_compact_serialization_irrelevant() -> None:
    doc = copy.deepcopy(SAMPLE_COMPOSITE)
    pretty = json.loads(json.dumps(doc, indent=2, ensure_ascii=False))
    compact = json.loads(json.dumps(doc, separators=(",", ":"), sort_keys=True, ensure_ascii=True))
    assert composite_content_hash(pretty) == composite_content_hash(compact)
    assert composite_content_hash(pretty) == composite_content_hash(doc)


# --- idempotency (§8.2): quantize -> write q/10^p -> reparse -> same hash ---

_QUANTIZED_VECTOR_FIELDS = {
    "translation_cm": P_TRANSLATION_CM,
    "rotation_quat": P_ROTATION_QUAT,
    "scale": P_SCALE,
}


def _round_trip_numbers(value: Any, p: int) -> Any:
    if isinstance(value, dict):
        return {k: _round_trip_numbers(v, p) for k, v in value.items()}
    if isinstance(value, list):
        return [_round_trip_numbers(v, p) for v in value]
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return value
    return quantize(value, p) / float(10**p)


def _write_back_as_decimals(doc: dict) -> dict:
    """Emulate the exporter: store quantized values as their `q / 10^p` decimals."""
    out = copy.deepcopy(doc)
    for node in out.get("nodes", []):
        transform = node.get("local_transform", {})
        for field, p in _QUANTIZED_VECTOR_FIELDS.items():
            if field in transform:
                transform[field] = _round_trip_numbers(transform[field], p)
        if "properties" in node:
            node["properties"] = _round_trip_numbers(node["properties"], P_PROPERTIES)
        for variant in node.get("variants", []):
            if "weight" in variant:
                variant["weight"] = _round_trip_numbers(variant["weight"], P_WEIGHT)
    return out


def test_idempotent_reexport_keeps_hash() -> None:
    doc = copy.deepcopy(SAMPLE_COMPOSITE)
    original_hash = composite_content_hash(doc)

    on_disk = _write_back_as_decimals(doc)
    for _ in range(3):
        text = json.dumps(on_disk, indent=2, ensure_ascii=False) + "\n"
        reparsed = json.loads(text)
        assert composite_content_hash(reparsed) == original_hash
        # a second export of the unchanged file must produce the same decimals
        assert _write_back_as_decimals(reparsed) == on_disk
        on_disk = _write_back_as_decimals(reparsed)


def test_rename_changes_hash() -> None:
    doc = copy.deepcopy(SAMPLE_COMPOSITE)
    before = composite_content_hash(doc)

    renamed_node = copy.deepcopy(doc)
    renamed_node["nodes"][0]["display_name"] += "_renamed"
    assert composite_content_hash(renamed_node) != before

    renamed_doc = copy.deepcopy(doc)
    renamed_doc["name"] = "window_set_b"
    assert composite_content_hash(renamed_doc) != before


def test_transform_change_changes_hash_only_above_quantization_step() -> None:
    doc = copy.deepcopy(SAMPLE_COMPOSITE)
    before = composite_content_hash(doc)

    below_step = copy.deepcopy(doc)
    below_step["nodes"][1]["local_transform"]["translation_cm"][1] += 1e-9
    assert composite_content_hash(below_step) == before

    above_step = copy.deepcopy(doc)
    above_step["nodes"][1]["local_transform"]["translation_cm"][1] += 0.01
    assert composite_content_hash(above_step) != before


def test_canonical_form_quantizes_field_classes() -> None:
    canonical = composite_canonical_form(SAMPLE_COMPOSITE)
    by_uid = {n["node_uid"]: n for n in canonical["nodes"]}
    node = by_uid["6866f569-4d42-472f-a676-a836a3df18ec"]
    assert node["local_transform"]["translation_cm"] == [0, 45000, 210000]
    assert node["local_transform"]["scale"] == [1000000, 1000000, 1000000]
    assert node["local_transform"]["rotation_quat"] == [0, 0, 0, 1000000]
    assert node["properties"]["render.offset"] == 500000
    # unknown keys in `properties` survive with their values (broken_properties)
    assert node["properties"]["vendor.unknown_flag"] is True
    assert node["properties"]["vendor.unknown_null"] is None
    assert node["properties"]["vendor.unknown_number"] == 2000000
    # plain-integer schema fields are not quantized
    assert canonical["schema_version"] == 1
    # nodes are sorted by node_uid, bytewise
    uids = [n["node_uid"] for n in canonical["nodes"]]
    assert uids == sorted(uids, key=lambda u: u.encode("utf-8"))
    # no float survives anywhere in the canonical form
    canonical_json_bytes(canonical)


def test_canonical_form_sign_canonicalizes_quaternion_defensively() -> None:
    doc = copy.deepcopy(SAMPLE_COMPOSITE)
    negated = copy.deepcopy(SAMPLE_COMPOSITE)
    quat = negated["nodes"][0]["local_transform"]["rotation_quat"]
    negated["nodes"][0]["local_transform"]["rotation_quat"] = [-c for c in quat]
    assert composite_content_hash(negated) == composite_content_hash(doc)


def test_canonical_form_rejects_non_object() -> None:
    with pytest.raises(TypeError):
        composite_canonical_form([])  # type: ignore[arg-type]


def test_canonical_form_rejects_nan_in_quantized_field() -> None:
    doc = copy.deepcopy(SAMPLE_COMPOSITE)
    doc["nodes"][0]["local_transform"]["translation_cm"][0] = float("inf")
    with pytest.raises(ValueError):
        composite_canonical_form(doc)


# ==========================================================================
# §10 file naming
# ==========================================================================


@pytest.mark.parametrize(
    "name",
    [
        "wall_a",
        "Wall A",
        "Window-Set A",
        "a-b_c 1",
        "CON",
        "com1",
        "x",
        "0",
        "  ",  # spaces only: legal input, sanitizes to '_'
        "-",
    ],
)
def test_validate_resource_name_accepts_ascii_names(name: str) -> None:
    assert validate_resource_name(name) is None


@pytest.mark.parametrize(
    "name",
    [
        "",  # empty is a violation too
        "окно",
        "стена A",
        "wall_á",
        "café",
        "wall.001",  # dot is not in [A-Za-z0-9_ -]
        "wall/a",
        "wall\ta",
        "wall\U0001f9f1",
        LATIN_NFD,
    ],
)
def test_validate_resource_name_rejects_non_ascii_and_empty(name: str) -> None:
    with pytest.raises(ValueError) as excinfo:
        validate_resource_name(name)
    assert str(excinfo.value).startswith("MH_E_NON_ASCII_RESOURCE_NAME")


def test_validate_resource_name_rejects_non_strings() -> None:
    with pytest.raises(TypeError):
        validate_resource_name(None)  # type: ignore[arg-type]


@pytest.mark.parametrize(
    "raw,expected",
    [
        ("wall_a", "wall_a"),
        ("Wall_A", "wall_a"),
        ("Window Set A", "window_set_a"),
        ("Window-Set A", "window_set_a"),
        ("a---b", "a_b"),
        ("a   b", "a_b"),
        ("a - b", "a_b"),
        ("  ", "_"),
        ("", "unnamed"),
        ("CON", "_con"),
        ("con", "_con"),
        ("Nul", "_nul"),
        ("com1", "_com1"),
        ("lpt9", "_lpt9"),
        ("com0", "com0"),  # not reserved
        ("com10", "com10"),  # not reserved
        ("con 1", "con_1"),  # reserved check runs on the sanitized string
        ("con-1", "con_1"),
    ],
)
def test_sanitize_name(raw: str, expected: str) -> None:
    assert sanitize_name(raw) == expected


def test_sanitize_name_stays_total_for_unvalidated_input() -> None:
    # `sanitize_name` is not the gate - `validate_resource_name` is - so it must
    # keep working on arbitrary strings (previews, diagnostics).
    for raw in ["Wall A", "окно 42", "a/b\\c:d*e", "\t\n", "!!!"]:
        result = sanitize_name(raw)
        assert result
        assert all(c in "abcdefghijklmnopqrstuvwxyz0123456789_" for c in result)
        assert "__" not in result


def test_resource_filename_shape() -> None:
    uid = "2db5574c-3aca-43cc-9ab5-8242403e18cd"
    assert resource_filename("wall_a", uid, ".mesh.fbx") == "wall_a__2db5574c.mesh.fbx"
    assert (
        resource_filename("Window Set A", "f53d93af-94c3-472f-98d0-ff36eb93c417", ".composite")
        == "window_set_a__f53d93af.composite"
    )
    assert resource_filename("CON", uid, ".composite") == "_con__2db5574c.composite"
    assert resource_filename("con 1", uid, ".composite") == "con_1__2db5574c.composite"


def test_resource_filename_validates_the_name_first() -> None:
    uid = "2db5574c-3aca-43cc-9ab5-8242403e18cd"
    for bad in ["", "окно", "wall.001"]:
        with pytest.raises(ValueError) as excinfo:
            resource_filename(bad, uid, ".composite")
        assert str(excinfo.value).startswith("MH_E_NON_ASCII_RESOURCE_NAME")


def test_resource_filename_validates_uid8() -> None:
    with pytest.raises(ValueError):
        resource_filename("wall_a", "2DB5574C-3aca-43cc-9ab5-8242403e18cd", ".mesh.fbx")
    with pytest.raises(ValueError):
        resource_filename("wall_a", "2db5574", ".mesh.fbx")
    with pytest.raises(ValueError):
        resource_filename("wall_a", "not-a-uid-at-all", ".mesh.fbx")


# ==========================================================================
# golden/canonical_vectors.json - the cross-implementation regression lock
# ==========================================================================


def _load_vectors() -> list[dict[str, Any]]:
    with VECTORS_PATH.open(encoding="utf-8") as handle:
        document = json.load(handle)
    assert document["schema"] == "mh.canonical_vectors"
    assert document["schema_version"] == 1
    return document["vectors"]


VECTORS = _load_vectors()


def test_vectors_file_is_well_formed() -> None:
    assert len(VECTORS) >= 15
    names = [v["name"] for v in VECTORS]
    assert len(names) == len(set(names))
    kinds = {v["kind"] for v in VECTORS}
    assert kinds == {"quantize", "quat", "json", "composite", "material"}
    raw = VECTORS_PATH.read_bytes()
    assert b"\r\n" not in raw
    assert raw.endswith(b"\n")
    # every non-ASCII character is written as a \uXXXX escape, so the exact code
    # point sequence of the NFD/NFC vectors survives any editor
    assert raw.decode("utf-8").isascii()


def test_vectors_cover_nfd_normalization() -> None:
    by_name = {v["name"]: v for v in VECTORS}
    nfd_vectors = [name for name in by_name if "nfd" in name]
    assert nfd_vectors, "the vector set must lock the NFC rule of §8.4"
    for name in nfd_vectors:
        vector = by_name[name]
        strings = (
            [vector["input"]["value"]]
            if "value" in vector["input"]
            else [pair[0] for pair in vector["input"]["pairs"]]
        )
        # at least one input string must really be decomposed, otherwise the
        # vector silently stopped testing normalization
        assert any(s != unicodedata.normalize("NFC", s) for s in strings), name

    # the NFD and NFC spellings of the same word must carry the same expectation
    assert (
        by_name["json_nfd_latin_normalized"]["expected"]
        == by_name["json_nfc_latin_reference"]["expected"]
    )
    assert (
        by_name["json_nfd_cyrillic_normalized"]["expected"]
        == by_name["json_nfc_cyrillic_reference"]["expected"]
    )


@pytest.mark.parametrize("vector", VECTORS, ids=[v["name"] for v in VECTORS])
def test_vector(vector: dict[str, Any]) -> None:
    kind = vector["kind"]
    given = vector["input"]
    expected = vector["expected"]

    if kind == "quantize":
        assert quantize(given["value"], given["p"]) == expected["q"]
        return

    if kind == "quat":
        assert list(canonicalize_quat(given["q"])) == expected["quat"]
        return

    if kind == "json":
        if "pairs" in given:
            tree: Any = {k: v for k, v in given["pairs"]}
        else:
            tree = given["value"]
        data = canonical_json_bytes(tree)
        assert data.hex() == expected["bytes_hex"]
        assert hash_hex(data) == expected["hash"]
        return

    if kind == "composite":
        canonical = composite_canonical_form(given["doc"])
        assert canonical == expected["canonical"]
        data = canonical_json_bytes(canonical)
        assert data.hex() == expected["bytes_hex"]
        assert hash_hex(data) == expected["hash"]
        assert composite_content_hash(given["doc"]) == expected["hash"]
        return

    if kind == "material":
        args = (given["shader_class"], given["params"], given["textures"])
        disk = material_disk_payload(*args)
        canonical = material_canonical_form(*args)
        assert disk == expected["disk"]
        assert canonical == expected["canonical"]
        data = canonical_json_bytes(canonical)
        assert data.hex() == expected["bytes_hex"]
        assert hash_hex(data) == expected["hash"]
        assert material_content_hash(*args) == expected["hash"]
        return

    raise AssertionError(f"unknown vector kind: {kind}")
