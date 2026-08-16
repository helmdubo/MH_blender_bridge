"""Mutation: move a node to another parent.

leak_decal (root-level child of CA_Building) is reparented under
props_group, preserving its WORLD transform — the standard authoring
expectation. props_group has a non-identity transform, so the node's
local transform relative to its new parent necessarily changes.

matrix_parent_inverse is kept identity and the local basis is set
explicitly: the exported local_transform is always "relative to
parent", and authoring state should match that with no hidden offset.

Expected diff: node {UPDATE_TRANSFORM, REPARENT} — the orthogonal-flags
contract (§7.2) in action; resources unchanged.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import _common
from golden_uids import UIDS

_common.open_golden()
child = _common.object_by_uid(UIDS["node/ca_building/leak_decal"])
parent = _common.object_by_uid(UIDS["node/ca_building/props_group"])

world = child.matrix_world.copy()
child.parent = parent
child.matrix_parent_inverse.identity()
child.matrix_basis = parent.matrix_world.inverted() @ world

_common.save_variant("reparent_node")
