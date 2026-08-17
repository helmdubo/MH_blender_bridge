"""Blender -> UE transform conversion (schema §11).

Formula status: hypothesis until the R1 golden axis test passes in UE
(docs/RISK_RESULTS.md); the schema fixes the data format, this module fixes
the math. Pure Python — no bpy/mathutils: quaternions are plain (x, y, z, w)
sequences (NOTE: mathutils.Quaternion stores (w, x, y, z) — callers convert).

    pos_UE   = ( x * 100,  -y * 100,  z * 100 )     # meters -> centimeters
    quat_UE  = ( -qx,  qy,  -qz,  qw )
    scale_UE = ( sx,  sy,  sz )

Outputs are quantized integers (§8.2) — the canonical currency of the
pipeline; the .composite writer renders them as decimals q / 10^p.
"""

import math

from .canonical import (
    P_SCALE,
    P_TRANSLATION_CM,
    canonicalize_quat,
    quantize,
)

__all__ = [
    "translation_to_ue",
    "quat_to_ue",
    "scale_to_ue",
    "validate_scale",
]


def translation_to_ue(translation_m):
    """Blender world/local translation (meters, (x, y, z)) -> quantized UE cm."""
    x, y, z = translation_m
    return (
        quantize(x * 100.0, P_TRANSLATION_CM),
        quantize(-y * 100.0, P_TRANSLATION_CM),
        quantize(z * 100.0, P_TRANSLATION_CM),
    )


def quat_to_ue(quat_xyzw):
    """Blender rotation quaternion (x, y, z, w) -> canonical quantized UE quat.

    Applies the §11 component mapping, then the full §11 pipeline
    (normalize -> quantize p=6 -> sign-canonicalize on quantized ints).
    """
    x, y, z, w = quat_xyzw
    return canonicalize_quat((-x, y, -z, w))


def validate_scale(scale):
    """§11: scale <= 0 on any axis is a validation error (MH_E_INVALID_SCALE)."""
    for component in scale:
        if isinstance(component, bool) or not isinstance(component, (int, float)):
            raise ValueError(f"MH_E_INVALID_SCALE: non-numeric scale {scale!r}")
        f = float(component)
        if math.isnan(f) or math.isinf(f):
            raise ValueError(f"MH_E_NAN_INF_VALUE: scale {scale!r}")
        if f <= 0.0:
            raise ValueError(
                "MH_E_INVALID_SCALE: mirror the geometry, not the placement "
                f"(scale {scale!r})")


def scale_to_ue(scale):
    """Blender scale (sx, sy, sz) -> quantized UE scale (validated)."""
    validate_scale(scale)
    return tuple(quantize(float(c), P_SCALE) for c in scale)
