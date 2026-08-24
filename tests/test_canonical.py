"""Tests for the retained Source Protocol v4 canonical primitives."""

import math
from pathlib import Path
import re
import sys

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.canonical import (
    ERROR_CODES,
    canonical_json_bytes,
    canonicalize_quat,
    nfc,
    quantize,
    resource_filename,
    validate_resource_name,
)


def test_error_codes_registry_matches_golden_list():
    assert ERROR_CODES == frozenset({
        "MH_E_AMBIGUOUS_GENERATED_ASSET",
        "MH_E_AMBIGUOUS_RESOURCE_NAME",
        "MH_E_AMBIGUOUS_RESOURCE_OWNER",
        "MH_E_COMPOSITE_CYCLE",
        "MH_E_COMPOSITE_GRAMMAR",
        "MH_E_DANGLING_PARENT",
        "MH_E_DEPRECATED_LOD_ROWS",
        "MH_E_DIVERGENT_REVISIONS",
        "MH_E_EMPTY_MATERIAL_SLOT",
        "MH_E_EMPTY_RESOURCE_COLLECTION",
        "MH_E_EXTERNAL_MODIFICATION_CONFIRMATION_REQUIRED",
        "MH_E_INVALID_COLLECTION_OFFSET",
        "MH_E_INVALID_NODE_MARKERS",
        "MH_E_INVALID_EXPORT_MANIFEST",
        "MH_E_IMPORT_TARGET_OCCUPIED",
        "MH_E_INVALID_LOD_HIERARCHY",
        "MH_E_INVALID_MATERIAL_VALUE",
        "MH_E_INVALID_RESOURCE_SOURCE",
        "MH_E_INVALID_SCALE",
        "MH_E_LOD_LEVELS_SPARSE",
        "MH_E_LOD_SLOT_NOT_IN_BASE",
        "MH_E_MATERIAL_GRAMMAR",
        "MH_E_MATERIAL_NOT_ROUNDTRIPPABLE",
        "MH_E_MATERIAL_SLOT_CONFLICT",
        "MH_E_NAME_MISMATCH",
        "MH_E_NAN_INF_VALUE",
        "MH_E_NESTED_COMPOSITE_COLLECTION",
        "MH_E_NONCANONICAL_RESOURCE_NAME",
        "MH_E_NONCANONICAL_TEXTURE_REFERENCE",
        "MH_E_PARENT_CYCLE",
        "MH_E_PARENT_OUTSIDE_RESOURCE",
        "MH_E_PAYLOAD_LOCK_TIMEOUT",
        "MH_E_RESOURCE_KIND_MISMATCH",
        "MH_E_RESOURCE_NOT_FOUND",
        "MH_E_SOURCE_INDEX_INVALID",
        "MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT",
        "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
        "MH_E_TARGET_NAME_COLLISION",
        "MH_E_TEXTURE_OUTSIDE_ROOT",
        "MH_E_UNKNOWN_SCHEMA_VERSION",
        "MH_E_UNRESOLVED_EXTERNAL",
        "MH_E_UNRESOLVED_COMPOSITE_REFERENCE",
        "MH_E_UNRESOLVED_MATERIAL_REFERENCE",
        "MH_E_UNRESOLVED_TEXTURE_REFERENCE",
        "MH_E_UNSUPPORTED_NODE_KIND",
        "MH_W_DUPLICATE_RESOURCE_NAME",
        "MH_W_LOD_AUX_NODE_IGNORED",
        "MH_W_MANAGED_ASSET_LOCALLY_MODIFIED",
        "MH_W_MATERIAL_PAYLOAD_FALLBACK",
        "MH_W_MATERIAL_SLOT_NOT_FOUND",
        "MH_W_MATERIAL_SLOT_UNMAPPED",
        "MH_W_ORPHAN_REBOUND_CONTENT_DIVERGED",
        "MH_W_PAYLOAD_EXTERNAL_MODIFIED",
        "MH_W_PROBABLE_RESOURCE_RENAME",
        "MH_W_REGISTRY_INVALID",
        "MH_W_REGISTRY_STALE",
        "MH_W_RESOURCE_FAR_FROM_ORIGIN",
        "MH_W_UNRESOLVED_PLACEMENT",
    })
    assert sum(code.startswith("MH_E_") for code in ERROR_CODES) == 45
    assert sum(code.startswith("MH_W_") for code in ERROR_CODES) == 13
    assert all(re.fullmatch(r"MH_[EW]_[A-Z0-9_]+", code)
               for code in ERROR_CODES)


@pytest.mark.parametrize("value,precision,expected", [
    (0.0005, 3, 0),
    (0.0015, 3, 2),
    (0.0025, 3, 2),
    (-0.0015, 3, -2),
    (12.3456, 3, 12346),
])
def test_quantize_half_even(value, precision, expected):
    assert quantize(value, precision) == expected


@pytest.mark.parametrize("bad", [math.nan, math.inf, -math.inf])
def test_quantize_rejects_non_finite(bad):
    with pytest.raises(ValueError, match="MH_E_NAN_INF_VALUE"):
        quantize(bad, 3)


def test_quaternion_sign_and_normalization_are_canonical():
    assert canonicalize_quat((0, 0, 0, -1)) == (0, 0, 0, 1_000_000)
    assert canonicalize_quat((0, 0, 0, 2)) == (0, 0, 0, 1_000_000)


def test_canonical_json_is_compact_sorted_utf8_and_nfc():
    assert canonical_json_bytes({"z": 1, "a": "e\u0301"}) == (
        '{"a":"é","z":1}'.encode("utf-8"))
    assert nfc("e\u0301") == "é"


def test_canonical_json_rejects_floats():
    with pytest.raises(TypeError):
        canonical_json_bytes({"value": 1.0})


@pytest.mark.parametrize("name", ["a", "garage_a", "asset_01", "0"])
def test_logical_name_accepts_exact_v4_alphabet(name):
    validate_resource_name(name)
    assert resource_filename(name, ".mesh.fbx") == f"{name}.mesh.fbx"


@pytest.mark.parametrize(
    "name", ["", "Garage", "garage a", "garage-a", "foo.bar", "гараж"])
def test_logical_name_rejects_without_normalization(name):
    with pytest.raises(ValueError, match="MH_E_NONCANONICAL_RESOURCE_NAME"):
        validate_resource_name(name)
    with pytest.raises(ValueError, match="MH_E_NONCANONICAL_RESOURCE_NAME"):
        resource_filename(name, ".mesh.fbx")


def test_resource_filename_requires_dot_prefixed_extension():
    with pytest.raises(ValueError, match="dot-prefixed"):
        resource_filename("garage", "mesh.fbx")
