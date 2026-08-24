"""Pure Source Protocol v4 material codec and resolver gates."""

import json
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.materials import (  # noqa: E402
    MaterialValueError,
    material_document,
    material_json_bytes,
    parse_material,
    resolve_texture_reference,
)
from mh4blend.core.model import MaterialResource  # noqa: E402


def test_shared_material_golden_vectors_are_byte_exact_and_roundtrip():
    fixture = json.loads(
        (REPO_ROOT / "golden" / "material_v4_vectors.json").read_text(
            encoding="utf-8"))
    assert fixture["schema"] == "mh.material_v4_vectors"
    for vector in fixture["vectors"]:
        expected = vector["canonical_utf8"].encode("utf-8")
        assert material_json_bytes(vector["value"]) == expected, vector["name"]
        assert material_json_bytes(parse_material(expected)) == expected


def test_class_form_orders_slots_params_and_integral_numbers():
    resource = MaterialResource(
        "wall", material_class="layered", twosided=True,
        textures={"tex10": "normal", "tex2": "diffuse"},
        params={"z": 1.0, "a": [0.5, 1.0, -2.0, 4]},
    )
    payload = material_json_bytes(resource)
    assert payload.endswith(b"\n")
    assert payload.index(b'"tex2"') < payload.index(b'"tex10"')
    assert payload.index(b'"a"') < payload.index(b'"z"')
    assert b"1.0" not in payload and b"-2.0" not in payload
    assert parse_material(payload, name="wall") == resource


def test_library_form_is_exactly_one_field():
    assert material_document(MaterialResource("wet", library="wet_concrete")) == {
        "library": "wet_concrete"}
    with pytest.raises(MaterialValueError) as excinfo:
        material_document(MaterialResource(
            "wet", library="wet_concrete", params={"roughness": 0.2}))
    assert excinfo.value.code == "MH_E_MATERIAL_GRAMMAR"


@pytest.mark.parametrize("document", [
    {},
    {"class": "simple", "unknown": 1},
    {"class": "simple", "library": "base"},
    {"library": "base", "twosided": False},
    {"class": "simple", "twosided": 1},
    {"class": "simple", "textures": []},
    {"class": "simple", "textures": {"tex01": "a"}},
    {"class": "simple", "textures": {"tex16": "a"}},
    {"class": "simple", "params": {"A": 1}},
    {"class": "simple", "params": {"v": [1, 2, 3]}},
    {"class": "simple", "params": {"v": True}},
])
def test_closed_grammar_rejects_unknown_fields_and_shapes(document):
    with pytest.raises(MaterialValueError) as excinfo:
        parse_material(document)
    assert excinfo.value.code == "MH_E_MATERIAL_GRAMMAR"


@pytest.mark.parametrize("reference", [
    "brick.png", "textures/brick", "Brick", "brick-normal", "", "кирпич",
])
def test_texture_reference_is_exact_extensionless_logical_name(reference):
    with pytest.raises(MaterialValueError) as excinfo:
        parse_material({"class": "simple", "textures": {"tex0": reference}})
    assert excinfo.value.code == "MH_E_NONCANONICAL_TEXTURE_REFERENCE"


def test_reader_rejects_duplicate_keys_and_nan():
    with pytest.raises(MaterialValueError, match="duplicate JSON field"):
        parse_material('{"class":"a","class":"b"}')
    with pytest.raises(MaterialValueError) as excinfo:
        parse_material('{"class":"a","params":{"bad":NaN}}')
    assert excinfo.value.code == "MH_E_NAN_INF_VALUE"


def test_texture_resolver_scans_source_tree_by_kind_and_stem(tmp_path):
    nested = tmp_path / "moved" / "deep"
    nested.mkdir(parents=True)
    texture = nested / "marble_d.tga"
    texture.write_bytes(b"texture")
    (nested / "marble_d.material").write_text("{}", encoding="utf-8")
    assert resolve_texture_reference(tmp_path, "marble_d") == texture


def test_texture_resolver_fails_closed_for_missing_and_same_stem(tmp_path):
    with pytest.raises(MaterialValueError) as excinfo:
        resolve_texture_reference(tmp_path, "missing")
    assert excinfo.value.code == "MH_E_UNRESOLVED_TEXTURE_REFERENCE"

    (tmp_path / "same.png").write_bytes(b"png")
    (tmp_path / "same.tif").write_bytes(b"tif")
    with pytest.raises(MaterialValueError) as excinfo:
        resolve_texture_reference(tmp_path, "same")
    assert excinfo.value.code == "MH_E_AMBIGUOUS_RESOURCE_NAME"


def test_texture_resolver_rejects_uppercase_extension(tmp_path):
    (tmp_path / "wall.PNG").write_bytes(b"png")
    with pytest.raises(MaterialValueError) as excinfo:
        resolve_texture_reference(tmp_path, "wall")
    assert excinfo.value.code == "MH_E_NONCANONICAL_RESOURCE_NAME"


def test_texture_resolver_surfaces_casefold_equal_noncanonical_stem(tmp_path):
    (tmp_path / "Wall.png").write_bytes(b"png")
    with pytest.raises(MaterialValueError) as excinfo:
        resolve_texture_reference(tmp_path, "wall")
    assert excinfo.value.code == "MH_E_NONCANONICAL_RESOURCE_NAME"


def test_targeted_texture_resolve_ignores_unrelated_invalid_candidate(tmp_path):
    valid = tmp_path / "wall.png"
    valid.write_bytes(b"wall")
    (tmp_path / "Other.PNG").write_bytes(b"other")
    assert resolve_texture_reference(tmp_path, "wall") == valid


def test_numeric_edges_narrow_to_float32_before_shortest_spelling():
    payload = material_json_bytes(MaterialResource(
        "probe", material_class="numeric_probe", params={
            "large_integral": 1e20,
            "negative_zero": -0.0,
            "non_float32_exact": 0.10000000001,
            "small_exponent": 1e-7,
        }))
    assert b'"large_integral": 1e+20' in payload
    assert b'"negative_zero": 0' in payload
    assert b'"non_float32_exact": 0.1' in payload
    assert b'"small_exponent": 1e-07' in payload
    assert material_json_bytes(parse_material(payload)) == payload


@pytest.mark.parametrize("bad", [float("nan"), float("inf"), -float("inf")])
def test_nonfinite_param_has_stable_code(bad):
    with pytest.raises(MaterialValueError) as excinfo:
        material_json_bytes(MaterialResource(
            "wall", material_class="simple", params={"bad": bad}))
    assert excinfo.value.code == "MH_E_NAN_INF_VALUE"
