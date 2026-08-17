"""Tests for mh4blend.core.validate (§6): every model-level code fires and
the composite-cycle verdict matches the stage-A expected_errors spec."""

import json
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))
sys.path.insert(0, str(REPO_ROOT / "tools"))

from golden_uids import UIDS  # noqa: E402
from mh4blend.core.model import (  # noqa: E402
    IDENTITY_TRANSFORM,
    Composite,
    Manifest,
    MaterialResource,
    MeshResource,
    Node,
    QuantizedTransform,
)
from mh4blend.core.validate import (  # noqa: E402
    ValidationError,
    ValidationWarning,
    build_report,
    validate_manifest,
)


def make_manifest(**overrides):
    base = dict(
        bundle_uid=UIDS["col/ca_building"], bundle_name="Golden",
        blend_file="golden.blend", exporter_version="0.2.0")
    base.update(overrides)
    return Manifest(**base)


def node(key, parent=None, kind="mesh", resource=None, transform=IDENTITY_TRANSFORM):
    return Node(
        node_uid=UIDS[key], parent_uid=parent, kind=kind, display_name=key,
        resource_uid=resource or UIDS["col/wall_a"], local_transform=transform)


def codes(report):
    return [(e["code"], tuple(e["subjects"])) for e in report["errors"]]


def test_clean_manifest_is_empty_report():
    manifest = make_manifest(
        meshes=[MeshResource(UIDS["col/wall_a"], "wall_a", "xxh3:0011223344556677")],
        composites=[Composite(UIDS["col/ca_windowset"], "ca_windowset",
                              [node("node/ca_windowset/window_1")])])
    report = validate_manifest(manifest)
    assert report == {"schema": "mh.validation_report", "schema_version": 2,
                      "errors": [], "warnings": []}


def test_cycle_fixture_verdict_matches_expected_errors_spec():
    """Validator vs the stage-A spec artifact: identical (code, subjects)."""
    composites = [
        Composite(UIDS["col/cycle_a"], "cycle_a", [
            node("node/cycle_a/ref_b", kind="composite_ref",
                 resource=UIDS["col/cycle_b"])]),
        Composite(UIDS["col/cycle_b"], "cycle_b", [
            node("node/cycle_b/ref_a", kind="composite_ref",
                 resource=UIDS["col/cycle_a"])]),
    ]
    report = validate_manifest(make_manifest(composites=composites))

    expected_path = REPO_ROOT / "golden" / "expected_errors" / "composite_cycle.json"
    expected = json.loads(expected_path.read_text())
    got = [{"code": e["code"], "subjects": e["subjects"]}
           for e in report["errors"]]
    want = [{"code": e["code"], "subjects": e["subjects"]}
            for e in expected["errors"]]
    assert got == want


def test_duplicate_node_uid_matches_expected_errors_spec():
    dup = node("node/ca_windowset/window_1")
    composite = Composite(UIDS["col/ca_windowset"], "ca_windowset",
                          [dup, node("node/ca_windowset/window_1")])
    report = validate_manifest(make_manifest(composites=[composite]))
    expected = json.loads(
        (REPO_ROOT / "golden" / "expected_errors" / "duplicate_uid.json").read_text())
    assert codes(report) == [
        (e["code"], tuple(e["subjects"])) for e in expected["errors"]]


def test_dangling_parent_and_parent_cycle():
    dangling = Composite(UIDS["col/ca_building"], "ca_building", [
        node("node/ca_building/leak_decal",
             parent="00000000-0000-0000-0000-000000000000")])
    report = validate_manifest(make_manifest(composites=[dangling]))
    assert ("MH_E_DANGLING_PARENT",
            (UIDS["node/ca_building/leak_decal"],)) in codes(report)

    a, b = UIDS["node/ca_building/props_group"], UIDS["node/ca_building/gp_wall"]
    cyclic = Composite(UIDS["col/ca_building"], "ca_building", [
        Node(a, b, "group", "a", None, IDENTITY_TRANSFORM),
        Node(b, a, "group", "b", None, IDENTITY_TRANSFORM)])
    report = validate_manifest(make_manifest(composites=[cyclic]))
    assert any(code == "MH_E_PARENT_CYCLE" for code, _ in codes(report))


def test_diamond_is_not_a_cycle():
    leaf = Composite(UIDS["col/cycle_b"], "leaf", [
        node("node/ca_windowset/window_1")])
    top = Composite(UIDS["col/cycle_a"], "top", [
        node("node/cycle_a/ref_b", kind="composite_ref",
             resource=UIDS["col/cycle_b"]),
        node("node/ca_building/windows_front", kind="composite_ref",
             resource=UIDS["col/cycle_b"])])
    report = validate_manifest(make_manifest(composites=[top, leaf]))
    assert report["errors"] == []


def test_zero_scale_ascii_name_uid8_and_authored_texture_paths():
    bad_scale = QuantizedTransform((0, 0, 0), (0, 0, 0, 1000000), (1000000, 0, 1000000))
    composite = Composite(UIDS["col/ca_windowset"], "ca_windowset", [
        node("node/ca_windowset/window_1", transform=bad_scale)])
    report = validate_manifest(make_manifest(composites=[composite]))
    assert ("MH_E_INVALID_SCALE",
            (UIDS["node/ca_windowset/window_1"],)) in codes(report)

    report = validate_manifest(make_manifest(
        meshes=[MeshResource(UIDS["col/wall_a"], "стена", "xxh3:0011223344556677")]))
    assert ("MH_E_NON_ASCII_RESOURCE_NAME", (UIDS["col/wall_a"],)) in codes(report)

    fake = UIDS["col/wall_a"][:8] + "-0000-0000-0000-000000000000"
    report = validate_manifest(make_manifest(
        meshes=[MeshResource(UIDS["col/wall_a"], "a", "xxh3:0011223344556677"),
                MeshResource(fake, "b", "xxh3:0011223344556677")]))
    assert any(code == "MH_E_UID8_COLLISION" for code, _ in codes(report))

    report = validate_manifest(make_manifest(materials=[
        MaterialResource(UIDS["mesh/wall_b"], "m_x", "shader",
                         textures={"tex0": "C:/abs/path_tex_d.tif"})]))
    assert report["errors"] == []


def test_registry_warnings_are_non_blocking_and_missing_registry_is_silent():
    material = MaterialResource(
        UIDS["mesh/wall_b"], "m_x", "unknown_shader")
    manifest = make_manifest(materials=[material])

    absent = validate_manifest(manifest, registry=None)
    assert absent["errors"] == []
    assert absent["warnings"] == []

    registry = {
        "schema": "mh.registry",
        "schema_version": 1,
        "shader_classes": ["rendinst_simple"],
    }
    report = validate_manifest(manifest, registry=registry)
    assert report["errors"] == []
    assert [(w["code"], w["subjects"]) for w in report["warnings"]] == [
        ("MH_W_UNKNOWN_SHADER_CLASS", [UIDS["mesh/wall_b"]])]


def test_invalid_registry_is_warning_not_error():
    report = validate_manifest(
        make_manifest(), registry={"schema": "mh.registry", "schema_version": 99})
    assert report["errors"] == []
    assert report["warnings"][0]["code"] == "MH_W_REGISTRY_INVALID"
    assert report["warnings"][0]["subjects"] == [UIDS["col/ca_building"]]


def test_report_builder_keeps_error_and_warning_channels_separate_and_sorted():
    errors = [ValidationError(
        "MH_E_EMPTY_MATERIAL_SLOT", [UIDS["col/wall_a"]], "empty slot")]
    warnings = [
        ValidationWarning(
            "MH_W_UNKNOWN_SHADER_CLASS", [UIDS["mesh/wall_b"]], "unknown"),
        ValidationWarning(
            "MH_W_REGISTRY_INVALID", [UIDS["col/ca_building"]], "invalid"),
    ]
    report = build_report(errors, warnings)
    assert [row["code"] for row in report["errors"]] == [
        "MH_E_EMPTY_MATERIAL_SLOT"]
    assert [row["code"] for row in report["warnings"]] == [
        "MH_W_REGISTRY_INVALID", "MH_W_UNKNOWN_SHADER_CLASS"]
    with pytest.raises(AssertionError, match="non-blocking code"):
        ValidationError("MH_W_UNKNOWN_SHADER_CLASS", [], "wrong channel")
    with pytest.raises(AssertionError, match="blocking code"):
        ValidationWarning("MH_E_EMPTY_MATERIAL_SLOT", [], "wrong channel")


@pytest.mark.parametrize(
    ("params", "expected_code"),
    [
        ({"bad": float("nan")}, "MH_E_NAN_INF_VALUE"),
        ({"bad": float("inf")}, "MH_E_NAN_INF_VALUE"),
        ({"bad": object()}, "MH_E_INVALID_MATERIAL_VALUE"),
    ],
)
def test_invalid_material_payload_is_reported_before_manifest_write(
        params, expected_code):
    material = MaterialResource(
        UIDS["mesh/wall_b"], "m_x", "rendinst_simple", params=params)
    report = validate_manifest(make_manifest(materials=[material]))
    assert [(row["code"], row["subjects"]) for row in report["errors"]] == [
        (expected_code, [UIDS["mesh/wall_b"]])]
