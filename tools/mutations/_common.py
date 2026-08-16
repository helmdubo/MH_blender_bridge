"""Shared helpers for golden-scene mutation scripts.

Each mutation script loads golden/golden.blend, applies exactly one
authoring operation, and saves golden/mutations/<name>.blend. Mutations
model the CORRECT end state — where the future addon would assign fresh
UIDs (linked duplicate, make single user), the script assigns them itself
from the reserved entries in tools/golden_uids.py. Broken states
(duplicate UIDs, cycles) are stage-B negative tests, not stage-A
mutations (see docs/QUESTIONS.md QUESTION-7).
"""

import os
import sys

import bpy

TOOLS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TOOLS_DIR)

from golden_uids import UIDS  # noqa: E402

REPO_ROOT = os.path.dirname(TOOLS_DIR)
GOLDEN_PATH = os.path.join(REPO_ROOT, "golden", "golden.blend")
MUTATIONS_DIR = os.path.join(REPO_ROOT, "golden", "mutations")


def open_golden():
    if not os.path.exists(GOLDEN_PATH):
        raise SystemExit(
            f"{GOLDEN_PATH} not found — run tools/make_golden_scene.py first")
    bpy.ops.wm.open_mainfile(filepath=GOLDEN_PATH)


def save_variant(name):
    os.makedirs(MUTATIONS_DIR, exist_ok=True)
    path = os.path.join(MUTATIONS_DIR, f"{name}.blend")
    bpy.ops.wm.save_as_mainfile(filepath=path, compress=False)
    print(f"mutation '{name}' written: {path}")
    return path


def object_by_uid(uid):
    """Mutations target datablocks by UID, never by name — the identity
    principle applies to the test tooling too."""
    for obj in bpy.data.objects:
        if obj.get("mh_uid") == uid:
            return obj
    raise KeyError(f"no object with mh_uid={uid}")


def collection_by_uid(uid):
    for col in bpy.data.collections:
        if col.get("mh_uid") == uid:
            return col
    raise KeyError(f"no collection with mh_uid={uid}")
