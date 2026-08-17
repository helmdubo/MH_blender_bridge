"""Tests for mh4blend.core.uid (§4, §4.1) on plain-dict carriers."""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.uid import (  # noqa: E402
    arbitrate_resource_duplicates,
    ensure_uid,
    find_duplicate_uids,
    peek_uid,
)

UUID_RE = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")


def test_ensure_uid_assigns_once_and_never_overwrites():
    carrier = {}
    uid = ensure_uid(carrier)
    assert UUID_RE.match(uid)
    assert ensure_uid(carrier) == uid
    assert carrier["mh_uid"] == uid


def test_peek_ignores_empty_and_non_string():
    assert peek_uid({}) is None
    assert peek_uid({"mh_uid": ""}) is None
    assert peek_uid({"mh_uid": 5}) is None


def test_find_duplicates_only_reports_shared_uids():
    a, b = {"mh_uid": "x" * 8}, {"mh_uid": "x" * 8}
    c, d = {"mh_uid": "unique"}, {}
    dups = find_duplicate_uids([a, b, c, d])
    assert list(dups) == ["x" * 8]
    assert dups["x" * 8] == [a, b]


def test_arbitration_by_manifest_hash():
    """The Make Single User mechanics (§4.1): the untouched original keeps
    the UID, the modified copy gets reassigned."""
    original = {"mh_uid": "shared", "payload": "old"}
    copy = {"mh_uid": "shared", "payload": "edited"}
    result = arbitrate_resource_duplicates(
        "shared", [original, copy],
        manifest_hash="hash(old)",
        hash_of=lambda c: f"hash({c['payload']})")
    assert result.resolved
    assert result.keeper is original
    assert result.reassign == [copy]


def test_arbitration_manual_when_no_manifest_or_ambiguous():
    a = {"mh_uid": "shared", "payload": "x"}
    b = {"mh_uid": "shared", "payload": "y"}
    assert not arbitrate_resource_duplicates(
        "shared", [a, b], None, lambda c: c["payload"]).resolved
    # both changed
    assert not arbitrate_resource_duplicates(
        "shared", [a, b], "hash-of-neither", lambda c: c["payload"]).resolved
    # both identical to the manifest (identical copies) — still manual
    twin_a = {"mh_uid": "shared", "payload": "same"}
    twin_b = {"mh_uid": "shared", "payload": "same"}
    assert not arbitrate_resource_duplicates(
        "shared", [twin_a, twin_b], "same", lambda c: c["payload"]).resolved
