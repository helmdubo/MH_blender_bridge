"""Deterministic UID table for the golden scene.

Production UID assignment is lazy UUID4 (docs/01_bundle_schema_v1.md §4).
The golden scene must be reproducible from this script alone, so it uses
UUID5 over a fixed namespace instead — a TEST-ONLY mechanism. Expected
diffs (golden/expected_diffs/*.json) reference these exact UIDs, and the
stage-B/C tests compare against them literally.

Pure stdlib module: importable both under Blender and under plain pytest.
"""

import uuid

# Fixed namespace for the golden scene. Never change: every UID below and
# every expected_diffs file derives from it.
GOLDEN_NAMESPACE = uuid.UUID("8f2f9d1e-6a5b-4c7d-9e3a-2b1c0d4e5f6a")


def golden_uid(path: str) -> str:
    """UUID5 for a logical path inside the golden scene, lowercase with dashes."""
    return str(uuid.uuid5(GOLDEN_NAMESPACE, path))


# Logical path -> role comment. Keys are stable identifiers; renames of
# Blender datablocks never touch them (that is the point).
_PATHS = (
    # GEOMETRY: collection (public resource_uid), object, mesh datablock
    "col/wall_a", "obj/wall_a", "mesh/wall_a",
    "col/wall_b", "obj/wall_b", "mesh/wall_b",
    "col/window_a", "obj/window_a", "mesh/window_a",
    "col/decal_leak", "obj/decal_leak", "mesh/decal_leak",
    # COMPOSITS: composite collections (CompositeDefinitionUID)
    "col/ca_windowset",
    "col/ca_building",
    # CA_WindowSet placement nodes
    "node/ca_windowset/window_1",
    "node/ca_windowset/window_2",
    "node/ca_windowset/window_3",
    # CA_Building placement nodes
    "node/ca_building/wall_main",
    "node/ca_building/wall_side",
    "node/ca_building/leak_decal",
    "node/ca_building/windows_front",
    "node/ca_building/windows_back",
    "node/ca_building/props_group",
    "node/ca_building/gp_wall",
    "node/ca_building/gp_decal",
    # Reserved for mutations (assigned by mutation scripts, not by the
    # golden scene itself):
    #   linked_duplicate: 4th window placement in CA_WindowSet
    "node/ca_windowset/window_4",
    #   make_single_user: unique copy of the window_a resource
    "col/window_a_unique", "obj/window_a_unique", "mesh/window_a_unique",
)

UIDS = {path: golden_uid(path) for path in _PATHS}


def find_by_uid(uid: str) -> str:
    """Reverse lookup (diagnostics in mutation scripts and tests)."""
    for path, value in UIDS.items():
        if value == uid:
            return path
    raise KeyError(uid)


if __name__ == "__main__":
    for path in _PATHS:
        print(f"{UIDS[path]}  {path}")
