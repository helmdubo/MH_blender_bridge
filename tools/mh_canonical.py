"""Shim: the canonical-form library moved into the addon package (stage B1).

Real implementation: addon/mh4blend/core/canonical.py. This module keeps
`from mh_canonical import ...` working for tools/ and tests/ run from the
repo root. Import is bpy-free by design (mh4blend.core never touches bpy).
"""

import os
import sys

_ADDON_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "addon")
if _ADDON_DIR not in sys.path:
    sys.path.insert(0, _ADDON_DIR)

import mh4blend.core.canonical as _canonical  # noqa: E402

globals().update(
    {name: getattr(_canonical, name)
     for name in dir(_canonical) if not name.startswith("_")})
