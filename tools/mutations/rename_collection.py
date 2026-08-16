"""Mutation: rename a mesh resource collection.

wall_a (GEOMETRY collection) -> "wall_a_main". UID untouched.
Expected diff: resource RENAME only. The bundle filename changes
(wall_a__3f53abc1 -> wall_a_main__3f53abc1) but the UID does not,
so this must NOT read as REMOVE+CREATE.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import _common
from golden_uids import UIDS

_common.open_golden()
col = _common.collection_by_uid(UIDS["col/wall_a"])
col.name = "wall_a_main"
_common.save_variant("rename_collection")
