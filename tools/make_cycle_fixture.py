"""Generate the composite-cycle negative fixture (bundle-level, no Blender).

Why not a .blend mutation like the others: Blender cannot persist a true
instance_collection cycle inside one file — the dependency graph crashes
on it (verified on 4.5, both with instance_type COLLECTION and NONE), and
the UI forbids creating one. Real cycles arrive exactly like this fixture:
through cross-bundle references or hand-edited .composite files. The
UE-Analyzer's topological sort (schema §6, D10) is tested on precisely
this kind of data.

Writes golden/fixtures/composite_cycle/{cycle_a__*,cycle_b__*}.composite —
two minimal, schema-valid composites whose composite_ref nodes reference
each other. Expected error: MH_E_COMPOSITE_CYCLE (see
golden/expected_errors/composite_cycle.json).

Files follow the on-disk format of §8.1/§8.4: 2-space indent, fixed key
order, nodes sorted by node_uid, LF, trailing newline, quantized values.

Usage: python3 tools/make_cycle_fixture.py   (pure stdlib, no bpy)
"""

import json
import os
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS_DIR)

from golden_uids import UIDS  # noqa: E402
from mh_canonical import resource_filename  # noqa: E402

REPO_ROOT = os.path.dirname(TOOLS_DIR)
FIXTURE_DIR = os.path.join(REPO_ROOT, "golden", "fixtures", "composite_cycle")

IDENTITY_TRANSFORM = {
    "translation_cm": [0.0, 0.0, 0.0],
    "rotation_quat": [0.0, 0.0, 0.0, 1.0],
    "scale": [1.0, 1.0, 1.0],
}


def composite_doc(uid_key, name, node_key, ref_display, ref_uid_key):
    return {
        "schema": "mh.composite",
        "schema_version": 1,
        "uid": UIDS[uid_key],
        "name": name,
        "nodes": [
            {
                "node_uid": UIDS[node_key],
                "parent_uid": None,
                "kind": "composite_ref",
                "display_name": ref_display,
                "resource_uid": UIDS[ref_uid_key],
                "local_transform": IDENTITY_TRANSFORM,
                "properties": {},
            },
        ],
    }


def main():
    os.makedirs(FIXTURE_DIR, exist_ok=True)
    docs = [
        ("cycle_a", composite_doc(
            "col/cycle_a", "cycle_a", "node/cycle_a/ref_b", "ref_b", "col/cycle_b")),
        ("cycle_b", composite_doc(
            "col/cycle_b", "cycle_b", "node/cycle_b/ref_a", "ref_a", "col/cycle_a")),
    ]
    for name, doc in docs:
        path = os.path.join(
            FIXTURE_DIR, resource_filename(name, doc["uid"], ".composite"))
        with open(path, "w", newline="\n") as f:
            json.dump(doc, f, indent=2, ensure_ascii=False)
            f.write("\n")
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
