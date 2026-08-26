"""Pure Blender/UE transform seam tests for the R1 pinned FBX mapping."""

import math
import json
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.model import CompositeTransform  # noqa: E402
from mh4blend.core.transforms import (  # noqa: E402
    blender_to_ue_transform,
    matrix_reconstructs_as_float32_trs,
    quat_from_ue,
    quat_to_ue,
    scale_from_ue,
    scale_to_ue,
    translation_from_ue,
    translation_to_ue,
    ue_to_blender_transform,
    validate_scale,
)


def _quat_equivalent(left, right, tolerance=1e-6):
    direct = max(abs(a - b) for a, b in zip(left, right))
    negated = max(abs(a + b) for a, b in zip(left, right))
    return min(direct, negated) <= tolerance


def _axis_angle(axis, degrees):
    x, y, z = axis
    half = math.radians(degrees) / 2.0
    sine = math.sin(half)
    return (x * sine, y * sine, z * sine, math.cos(half))


def test_r1_translation_mapping_is_float32_without_quantization():
    assert translation_to_ue((1.0, 2.0, 3.0)) == (100.0, -200.0, 300.0)
    assert translation_to_ue((0.001, 0.0, 0.0)) == pytest.approx(
        (0.1, 0.0, 0.0), abs=1e-8)
    assert translation_from_ue((100.0, -200.0, 300.0)) == (1.0, 2.0, 3.0)


def test_r1_quaternion_mapping_and_canonical_sign():
    source = _axis_angle((0.267261, 0.534522, 0.801784), 30)
    expected = (-source[0], source[1], -source[2], source[3])
    assert _quat_equivalent(quat_to_ue(source), expected)
    assert _quat_equivalent(quat_from_ue(quat_to_ue(source)), source)
    assert quat_to_ue((0, 0, 0, -1)) == (0.0, 0.0, 0.0, 1.0)


def test_r1_scale_passthrough_and_validation():
    assert scale_to_ue((1.25, 2.0, 0.5)) == (1.25, 2.0, 0.5)
    assert scale_from_ue((1.25, 2.0, 0.5)) == (1.25, 2.0, 0.5)
    assert scale_to_ue((-1.0, 2.0, 0.5)) == (-1.0, 2.0, 0.5)
    with pytest.raises(ValueError, match="MH_E_INVALID_SCALE"):
        validate_scale((1.0, 0.0, 1.0))
    with pytest.raises(ValueError, match="MH_E_NAN_INF_VALUE"):
        validate_scale((1.0, float("nan"), 1.0))


def test_shared_8_ulp_transform_representability_vectors():
    fixture = json.loads((
        REPO_ROOT / "golden" / "v5" / "source_protocol_v5_codec_vectors.json"
    ).read_text(encoding="utf-8"))
    for vector in fixture["transform_representability_vectors"]:
        assert matrix_reconstructs_as_float32_trs(
            vector["matrix"], vector["reconstructed"]
        ) is vector["representable"], vector["name"]


def test_nonfinite_matrix_is_unrepresentable():
    identity = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]
    invalid = [row[:] for row in identity]
    invalid[0][0] = float("nan")
    assert not matrix_reconstructs_as_float32_trs(invalid, identity)


@pytest.mark.parametrize("translation,rotation,scale", [
    ((0, 0, 0), (0, 0, 0, 1), (1, 1, 1)),
    ((1.25, -2.5, 0.75), _axis_angle((0, 0, 1), 30), (1.2, 0.5, 2.0)),
    ((-0.001, 0.003, 9.0), _axis_angle((1, 0, 0), 170), (3, 4, 5)),
])
def test_blender_json_blender_roundtrip_is_float32_identity(
        translation, rotation, scale):
    protocol = blender_to_ue_transform(translation, rotation, scale)
    assert isinstance(protocol, CompositeTransform)
    got_translation, got_rotation, got_scale = ue_to_blender_transform(protocol)
    assert got_translation == pytest.approx(translation, abs=1e-6)
    assert _quat_equivalent(got_rotation, rotation)
    assert got_scale == pytest.approx(scale, abs=1e-6)
