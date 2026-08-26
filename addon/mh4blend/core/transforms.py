"""Blender/UE placement conversion at the pinned FBX parity seam.

The protocol stores unquantized binary32 values in UE centimeters, UE axes,
and ``FQuat`` order ``[x, y, z, w]``.  The mapping below is the R1 UE 5.7
result for the pinned FBX export path (Forward=X, Up=Z, centimeters).  S3's
repeated UE parity gate remains the external arbiter for this pure-math seam.

No ``bpy`` or ``mathutils`` dependency is allowed here.  Callers convert a
``mathutils.Quaternion`` from ``(w, x, y, z)`` before entering this module.
"""

from __future__ import annotations

import math
import struct
from typing import Iterable

from .canonical_json import narrow_float32
from .model import CompositeTransform

__all__ = [
    "blender_to_ue_transform",
    "matrix_reconstructs_as_float32_trs",
    "quat_from_ue",
    "quat_to_ue",
    "scale_from_ue",
    "scale_to_ue",
    "translation_from_ue",
    "translation_to_ue",
    "ue_to_blender_transform",
    "validate_scale",
]


def _vector(value: Iterable[float], length: int, label: str) -> tuple[float, ...]:
    try:
        result = tuple(value)
    except TypeError as exc:
        raise ValueError(f"MH_E_COMPOSITE_GRAMMAR: {label} must be a vector") from exc
    if len(result) != length:
        raise ValueError(
            f"MH_E_COMPOSITE_GRAMMAR: {label} must contain {length} numbers")
    out = []
    for component in result:
        if isinstance(component, bool) or not isinstance(component, (int, float)):
            raise ValueError(
                f"MH_E_COMPOSITE_GRAMMAR: {label} contains a non-number")
        if not math.isfinite(component):
            raise ValueError(f"MH_E_NAN_INF_VALUE: non-finite {label}")
        try:
            out.append(narrow_float32(component))
        except ValueError as exc:
            raise ValueError(
                f"MH_E_COMPOSITE_GRAMMAR: {label} exceeds float32") from exc
    return tuple(out)


def _canonical_quaternion(quat_xyzw) -> tuple[float, float, float, float]:
    quat = _vector(quat_xyzw, 4, "rotation_quat")
    norm = math.sqrt(sum(component * component for component in quat))
    if norm == 0.0:
        raise ValueError("MH_E_COMPOSITE_GRAMMAR: zero quaternion")
    result = tuple(narrow_float32(component / norm) for component in quat)
    negate = result[3] < 0.0
    if result[3] == 0.0:
        first_nonzero = next(
            (component for component in result[:3] if component != 0.0), 0.0)
        negate = first_nonzero < 0.0
    if negate:
        result = tuple(narrow_float32(-component) for component in result)
    return result


def translation_to_ue(translation_m):
    """Blender world translation in meters -> UE centimeters.

    This is the placement observed through the pinned Blender -> UE FBX path.
    """
    x, y, z = _vector(translation_m, 3, "translation")
    return tuple(narrow_float32(component) for component in (
        x * 100.0, -y * 100.0, z * 100.0))


def translation_from_ue(translation_cm):
    """UE centimeters -> Blender meters (inverse of :func:`translation_to_ue`)."""
    x, y, z = _vector(translation_cm, 3, "translation_cm")
    return tuple(narrow_float32(component) for component in (
        x / 100.0, -y / 100.0, z / 100.0))


def quat_to_ue(quat_xyzw):
    """Blender quaternion -> canonical-sign UE ``FQuat``.

    The R1 parity mapping changes the signs of X and Z while preserving Y/W.
    """
    x, y, z, w = _canonical_quaternion(quat_xyzw)
    return _canonical_quaternion((-x, y, -z, w))


def quat_from_ue(quat_xyzw):
    """UE ``FQuat`` -> Blender quaternion; the basis map is an involution."""
    x, y, z, w = _canonical_quaternion(quat_xyzw)
    return _canonical_quaternion((-x, y, -z, w))


def validate_scale(scale):
    """Reject non-finite/zero scale; representable reflections are valid."""
    components = _vector(scale, 3, "scale")
    if any(component == 0.0 for component in components):
        raise ValueError(
            "MH_E_INVALID_SCALE: composite scale components must be non-zero")
    return components


def _float32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", float(value)))[0]


def matrix_reconstructs_as_float32_trs(matrix, reconstructed) -> bool:
    """Apply the owner-frozen 8-ULP representability predicate to 4x4 matrices."""
    try:
        for row in range(4):
            for column in range(4):
                source = _float32(matrix[row][column])
                restored = _float32(reconstructed[row][column])
                if not math.isfinite(source) or not math.isfinite(restored):
                    return False
                magnitude = max(1.0, abs(source), abs(restored))
                tolerance = 8.0 * (2.0 ** -23) * magnitude
                if abs(source - restored) > tolerance:
                    return False
    except (IndexError, OverflowError, struct.error, TypeError):
        return False
    return True


def scale_to_ue(scale):
    """Blender local scale -> UE local scale (R1 passthrough)."""
    return validate_scale(scale)


def scale_from_ue(scale):
    """UE local scale -> Blender local scale."""
    return validate_scale(scale)


def blender_to_ue_transform(
        translation_m, rotation_xyzw, scale) -> CompositeTransform:
    """Build one canonical protocol transform from Blender local components."""
    return CompositeTransform(
        translation_cm=translation_to_ue(translation_m),
        rotation_quat=quat_to_ue(rotation_xyzw),
        scale=scale_to_ue(scale),
    )


def ue_to_blender_transform(transform: CompositeTransform):
    """Return Blender ``(translation_m, rotation_xyzw, scale)`` components."""
    if not isinstance(transform, CompositeTransform):
        raise TypeError("transform must be CompositeTransform")
    return (
        translation_from_ue(transform.translation_cm),
        quat_from_ue(transform.rotation_quat),
        scale_from_ue(transform.scale),
    )
