"""NEGATIVE mutation: node parented outside its composite's node table.

leak_decal (node of CA_Building) is parented to a new Empty that is NOT
a member of the CA_Building collection (it lives directly in the
COMPOSITS scene collection and carries no mh_uid). The exported
CA_Building node table would contain a parent reference that resolves
to no node in the table — the dangling-parent case of §4.1/§6, which
normally arises from hand-edited .composite files but must be caught at
export too. This scene must FAIL export validation.

Expected error (golden/expected_errors/parent_uid_dangling.json):
MH_E_DANGLING_PARENT with the orphaned child node UID as subject.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bpy

import _common
from golden_uids import UIDS

_common.open_golden()
child = _common.object_by_uid(UIDS["node/ca_building/leak_decal"])

outsider = bpy.data.objects.new("outsider", None)
bpy.data.scenes["COMPOSITS"].collection.objects.link(outsider)

world = child.matrix_world.copy()
child.parent = outsider
child.matrix_parent_inverse.identity()
child.matrix_basis = world

_common.save_variant("parent_uid_dangling")
