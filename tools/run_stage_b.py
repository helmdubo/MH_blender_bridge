"""Stage-B acceptance runner (B13, 02_mvp_plan.md §2 приёмка).

    python3 tools/run_stage_b.py

1. Exports golden.blend, then re-exports into the same bundle: the second
   pass must write nothing (hash-skip) and diff empty (the UNCHANGED
   acceptance test).
2. Exports every positive mutation and compares
   diff_bundles(golden, mutation) against golden/expected_diffs/<name>.json
   literally.
3. Runs the negative scenes: export must be blocked and the validation
   (code, subjects) rows must equal golden/expected_errors/<name>.json.

Requires the pip bpy module (or run under blender -b -P). Exit code 0 only
when every check passes.
"""

import json
import os
import shutil
import sys

import bpy

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(TOOLS_DIR)
sys.path.insert(0, TOOLS_DIR)
sys.path.insert(0, os.path.join(REPO_ROOT, "addon"))

from diff_bundles import load_bundle  # noqa: E402
from mh4blend.core.diff import diff_bundles  # noqa: E402
from mh4blend.scene.export_bundle import export_bundle  # noqa: E402

GOLDEN_BLEND = os.path.join(REPO_ROOT, "golden", "golden.blend")
MUTATIONS_DIR = os.path.join(REPO_ROOT, "golden", "mutations")
EXPECTED_DIFFS = os.path.join(REPO_ROOT, "golden", "expected_diffs")
EXPECTED_ERRORS = os.path.join(REPO_ROOT, "golden", "expected_errors")
BUILD_DIR = os.path.join(REPO_ROOT, "golden", "build", "stage_b")

POSITIVE = ("rename_object", "rename_collection", "linked_duplicate",
            "make_single_user", "delete_node", "edit_geometry",
            "reparent_node")
NEGATIVE = ("duplicate_uid", "parent_uid_dangling")

failures = []


def check(name, ok, detail=""):
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))
    if not ok:
        failures.append((name, detail))


def export_blend_to(blend_path, bundle_dir):
    bpy.ops.wm.open_mainfile(filepath=blend_path)
    return export_bundle(bundle_dir)


def main():
    if os.path.exists(BUILD_DIR):
        shutil.rmtree(BUILD_DIR)

    print("== golden export + UNCHANGED acceptance ==")
    golden_dir = os.path.join(BUILD_DIR, "golden")
    report = export_blend_to(GOLDEN_BLEND, golden_dir)
    check("golden export ok", report["ok"], json.dumps(report["validation"]["errors"]))
    check("golden wrote everything",
          len(report["written"]) == 6 and not report["skipped"],
          f"written={report['written']}")
    report2 = export_blend_to(GOLDEN_BLEND, golden_dir)
    check("re-export writes nothing (hash-skip)",
          report2["ok"] and not report2["written"] and len(report2["skipped"]) == 6,
          f"written={report2['written']} skipped={report2['skipped']}")
    old_manifest, old_composites = load_bundle(golden_dir)
    self_diff = diff_bundles(old_manifest, old_manifest,
                             old_composites, old_composites)
    check("self-diff is empty",
          self_diff["resources"] == {} and self_diff["nodes"] == {})

    print("== positive mutations vs expected_diffs ==")
    for name in POSITIVE:
        out_dir = os.path.join(BUILD_DIR, name)
        report = export_blend_to(
            os.path.join(MUTATIONS_DIR, f"{name}.blend"), out_dir)
        if not report["ok"]:
            check(name, False,
                  f"export blocked: {report['validation']['errors']}")
            continue
        new_manifest, new_composites = load_bundle(out_dir)
        got = diff_bundles(old_manifest, new_manifest,
                           old_composites, new_composites)
        with open(os.path.join(EXPECTED_DIFFS, f"{name}.json")) as f:
            expected = json.load(f)
        ok = (got["resources"] == expected["resources"]
              and got["nodes"] == expected["nodes"])
        detail = "" if ok else (
            f"got {json.dumps(got, ensure_ascii=False)} "
            f"expected {json.dumps(expected, ensure_ascii=False)}")
        check(name, ok, detail)

    print("== negative scenes vs expected_errors ==")
    for name in NEGATIVE:
        out_dir = os.path.join(BUILD_DIR, name)
        report = export_blend_to(
            os.path.join(MUTATIONS_DIR, f"{name}.blend"), out_dir)
        with open(os.path.join(EXPECTED_ERRORS, f"{name}.json")) as f:
            expected = json.load(f)
        got_rows = [{"code": e["code"], "subjects": e["subjects"]}
                    for e in report["validation"]["errors"]]
        want_rows = [{"code": e["code"], "subjects": e["subjects"]}
                     for e in expected["errors"]]
        ok = (not report["ok"]) and got_rows == want_rows
        check(name, ok,
              "" if ok else f"got {got_rows} expected {want_rows}, "
                            f"ok={report['ok']}")
        check(f"{name}: nothing written",
              not os.path.exists(os.path.join(out_dir, "export_manifest.json")))

    print()
    if failures:
        print(f"STAGE B ACCEPTANCE: FAILED ({len(failures)} checks)")
        sys.exit(1)
    print("STAGE B ACCEPTANCE: ALL CHECKS PASSED")


if __name__ == "__main__":
    main()
