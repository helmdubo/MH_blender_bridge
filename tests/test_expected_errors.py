"""Structural validation of golden/expected_errors/*.json and the
composite-cycle fixture.

expected_errors files are the stage-B negative-test specification
(mh.validation_report, schema §6.2): stable machine codes from the §6.1
registry, subjects keyed by UID.
"""

import json
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))

from golden_uids import UIDS  # noqa: E402
from mh_canonical import ERROR_CODES, composite_content_hash, resource_filename  # noqa: E402

ERRORS_DIR = os.path.join(REPO_ROOT, "golden", "expected_errors")
CYCLE_FIXTURE_DIR = os.path.join(REPO_ROOT, "golden", "fixtures", "composite_cycle")

EXPECTED_SCENARIOS = {"duplicate_uid", "composite_cycle", "parent_uid_dangling"}
KNOWN_UIDS = set(UIDS.values())


def load_all():
    docs = {}
    for fname in os.listdir(ERRORS_DIR):
        if fname.endswith(".json"):
            with open(os.path.join(ERRORS_DIR, fname)) as f:
                docs[fname[:-5]] = json.load(f)
    return docs


def test_all_scenarios_covered():
    assert set(load_all()) == EXPECTED_SCENARIOS


def test_report_schema():
    for name, doc in load_all().items():
        assert doc["schema"] == "mh.validation_report", name
        assert doc["schema_version"] == 2, name
        assert set(doc) == {"schema", "schema_version", "errors", "warnings"}, name
        assert doc["errors"], f"{name}: an expected-error file must expect errors"
        assert doc["warnings"] == [], name


def test_codes_in_registry():
    for name, doc in load_all().items():
        for err in doc["errors"]:
            assert err["code"] in ERROR_CODES, f"{name}: unknown code {err['code']}"
            assert re.fullmatch(r"MH_E_[A-Z0-9_]+", err["code"]), (
                f"{name}: expected errors must be blocking (MH_E_*), got {err['code']}")


def test_subjects_sorted_and_known():
    for name, doc in load_all().items():
        for err in doc["errors"]:
            subjects = err["subjects"]
            assert subjects, f"{name}: error without subjects"
            assert subjects == sorted(subjects), f"{name}: subjects not sorted"
            for uid in subjects:
                assert uid in KNOWN_UIDS, f"{name}: unknown subject uid {uid}"


def test_cycle_fixture_is_schema_valid_and_cyclic():
    files = sorted(os.listdir(CYCLE_FIXTURE_DIR))
    assert len(files) == 2, files
    docs = []
    for fname in files:
        with open(os.path.join(CYCLE_FIXTURE_DIR, fname)) as f:
            doc = json.load(f)
        assert doc["schema"] == "mh.composite"
        assert doc["schema_version"] == 1
        assert fname == resource_filename(doc["name"], doc["uid"], ".composite")
        # Must be canonicalizable/hashable without errors.
        assert composite_content_hash(doc).startswith("xxh3:")
        docs.append(doc)
    # The two composites must reference each other (a real cycle).
    a, b = docs
    a_refs = {n["resource_uid"] for n in a["nodes"] if n["kind"] == "composite_ref"}
    b_refs = {n["resource_uid"] for n in b["nodes"] if n["kind"] == "composite_ref"}
    assert b["uid"] in a_refs and a["uid"] in b_refs


def test_cycle_subjects_match_fixture():
    doc = load_all()["composite_cycle"]
    subjects = doc["errors"][0]["subjects"]
    assert subjects == sorted([UIDS["col/cycle_a"], UIDS["col/cycle_b"]])
