"""Unit gates for the S6 report validator, NOT cross-host acceptance runs.

The fake UE reports below deliberately copy a Python observation into temporary
files. They exercise rejection/validation only; they are never receipts or proof
that Automation, Editor preview, PIE, or packaged execution took place.
"""

from copy import deepcopy
import json

import pytest

from tools import mh_random_reference as reference
from tools import s6_runtime_parity as parity


def _write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, allow_nan=False), encoding="utf-8")


@pytest.fixture
def validator_reports(tmp_path):
    """Synthetic validator inputs only; actual host reports are produced elsewhere."""
    observation = parity.observe(parity.DEFAULT_GOLDEN, tmp_path / "Saved")
    reports = {"python": tmp_path / "Saved/Mimir/S6/python.json"}
    for lane in ("automation", "editor_preview", "pie", "packaged"):
        unit_report = deepcopy(observation)
        unit_report["lane"] = lane
        unit_report["world_type"] = {"pie": "PIE", "packaged": "Game"}.get(lane, "EditorPreview")
        unit_report["runtime_modules_only"] = lane == "packaged"
        for plan in unit_report["plans"]:
            for leaf in plan["leaves"]:
                leaf["materialized_matrix"] = deepcopy(leaf["world_matrix"])
                leaf["component_class"] = "/Script/Engine.StaticMeshComponent"
        path = tmp_path / f"validator_unit_only_{lane}.json"
        _write_json(path, unit_report)
        reports[lane] = path
    return reports


def _assert_bad_reports(reports, lane, mutate):
    path = reports[lane]
    original = path.read_bytes()
    changed = json.loads(original)
    mutate(changed)
    _write_json(path, changed)
    try:
        with pytest.raises(ValueError):
            parity.verify(parity.DEFAULT_GOLDEN, reports)
    finally:
        path.write_bytes(original)


def test_python_observer_preserves_golden_and_reference_wrapper(tmp_path):
    golden_before = parity.DEFAULT_GOLDEN.read_bytes()
    compose_before = reference.compose_trs
    report = parity.observe(parity.DEFAULT_GOLDEN, tmp_path / "Saved")
    assert tuple(report["seed_set"]) == parity.SEEDS
    assert len(report["plans"]) == 7
    assert all(leaf["world_matrix"] for plan in report["plans"] for leaf in plan["leaves"])
    assert reference.compose_trs is compose_before
    assert parity.DEFAULT_GOLDEN.read_bytes() == golden_before
    written = json.loads((tmp_path / "Saved/Mimir/S6/python.json").read_bytes())
    assert written["lane"] == "python"
    assert len(written["plans"]) == 7


def test_python_observer_restores_wrapper_after_resolver_failure(tmp_path, monkeypatch):
    golden_before = parity.DEFAULT_GOLDEN.read_bytes()
    compose_before = reference.compose_trs

    def fail_resolver(*args, **kwargs):
        assert reference.compose_trs is not compose_before
        raise ValueError("injected resolver failure")

    monkeypatch.setattr(reference, "resolve_composite", fail_resolver)
    with pytest.raises(ValueError, match="injected resolver failure"):
        parity.observe(parity.DEFAULT_GOLDEN, tmp_path / "Saved")
    assert reference.compose_trs is compose_before
    assert parity.DEFAULT_GOLDEN.read_bytes() == golden_before
    assert not (tmp_path / "Saved/Mimir/S6/python.json").exists()


def test_validator_accepts_consistent_synthetic_unit_reports_only(validator_reports):
    # This is a validator sanity check, deliberately NOT five host executions.
    parity.verify(parity.DEFAULT_GOLDEN, validator_reports)


def test_validator_rejects_seed_tag_and_frozen_choice_corruption(validator_reports):
    def change_choice(report):
        report["plans"][0]["decisions"][0]["option"] += 1

    def boolean_choice(report):
        decision = next(decision for plan in report["plans"] for decision in plan["decisions"]
                        if decision["option"] in (0, 1))
        decision["option"] = bool(decision["option"])

    mutations = (
        lambda report: report["seed_set"].pop(),
        lambda report: report["plans"].pop(),
        lambda report: report["seed_set"].__setitem__(0, False),
        lambda report: report.__setitem__("stream", "mh.random_stream:unratified"),
        lambda report: report.__setitem__("resolver", "mh.random_resolver:unratified"),
        change_choice,
        boolean_choice,
    )
    for mutate in mutations:
        _assert_bad_reports(validator_reports, "automation", mutate)
    # A forged Python frozen field is rejected against the immutable golden too.
    _assert_bad_reports(validator_reports, "python", change_choice)


def test_validator_rejects_full_world_matrix_drift(validator_reports):
    def drift(report):
        # Smaller than host materialization tolerance: full plan products still
        # must compare exactly against the separate Python observation.
        report["plans"][0]["leaves"][0]["world_matrix"][0][0] += 1e-9

    _assert_bad_reports(validator_reports, "editor_preview", drift)


def test_validator_rejects_materialized_component_drift(validator_reports):
    def drift(report):
        report["plans"][0]["leaves"][0]["materialized_matrix"][3][0] += 1.0

    _assert_bad_reports(validator_reports, "pie", drift)


def test_validator_rejects_boolean_or_nonnumeric_materialized_matrix_entries(validator_reports):
    for row, column, value in ((0, 3, False), (3, 3, True), (0, 3, "0"), (0, 3, None)):
        def corrupt(report, row=row, column=column, value=value):
            # False/True otherwise round-trip as the exact expected 0/1 and
            # would pass the geometric tolerance check after struct.pack.
            report["plans"][0]["leaves"][0]["materialized_matrix"][row][column] = value

        _assert_bad_reports(validator_reports, "packaged", corrupt)


def test_validator_requires_packaged_game_and_boolean_runtime_only_proof(validator_reports):
    for value in (False, "true", 1):
        _assert_bad_reports(validator_reports, "packaged",
                            lambda report, value=value: report.__setitem__("runtime_modules_only", value))
    _assert_bad_reports(validator_reports, "packaged",
                        lambda report: report.__setitem__("world_type", "PIE"))
    _assert_bad_reports(validator_reports, "pie",
                        lambda report: report.__setitem__("world_type", "EditorPreview"))


def test_validator_rejects_duplicate_json_keys_and_reused_lane_file(validator_reports):
    path = validator_reports["automation"]
    original = path.read_bytes()
    path.write_bytes(original.replace(b'"schema":', b'"schema":"duplicate","schema":', 1))
    try:
        with pytest.raises(ValueError, match="duplicate JSON key"):
            parity.verify(parity.DEFAULT_GOLDEN, validator_reports)
    finally:
        path.write_bytes(original)
    duplicate_path_reports = dict(validator_reports)
    duplicate_path_reports["pie"] = duplicate_path_reports["automation"]
    with pytest.raises(ValueError, match="five separate report files"):
        parity.verify(parity.DEFAULT_GOLDEN, duplicate_path_reports)
