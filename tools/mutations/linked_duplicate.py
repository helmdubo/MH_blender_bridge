"""Mutation: linked duplicate of a placement (Alt+D analog).

window_3 is copied: same instance_collection (shared resource), new
node UID. Blender copies custom props on duplication, so the copy is
born with a DUPLICATE mh_uid — the script immediately assigns the
reserved fresh UID, modeling what the addon's duplicate handling / Fix
will do (schema §4, §4.1).

Expected diff: one node CREATE in CA_WindowSet; resources unchanged
(same resource_uid is referenced by one more node).
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import _common
from golden_uids import UIDS

_common.open_golden()
src = _common.object_by_uid(UIDS["node/ca_windowset/window_3"])
dup = src.copy()  # Empty: no object data to copy
assert dup["mh_uid"] == src["mh_uid"], "expected Blender to copy custom props"
dup["mh_uid"] = UIDS["node/ca_windowset/window_4"]
dup.location = (4.5, 0.2, 1.1)
for col in src.users_collection:
    col.objects.link(dup)
_common.save_variant("linked_duplicate")
