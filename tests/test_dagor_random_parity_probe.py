"""Gates for the v5 GAZ oracle binding and Dagor parity probe."""

import json
from pathlib import Path

from tools.dagor_random_parity_probe import (
    DAGOR_SOURCE_COMMIT,
    generated_outputs,
    parse_dagor_random_fixture,
    parse_dagor_references,
    parity_document,
)


REPO_ROOT = Path(__file__).resolve().parent.parent
GAZ_REFERENCE = (
    REPO_ROOT / "reference" / "dagor_fixtures" / "gaz53" /
    "gaz53_body_bc_random_cmp.composit.blk"
)
PROBE_ROOT = REPO_ROOT / "golden" / "v5" / "dagor_random_probe"
PROBE_FIXTURE = PROBE_ROOT / "random_parity_probe.composit.blk"


def test_owner_gaz_oracle_uses_ordered_composite_options_with_implicit_weight_one():
    fixture = parse_dagor_random_fixture(GAZ_REFERENCE.read_bytes())
    assert [(option.kind, option.resource, option.weight) for option in fixture.options] == [
        ("composite", "gaz53_bread_b_cmp", 1.0),
        ("composite", "gaz53_wooden_b_cmp", 1.0),
        ("composite", "gaz53_wooden_c_cmp", 1.0),
    ]
    assert fixture.ranges == ()

    body_references = parse_dagor_references((
        GAZ_REFERENCE.parent / "gaz53_b_body_cmp.composit.blk").read_bytes())
    assert ("composite", "gaz53_b_window_front_cmp") in body_references
    assert ("mesh", "gaz53_b_wiper_right") in body_references


def test_minimal_probe_has_all_transform_ranges_and_real_composite_options():
    fixture = parse_dagor_random_fixture(PROBE_FIXTURE.read_bytes())
    assert [option.kind for option in fixture.options] == ["composite"] * 3
    assert [name for name, _ in fixture.ranges] == [
        "offset_x", "offset_y", "offset_z",
        "rot_x", "rot_y", "rot_z",
        "scale", "vertical_scale",
    ]


def test_pinned_source_facts_are_separate_and_disprove_stream_byte_identity():
    document = parity_document(PROBE_FIXTURE)
    assert document["status"] == {
        "mh_normative": "frozen",
        "dagor_evidence": "pinned_public_source_derived",
        "dagor_runtime_observation": "not_run",
        "owner_a_or_b_decision": "pending",
    }
    assert document["provenance"]["commit"] == DAGOR_SOURCE_COMMIT
    assert document["comparison"]["stream_bytes_equal"] is False
    assert document["comparison"]["selection_mismatch_seeds"] == [
        0, 2, 123, 1024, 2147483647,
    ]
    assert document["comparison"]["transform_axis_binding"] == (
        "requires_real_runtime_observation")
    first = document["vectors"][0]
    assert first["mh_random_stream_1"]["selection"]["option"] == 1
    assert first["dagor_pinned_source"]["selection"] == {
        "raw15": 0,
        "unit_f32": 0.0,
        "normalized_weights_f32": [
            0.3333333432674408,
            0.3333333432674408,
            0.3333333432674408,
        ],
        "option": 0,
    }


def test_probe_and_gaz_generated_files_are_byte_identical():
    for path, expected in generated_outputs(REPO_ROOT):
        assert path.read_bytes() == expected

    gaz = json.loads((
        REPO_ROOT / "golden" / "v5" / "gaz53" /
        "baseline_rng_choices.json").read_text(encoding="utf-8"))
    assert len(gaz["source_declared_closure"]["composite"]) == 7
    assert len(gaz["source_declared_closure"]["static_mesh"]) == 16


def test_runtime_template_never_claims_an_observation():
    template = json.loads(
        (PROBE_ROOT / "runtime_observation.template.json").read_text(encoding="utf-8"))
    assert template["status"] == "not_measured"
    assert template["seed_observations"] == []
    assert all(value is None for value in template["provenance"].values())


def test_gaz_protocol_fixture_contains_no_synthetic_option_tokens():
    payload = json.loads((
        REPO_ROOT / "golden" / "v5" / "gaz53" /
        "gaz53_body_bc_random_cmp.composite"
    ).read_text(encoding="utf-8"))
    options = payload["nodes"][0]["options"]
    assert [(option["kind"], option["resource"]) for option in options] == [
        ("composite", "gaz53_bread_b_cmp"),
        ("composite", "gaz53_wooden_b_cmp"),
        ("composite", "gaz53_wooden_c_cmp"),
    ]
