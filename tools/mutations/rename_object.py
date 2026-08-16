"""Mutation: rename a placement object.

window_2 (Empty in CA_WindowSet) -> "window_2_left". UID untouched.
Expected diff: node RENAME only (display_name change); no resource ops.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import _common
from golden_uids import UIDS

_common.open_golden()
obj = _common.object_by_uid(UIDS["node/ca_windowset/window_2"])
obj.name = "window_2_left"
_common.save_variant("rename_object")
