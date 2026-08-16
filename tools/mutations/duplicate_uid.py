"""NEGATIVE mutation: real Ctrl+D — duplicate node UID left in place.

window_1 is copied and the copy KEEPS the copied mh_uid, exactly what
Blender does on Ctrl+D before the addon interferes. Unlike
linked_duplicate.py, no fresh UID is assigned: this scene must FAIL
export validation.

Expected error (golden/expected_errors/duplicate_uid.json):
MH_E_DUPLICATE_NODE_UID with the shared UID as subject.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import _common
from golden_uids import UIDS

_common.open_golden()
src = _common.object_by_uid(UIDS["node/ca_windowset/window_1"])
dup = src.copy()
assert dup["mh_uid"] == src["mh_uid"], "the duplicate must keep the UID"
dup.location = (0.0, 0.0, 2.5)
for col in src.users_collection:
    col.objects.link(dup)
_common.save_variant("duplicate_uid")
