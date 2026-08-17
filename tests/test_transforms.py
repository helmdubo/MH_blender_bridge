"""Tests for mh4blend.core.transforms (schema §11).

The strongest check is the commutation property: converting a Blender
transform to UE and applying it must give the same point as applying the
Blender transform first and mirroring the resulting world position —
M(T_blender(p)) == T_ue(M(p)) for the §11 mirror map M. This validates the
formula's internal consistency; the external arbiter stays the R1 UE test.
"""

import math
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.canonical import P_ROTATION_QUAT, P_SCALE, P_TRANSLATION_CM  # noqa: E402
from mh4blend.core.transforms import (  # noqa: E402
    quat_to_ue,
    scale_to_ue,
    translation_to_ue,
    validate_scale,
)


def rotate(quat_xyzw, point):
    """Rotate a 3D point by a unit quaternion (x, y, z, w) — reference impl."""
    qx, qy, qz, qw = quat_xyzw
    px, py, pz = point
    # q * p * q^-1 expanded
    tx = 2.0 * (qy * pz - qz * py)
    ty = 2.0 * (qz * px - qx * pz)
    tz = 2.0 * (qx * py - qy * px)
    return (
        px + qw * tx + (qy * tz - qz * ty),
        py + qw * ty + (qz * tx - qx * tz),
        pz + qw * tz + (qx * ty - qy * tx),
    )


def axis_angle(axis, degrees):
    x, y, z = axis
    half = math.radians(degrees) / 2.0
    s = math.sin(half)
    return (x * s, y * s, z * s, math.cos(half))


def mirror_m_to_ue_cm(point_m):
    """§11 map applied to a raw point: meters -> UE centimeters."""
    x, y, z = point_m
    return (x * 100.0, -y * 100.0, z * 100.0)


def test_translation_formula_and_quantization():
    assert translation_to_ue((1.0, 2.0, 3.0)) == (100000, -200000, 300000)
    assert translation_to_ue((0.0, 0.0, 0.0)) == (0, 0, 0)
    # p=3 on cm: 1 mm = 0.1 cm -> 100 units
    assert translation_to_ue((0.001, 0.0, 0.0)) == (100, 0, 0)


def test_quat_identity_and_canonical_sign():
    assert quat_to_ue((0.0, 0.0, 0.0, 1.0)) == (0, 0, 0, 10 ** P_ROTATION_QUAT)
    q = axis_angle((0, 0, 1), 90)
    assert quat_to_ue(q) == quat_to_ue(tuple(-c for c in q))


@pytest.mark.parametrize(
    "quat,point",
    [
        (axis_angle((0, 0, 1), 90), (1.0, 0.0, 0.0)),
        (axis_angle((0, 0, 1), 15), (0.37, 0.11, 1.93)),
        (axis_angle((1, 0, 0), 10), (0.5, -0.25, 2.0)),
        (axis_angle((0, 1, 0), 20), (1.25, -2.5, 0.75)),
        # composed rotation: 30deg around a skew axis
        (axis_angle((0.267261, 0.534522, 0.801784), 30), (0.37, 0.11, 1.93)),
    ],
)
def test_commutation_property(quat, point):
    """M(R_blender(p)) == R_ue(M(p)) within quantization tolerance."""
    world_blender = rotate(quat, point)
    expected_ue = mirror_m_to_ue_cm(world_blender)

    ue_quat_q = quat_to_ue(quat)
    ue_quat = tuple(c / 10 ** P_ROTATION_QUAT for c in ue_quat_q)
    # normalize after quantization for the reference rotation
    norm = math.sqrt(sum(c * c for c in ue_quat))
    ue_quat = tuple(c / norm for c in ue_quat)
    got_ue = rotate(ue_quat, mirror_m_to_ue_cm(point))

    for got, expected in zip(got_ue, expected_ue):
        assert abs(got - expected) < 0.01  # 0.01 cm — quantization noise


def test_full_transform_commutation_with_translation():
    quat = axis_angle((0, 0, 1), 30)
    translation = (1.25, -2.5, 0.75)
    point = (0.37, 0.11, 1.93)

    world_blender = tuple(
        r + t for r, t in zip(rotate(quat, point), translation))
    expected_ue = mirror_m_to_ue_cm(world_blender)

    ue_t = tuple(c / 10 ** P_TRANSLATION_CM for c in translation_to_ue(translation))
    ue_q = tuple(c / 10 ** P_ROTATION_QUAT for c in quat_to_ue(quat))
    norm = math.sqrt(sum(c * c for c in ue_q))
    ue_q = tuple(c / norm for c in ue_q)
    got_ue = tuple(
        r + t for r, t in zip(rotate(ue_q, mirror_m_to_ue_cm(point)), ue_t))

    for got, expected in zip(got_ue, expected_ue):
        assert abs(got - expected) < 0.01


def test_scale_passthrough_and_validation():
    assert scale_to_ue((1.0, 1.0, 1.0)) == (10 ** P_SCALE,) * 3
    assert scale_to_ue((1.2, 1.2, 1.2)) == (1200000,) * 3
    with pytest.raises(ValueError, match="MH_E_INVALID_SCALE"):
        validate_scale((1.0, -1.0, 1.0))
    with pytest.raises(ValueError, match="MH_E_INVALID_SCALE"):
        validate_scale((1.0, 0.0, 1.0))
    with pytest.raises(ValueError, match="MH_E_NAN_INF_VALUE"):
        validate_scale((float("nan"), 1.0, 1.0))
