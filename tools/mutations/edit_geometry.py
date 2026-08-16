"""Mutation: edit mesh geometry of a resource.

One vertex of the wall_a mesh datablock is moved (top +X+Y corner,
+0.25 m along Z). Deterministic, affects only vertex positions.

Expected diff: resource UPDATE_GEOMETRY only (mesh content_hash §9
changes); nodes untouched — placements reference the resource by UID
and know nothing about its contents.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import _common
from golden_uids import UIDS

_common.open_golden()
col = _common.collection_by_uid(UIDS["col/wall_a"])
mesh = col.objects[0].data
assert mesh["mh_uid"] == UIDS["mesh/wall_a"]
# Vertex 6 is the (+hx, +hy, sz) corner of the box (see make_box_mesh).
mesh.vertices[6].co.z += 0.25
_common.save_variant("edit_geometry")
