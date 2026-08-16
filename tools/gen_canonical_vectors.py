#!/usr/bin/env python3
"""Generate `golden/canonical_vectors.json` - the cross-implementation contract.

The file produced here is the test-vector set referenced by §8.3 of
docs/01_bundle_schema_v1.md: every implementation of the canonical form (this
Python reference library and the future C++ one inside the UE plugin) must
reproduce every vector byte for byte.

Run from anywhere:

    python3 tools/gen_canonical_vectors.py

Output is pretty-printed (indent 2, sorted keys, LF, trailing newline) so that
it diffs well in git; the *contents* of the vectors are what matters, the
formatting is only for humans.

Vector shape:

    {"name": str, "kind": "quantize"|"quat"|"json"|"composite",
     "input": <kind-specific>, "expected": <kind-specific>}

  kind "quantize"  input {"value": <number>, "p": <int>}
                   expected {"q": <int>}
  kind "quat"      input {"q": [x, y, z, w]}                    (raw, may be
                   unnormalized / non-canonical sign)
                   expected {"quat": [x, y, z, w]}              (scaled ints)
  kind "json"      input {"value": <tree>} or {"pairs": [[k, v], ...]}
                   ("pairs" builds an object in the given order, so that key
                   sorting is actually exercised - a JSON object literal would
                   already come out of the parser in some other order)
                   expected {"bytes_hex": <hex>, "hash": "xxh3:..."}
  kind "composite" input {"doc": <parsed *.composite document>}
                   expected {"canonical": <canonical value tree>,
                             "bytes_hex": <hex>, "hash": "xxh3:..."}
"""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))

from mh_canonical import (  # noqa: E402
    canonical_json_bytes,
    canonicalize_quat,
    composite_canonical_form,
    hash_hex,
    quantize,
)

OUTPUT_PATH = REPO_ROOT / "golden" / "canonical_vectors.json"


# --------------------------------------------------------------------------
# vector builders
# --------------------------------------------------------------------------


def quantize_vector(name: str, value: float, p: int) -> dict[str, Any]:
    return {
        "name": name,
        "kind": "quantize",
        "input": {"value": value, "p": p},
        "expected": {"q": quantize(value, p)},
    }


def quat_vector(name: str, q: list[float]) -> dict[str, Any]:
    return {
        "name": name,
        "kind": "quat",
        "input": {"q": list(q)},
        "expected": {"quat": list(canonicalize_quat(q))},
    }


def json_vector(name: str, value: Any = None, pairs: list[list[Any]] | None = None) -> dict[str, Any]:
    if pairs is not None:
        tree: Any = {k: v for k, v in pairs}
        vector_input: dict[str, Any] = {"pairs": [list(pair) for pair in pairs]}
    else:
        tree = value
        vector_input = {"value": value}
    data = canonical_json_bytes(tree)
    return {
        "name": name,
        "kind": "json",
        "input": vector_input,
        "expected": {"bytes_hex": data.hex(), "hash": hash_hex(data)},
    }


def composite_vector(name: str, doc: dict) -> dict[str, Any]:
    canonical = composite_canonical_form(doc)
    data = canonical_json_bytes(canonical)
    return {
        "name": name,
        "kind": "composite",
        "input": {"doc": doc},
        "expected": {
            "canonical": canonical,
            "bytes_hex": data.hex(),
            "hash": hash_hex(data),
        },
    }


# --------------------------------------------------------------------------
# the composite sample document
# --------------------------------------------------------------------------

# A small but complete document: three nodes (one of them a child, one a
# composite_ref), deliberately stored out of `node_uid` order so that the
# sorting rule of §8.4 is exercised, plus a `properties` bag carrying a number,
# a string, a bool, an explicit null and an unknown key (broken_properties).
SAMPLE_COMPOSITE: dict[str, Any] = {
    "schema": "mh.composite",
    "schema_version": 1,
    "uid": "f53d93af-94c3-472f-98d0-ff36eb93c417",
    "name": "window_set_a",
    "nodes": [
        {
            "node_uid": "a0ccf18c-2a1e-4c0c-9a2f-9d0d31a54e10",
            "parent_uid": None,
            "kind": "composite_ref",
            "display_name": "lamp",
            "resource_uid": "e3ba6783-1f6b-4f6e-9a4e-0f2f7a2f9b31",
            "local_transform": {
                "translation_cm": [-12.345, 0.0, 210.125],
                "rotation_quat": [0.0, 0.0, 0.382683, 0.92388],
                "scale": [1.0, 1.0, 1.0],
            },
            "properties": {},
        },
        {
            "node_uid": "6866f569-4d42-472f-a676-a836a3df18ec",
            "parent_uid": None,
            "kind": "mesh",
            "display_name": "window_a.001",
            "resource_uid": "5839a2e1-9b0a-4a83-9f9a-1e2d3c4b5a60",
            "local_transform": {
                "translation_cm": [0.0, 45.0, 210.0],
                "rotation_quat": [0.0, 0.0, 0.0, 1.0],
                "scale": [1.0, 1.0, 1.0],
            },
            "properties": {
                "role": "decal",
                "render.offset": 0.5,
                "vendor.unknown_flag": True,
                "vendor.unknown_null": None,
                "vendor.unknown_number": 2,
            },
        },
        {
            "node_uid": "b1d2c3e4-5f60-4718-8293-a4b5c6d7e8f9",
            "parent_uid": "6866f569-4d42-472f-a676-a836a3df18ec",
            "kind": "group",
            # deliberately spelled NFD (и + U+0306) - the canonical form must
            # normalize it to NFC (§8.4); non-ASCII is legal in `display_name`,
            # only resource/composite/bundle names are ASCII-restricted (§10)
            "display_name": "\u0447\u0430\u0438\u0306\u043d\u0438\u043a \"A\"",
            "resource_uid": None,
            "local_transform": {
                "translation_cm": [0.0005, -0.0015, 1.5],
                "rotation_quat": [0.0, 0.0, -1.0, 0.0],
                "scale": [0.5, 2.0, 1.000001],
            },
            "properties": {"role": "group"},
        },
    ],
}


# A document exercising the reserved fields of §3: `variants[*].weight` (p=4),
# the plain-integer `seed_salt`, and an unknown top-level key whose numbers fall
# back to the default exponent.
SAMPLE_RESERVED: dict[str, Any] = {
    "schema": "mh.composite",
    "schema_version": 1,
    "uid": "0a1b2c3d-4e5f-4a6b-8c9d-0e1f2a3b4c5d",
    "name": "variant_set_a",
    "vendor.unknown_top_level": {"scale_hint": 1.5, "count": 3},
    "nodes": [
        {
            "node_uid": "c0ffee00-1111-4222-8333-444455556666",
            "parent_uid": None,
            "kind": "variant_set",
            "display_name": "bush_variants",
            "variants": [
                {"resource_uid": "11111111-1111-4111-8111-111111111111", "weight": 1.0},
                {"resource_uid": "22222222-2222-4222-8222-222222222222", "weight": 0.33335},
            ],
            "seed_policy": "inherit",
            "seed_salt": 7,
            "local_transform": {
                "translation_cm": [1.0005, 2.0015, -3.5],
                "rotation_quat": [0.0, 0.0, 0.0, 1.0],
                "scale": [1.0, 1.0, 1.0],
            },
            "properties": {},
        }
    ],
}


def build_vectors() -> list[dict[str, Any]]:
    vectors: list[dict[str, Any]] = []

    # --- §8.2 quantization -------------------------------------------------
    # Half-even in both directions: 0.0005*1e3 == 0.5 exactly -> 0 (even),
    # 0.0015*1e3 == 1.5 exactly -> 2 (even).
    vectors.append(quantize_vector("quantize_half_even_down", 0.0005, 3))
    vectors.append(quantize_vector("quantize_half_even_up", 0.0015, 3))
    vectors.append(quantize_vector("quantize_half_even_2_5", 0.0025, 3))
    vectors.append(quantize_vector("quantize_negative_half_even_down", -0.0005, 3))
    vectors.append(quantize_vector("quantize_negative_half_even_up", -0.0015, 3))
    vectors.append(quantize_vector("quantize_negative_value", -12.3456, 3))
    vectors.append(quantize_vector("quantize_already_integral", 45.0, 3))
    vectors.append(quantize_vector("quantize_negative_zero", -0.0, 6))
    vectors.append(quantize_vector("quantize_int_input", 2, 6))
    vectors.append(quantize_vector("quantize_translation_typical", 210.125, 3))
    vectors.append(quantize_vector("quantize_p6_tie", 0.1234565, 6))
    vectors.append(quantize_vector("quantize_weight_p4", 0.33335, 4))

    # --- §11 quaternion ----------------------------------------------------
    vectors.append(quat_vector("quat_identity", [0.0, 0.0, 0.0, 1.0]))
    # q and -q are the same rotation and must produce identical output.
    yaw45 = [0.0, 0.0, math.sin(math.pi / 8.0), math.cos(math.pi / 8.0)]
    vectors.append(quat_vector("quat_yaw45", yaw45))
    vectors.append(quat_vector("quat_yaw45_negated", [-c for c in yaw45]))
    vectors.append(quat_vector("quat_w_negative", [0.5, -0.5, 0.5, -0.5]))
    # w == 0: sign decided by the first nonzero of (x, y, z).
    vectors.append(quat_vector("quat_w_zero_x_negative", [-1.0, 0.0, 0.0, 0.0]))
    vectors.append(quat_vector("quat_w_zero_z_negative", [0.0, 0.0, -1.0, 0.0]))
    vectors.append(quat_vector("quat_w_zero_positive", [0.0, 1.0, 0.0, 0.0]))
    vectors.append(quat_vector("quat_not_normalized", [0.0, 0.0, 0.0, 2.0]))
    vectors.append(quat_vector("quat_not_normalized_general", [1.0, 1.0, 1.0, 1.0]))

    # --- §8.4 canonical JSON ----------------------------------------------
    vectors.append(
        json_vector(
            "json_key_order_utf8",
            pairs=[
                ["z", 1],
                ["a", 2],
                ["A", 3],
                ["é", 4],
                ["я", 5],
                ["10", 6],
                ["2", 7],
                ["", 8],
            ],
        )
    )
    vectors.append(json_vector("json_escape_quote_backslash", value='he said "hi" \\ ok'))
    vectors.append(
        json_vector(
            "json_escape_control_chars",
            value="a\u0000b\bc\td\ne\u000bf\fg\rh\u001fi",
        )
    )
    vectors.append(json_vector("json_non_ascii_cyrillic_raw", value="окно_a"))
    # NFC normalization (§8.4): an NFD-spelled string must produce exactly the
    # bytes of its NFC spelling. Latin: e + U+0301 -> U+00E9.
    vectors.append(json_vector("json_nfd_latin_normalized", value="cafe\u0301"))
    vectors.append(json_vector("json_nfc_latin_reference", value="caf\u00e9"))
    # Cyrillic: и + U+0306 -> U+0439 (й).
    vectors.append(
        json_vector("json_nfd_cyrillic_normalized", value="\u0447\u0430\u0438\u0306\u043d\u0438\u043a")
    )
    vectors.append(json_vector("json_nfc_cyrillic_reference", value="\u0447\u0430\u0439\u043d\u0438\u043a"))
    # A combining mark with no precomposed form (Cyrillic 'о' + U+0301) is left
    # exactly as it is - NFC is not "compose everything".
    vectors.append(json_vector("json_nfc_no_precomposed_form", value="\u043e\u043a\u043d\u043e\u0301"))
    # Object keys are normalized BEFORE sorting: the NFD key `e`+U+0301 becomes
    # `é` (0xc3 0xa9) and therefore sorts after `z`, not between `a` and `z`.
    vectors.append(
        json_vector(
            "json_nfd_key_sorted_on_nfc_bytes",
            pairs=[["e\u0301", 1], ["z", 2], ["a", 3]],
        )
    )
    vectors.append(json_vector("json_emoji_raw", value="wall \U0001f9f1 a"))
    vectors.append(json_vector("json_del_char_not_escaped", value="ab"))
    vectors.append(json_vector("json_scalars", value=[True, False, None, 0, -5, 1000000]))
    vectors.append(json_vector("json_empty_containers", value={"a": {}, "b": [], "c": ""}))
    vectors.append(
        json_vector(
            "json_nested_structure",
            value={"nodes": [{"b": 2, "a": 1}, {"a": [1, [2, {"z": None}]]}]},
        )
    )

    # --- §8 composite ------------------------------------------------------
    vectors.append(composite_vector("composite_small_bundle", SAMPLE_COMPOSITE))
    vectors.append(composite_vector("composite_reserved_fields", SAMPLE_RESERVED))

    return vectors


def main() -> int:
    document = {
        "schema": "mh.canonical_vectors",
        "schema_version": 1,
        "vectors": build_vectors(),
    }
    names = [v["name"] for v in document["vectors"]]
    if len(names) != len(set(names)):
        raise SystemExit("duplicate vector names")

    # ensure_ascii=True on purpose: several vectors are *about* the exact code
    # point sequence of a string (NFD vs NFC), and `\uXXXX` escapes state that
    # sequence unambiguously - a raw-UTF-8 file could be silently re-normalized
    # by an editor or a platform tool and the vector would quietly stop testing
    # what it was written for.
    text = json.dumps(document, indent=2, sort_keys=True, ensure_ascii=True) + "\n"
    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT_PATH.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {len(names)} vectors to {OUTPUT_PATH}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
