"""Pure material fingerprint and registry contract tests (B4-mat)."""

import sys
import unicodedata
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.materials import (  # noqa: E402
    MaterialRegistry,
    MaterialValueError,
    RegistryValidationError,
    material_canonical_form,
    material_content_hash,
    material_disk_payload,
    parse_registry,
)


def test_material_hash_is_order_and_nfc_independent():
    nfd = unicodedata.normalize("NFD", "café")
    nfc = unicodedata.normalize("NFC", "café")
    a = material_content_hash(
        "rendinst_simple",
        {"z": 2, "label": nfd, "color": [0.1, 1.0, 1, 1]},
        {"tex2": "common/n.tif", "tex0": "common/d.tif"},
    )
    b = material_content_hash(
        "rendinst_simple",
        {"color": [0.1, 1, 1.0, 1.0], "label": nfc, "z": 2.0},
        {"tex0": "common/d.tif", "tex2": "common/n.tif"},
    )
    assert a == b
    assert a.startswith("xxh3:") and len(a) == len("xxh3:") + 16


def test_material_hash_changes_with_each_semantic_section():
    base = material_content_hash("rendinst_simple", {"roughness": 0.5}, {"tex0": "a.tif"})
    assert material_content_hash("rendinst_layered", {"roughness": 0.5}, {"tex0": "a.tif"}) != base
    assert material_content_hash("rendinst_simple", {"roughness": 0.6}, {"tex0": "a.tif"}) != base
    assert material_content_hash("rendinst_simple", {"roughness": 0.5}, {"tex0": "b.tif"}) != base


def test_material_canonical_form_has_no_floats_and_rejects_bad_values():
    canonical = material_canonical_form(
        "rendinst_simple", {"scalar": 0.25, "sides": 0}, {})
    assert canonical == {
        "shader_class": "rendinst_simple",
        "params": {"scalar": 250000, "sides": 0},
        "textures": {},
    }
    with pytest.raises(MaterialValueError, match="MH_E_INVALID_MATERIAL_VALUE"):
        material_canonical_form("rendinst_simple", {"bad": object()}, {})
    with pytest.raises(MaterialValueError, match="collide after NFC"):
        material_canonical_form(
            "rendinst_simple",
            {unicodedata.normalize("NFD", "é"): 1, "é": 2},
            {},
        )


def test_material_disk_and_hash_share_p6_normalization():
    lower = {"uv_scale": 16.371, "roughness": 0.25, "sides": 0}
    sub_step = {"uv_scale": 16.3710003, "roughness": 0.2500004, "sides": 0.0}
    expected = {"uv_scale": 16.371, "roughness": 0.25, "sides": 0}

    assert material_disk_payload("rendinst_simple", lower, {}) == {
        "shader_class": "rendinst_simple", "params": expected, "textures": {}}
    assert material_disk_payload("rendinst_simple", sub_step, {}) == {
        "shader_class": "rendinst_simple", "params": expected, "textures": {}}
    assert (material_content_hash("rendinst_simple", lower, {})
            == material_content_hash("rendinst_simple", sub_step, {}))


@pytest.mark.parametrize("bad", [float("nan"), float("inf"), -float("inf")])
def test_material_nan_inf_has_stable_machine_code(bad):
    with pytest.raises(MaterialValueError) as excinfo:
        material_disk_payload("rendinst_simple", {"bad": bad}, {})
    assert excinfo.value.code == "MH_E_NAN_INF_VALUE"


def test_material_unsupported_value_has_stable_machine_code():
    with pytest.raises(MaterialValueError) as excinfo:
        material_disk_payload("rendinst_simple", {"bad": {1, 2}}, {})
    assert excinfo.value.code == "MH_E_INVALID_MATERIAL_VALUE"


def test_registry_parser_accepts_dict_json_and_extra_sections():
    document = {
        "schema": "mh.registry",
        "schema_version": 1,
        "shader_classes": ["rendinst_simple", "rendinst_layered"],
        "future_section": {"ignored": True},
    }
    parsed = parse_registry(document)
    reparsed = parse_registry(
        '{"schema":"mh.registry","schema_version":1,'
        '"shader_classes":["rendinst_simple"]}'
    )
    assert isinstance(parsed, MaterialRegistry)
    assert parsed.contains("rendinst_layered")
    assert reparsed.shader_classes == frozenset({"rendinst_simple"})
    assert parse_registry(parsed) is parsed


@pytest.mark.parametrize(
    "document",
    [
        [],
        {"schema": "wrong", "schema_version": 1, "shader_classes": []},
        {"schema": "mh.registry", "schema_version": 2, "shader_classes": []},
        {"schema": "mh.registry", "schema_version": 1.0, "shader_classes": []},
        {"schema": "mh.registry", "schema_version": 1, "shader_classes": "x"},
        {"schema": "mh.registry", "schema_version": 1, "shader_classes": [1]},
        "{not json",
    ],
)
def test_registry_parser_rejects_malformed_contract(document):
    with pytest.raises(RegistryValidationError):
        parse_registry(document)
