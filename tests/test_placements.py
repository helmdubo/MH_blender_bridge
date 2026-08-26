"""Pure Source Protocol v5 placement-profile codec gates."""

import json
from pathlib import Path

import pytest

from addon.mh4blend.core.model import PlacementProfile, PlacementRange
from addon.mh4blend.core.placements import (
    PlacementValueError,
    parse_placement_profile,
    placement_document,
    placement_json_bytes,
    read_placement_file,
)


REPO_ROOT = Path(__file__).resolve().parent.parent
GOLDEN_PATH = REPO_ROOT / "golden" / "v5" / "source_protocol_v5_codec_vectors.json"


def test_shared_placement_vectors_are_byte_exact_and_roundtrip():
    fixture = json.loads(GOLDEN_PATH.read_text(encoding="utf-8"))
    for vector in fixture["placement_vectors"]:
        expected = vector["canonical_utf8"].encode("utf-8")
        assert placement_json_bytes(parse_placement_profile(expected)) == expected
    for vector in fixture["placement_negative_vectors"]:
        with pytest.raises(PlacementValueError) as excinfo:
            parse_placement_profile(vector["json"])
        assert excinfo.value.code == vector["error"], vector["name"]


def test_placement_field_order_and_omission_rules():
    profile = PlacementProfile(
        "scatter",
        offset_cm=(
            PlacementRange(0, 1),
            PlacementRange(0, 2),
            PlacementRange(0, 3),
        ),
        vertical_scale=PlacementRange(1, 0.1),
    )
    assert list(placement_document(profile)) == [
        "v", "kind", "offset_cm", "vertical_scale"]


def test_scale_domain_is_validated_before_sampling():
    with pytest.raises(PlacementValueError) as excinfo:
        placement_json_bytes(PlacementProfile(
            "bad", uniform_scale=PlacementRange(0.5, 0.5)))
    assert excinfo.value.code == "MH_E_PLACEMENT_PROFILE_GRAMMAR"


def test_read_uses_filename_identity(tmp_path):
    path = tmp_path / "scatter.placement"
    path.write_bytes(b'{"v":1,"kind":"placement_profile"}')
    assert read_placement_file(path).name == "scatter"
