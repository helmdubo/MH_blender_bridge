"""Mutation: delete a placement node.

leak_decal (Empty in CA_Building) is removed. The decal_leak resource
stays in GEOMETRY and stays exported.

Expected diff: node REMOVE only; resources unchanged (an unreferenced
resource is still a bundle resource, orphan review is a log concern,
not a diff op — 02_mvp_plan.md §0).
"""

import bpy

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import _common
from golden_uids import UIDS

_common.open_golden()
obj = _common.object_by_uid(UIDS["node/ca_building/leak_decal"])
bpy.data.objects.remove(obj)
_common.save_variant("delete_node")
