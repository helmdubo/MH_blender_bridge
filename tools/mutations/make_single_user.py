"""Mutation: make a placement's resource single-user.

The collection-instance analog of Make Single User: window_3 gets its
own unique copy of the window_a resource. A new GEOMETRY collection
"window_a_unique" is created (copied object + copied mesh datablock),
and window_3 is retargeted to it.

Blender's copy carries the ORIGINAL UIDs on the copied datablocks
(collection/object/mesh) — exactly the §4.1 duplicate situation. The
script assigns the reserved fresh UIDs, modeling the outcome of the
manifest-based Fix arbitration (the untouched original keeps its UIDs).

Expected diff: resource CREATE (window_a_unique); node UPDATE_RESOURCE
on window_3 (same node UID now references a different resource_uid).
"""

import bpy

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import _common
from golden_uids import UIDS

_common.open_golden()

src_col = _common.collection_by_uid(UIDS["col/window_a"])
src_obj = src_col.objects[0]

new_col = bpy.data.collections.new("window_a_unique")
new_col["mh_uid"] = UIDS["col/window_a_unique"]
new_obj = src_obj.copy()
new_obj.data = src_obj.data.copy()
# Copies inherit the original UIDs — reassign per §4.1 Fix semantics.
assert new_obj["mh_uid"] == src_obj["mh_uid"]
new_obj["mh_uid"] = UIDS["obj/window_a_unique"]
new_obj.data["mh_uid"] = UIDS["mesh/window_a_unique"]
new_obj.name = "window_a_unique"
new_col.objects.link(new_obj)
bpy.data.scenes["GEOMETRY"].collection.children.link(new_col)

window_3 = _common.object_by_uid(UIDS["node/ca_windowset/window_3"])
window_3.instance_collection = new_col

_common.save_variant("make_single_user")
