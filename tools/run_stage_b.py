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

Requires the pip bpy module (or run under
``blender -b --python-exit-code 1 -P tools/run_stage_b.py``). Exit code 0
only when every check passes.
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
from golden_uids import UIDS  # noqa: E402
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


class _AcceptanceTextures(bpy.types.PropertyGroup):
    """Minimal dagormat-shaped RNA used only by this headless acceptance."""
    tex0: bpy.props.StringProperty(default="")


class _AcceptanceOptional(bpy.types.PropertyGroup):
    pass


class _AcceptanceDagormat(bpy.types.PropertyGroup):
    shader_class: bpy.props.StringProperty(default="")
    sides: bpy.props.IntProperty(default=0)
    optional: bpy.props.PointerProperty(type=_AcceptanceOptional)
    textures: bpy.props.PointerProperty(type=_AcceptanceTextures)


def ensure_acceptance_dagormat_rna():
    """Register a tiny compatible PropertyGroup when dag4blend is absent.

    Production reads dag4blend's real Material.dagormat.  The golden runner
    stays self-contained and tests the same public shape without requiring
    that separate addon to be installed on CI.
    """
    if hasattr(bpy.types.Material, "dagormat"):
        return
    for cls in (_AcceptanceTextures, _AcceptanceOptional,
                _AcceptanceDagormat):
        bpy.utils.register_class(cls)
    bpy.types.Material.dagormat = bpy.props.PointerProperty(
        type=_AcceptanceDagormat)


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
          len(report["written"]) == 6 and not report["skipped"]
          and report["manifest_changed"] and report["manifest_written"],
          f"written={report['written']}")
    # Re-export the already-open authoring scene. Reopening an unsaved fixture
    # would intentionally discard lazily assigned production UIDs and is not
    # the owner workflow being accepted here.
    report2 = export_bundle(golden_dir)
    check("re-export writes nothing (hash-skip)",
          report2["ok"] and not report2["written"]
          and len(report2["skipped"]) == 6
          and not report2["manifest_changed"]
          and not report2["manifest_written"],
          f"written={report2['written']} skipped={report2['skipped']} "
          f"manifest_changed={report2['manifest_changed']}")
    old_manifest, old_composites = load_bundle(golden_dir)
    materials = {entry["uid"]: entry
                 for entry in old_manifest.get("materials", [])}
    check("golden exports two material resources",
          set(materials) == {
              UIDS["material/wall_shared"], UIDS["material/accent"]},
          f"materials={list(materials)}")
    check("plain materials use rendinst_simple fallback",
          all(entry["shader_class"] == "rendinst_simple"
              and entry["params"] == {}
              and entry["textures"] == {}
              and entry["content_hash"].startswith("xxh3:")
              for entry in materials.values()),
          f"materials={list(materials.values())}")
    resources = {entry["uid"]: entry for entry in old_manifest["resources"]}
    check("material slots preserve order and shared identity",
          resources[UIDS["col/wall_a"]]["material_slots"] == [
              {"slot_name": "m_wall_shared",
               "material_uid": UIDS["material/wall_shared"]},
              {"slot_name": "m_accent",
               "material_uid": UIDS["material/accent"]},
          ] and resources[UIDS["col/wall_b"]]["material_slots"] == [
              {"slot_name": "m_wall_shared",
               "material_uid": UIDS["material/wall_shared"]},
          ],
          f"wall_a={resources[UIDS['col/wall_a']].get('material_slots')} "
          f"wall_b={resources[UIDS['col/wall_b']].get('material_slots')}")
    self_diff = diff_bundles(old_manifest, old_manifest,
                             old_composites, old_composites)
    check("self-diff is empty",
          self_diff["resources"] == {} and self_diff["nodes"] == {})

    # B4/D25 regression: changing only dagormat metadata in the already
    # exported scene must update the manifest/material diff but leave every
    # FBX and .composite payload hash-skipped in the same destination.
    ensure_acceptance_dagormat_rna()
    accent = next(material for material in bpy.data.materials
                  if material.get("mh_uid") == UIDS["material/accent"])
    accent.dagormat.shader_class = "rendinst_simple"
    accent.dagormat.optional["acceptance_probe"] = 0.25
    material_report = export_bundle(golden_dir)
    material_manifest, material_composites = load_bundle(golden_dir)
    material_diff = diff_bundles(
        old_manifest, material_manifest, old_composites, material_composites)
    old_accent = materials[UIDS["material/accent"]]
    new_accent = next(entry for entry in material_manifest["materials"]
                      if entry["uid"] == UIDS["material/accent"])
    check("material-only re-export skips all FBX/composite payloads",
          material_report["ok"] and not material_report["written"]
          and len(material_report["skipped"]) == 6
          and material_report["manifest_changed"]
          and material_report["manifest_written"],
          f"written={material_report['written']} "
          f"skipped={material_report['skipped']} "
          f"manifest_changed={material_report['manifest_changed']}")
    check("material-only edit changes its content_hash",
          old_accent["content_hash"] != new_accent["content_hash"],
          f"old={old_accent['content_hash']} new={new_accent['content_hash']}")
    check("material-only diff is UPDATE_PROPERTIES",
          material_diff["resources"] == {
              UIDS["material/accent"]: ["UPDATE_PROPERTIES"]}
          and material_diff["nodes"] == {},
          f"diff={json.dumps(material_diff, ensure_ascii=False)}")

    # Blender/FBX uses material names as slot identity.  A material rename is
    # therefore both a material RENAME and a geometry/material-slot update for
    # exactly the mesh resources that use it.  The shared wall-only material
    # and both composites must stay hash-skipped.
    accent.name = "m_accent_renamed"
    rename_report = export_bundle(golden_dir)
    renamed_manifest, renamed_composites = load_bundle(golden_dir)
    rename_diff = diff_bundles(
        material_manifest, renamed_manifest,
        material_composites, renamed_composites)
    expected_rename_resources = {
        UIDS["material/accent"]: ["RENAME"],
        UIDS["col/wall_a"]: ["UPDATE_GEOMETRY", "UPDATE_PROPERTIES"],
        UIDS["col/window_a"]: ["UPDATE_GEOMETRY", "UPDATE_PROPERTIES"],
        UIDS["col/decal_leak"]: ["UPDATE_GEOMETRY", "UPDATE_PROPERTIES"],
    }
    check("material rename rewrites exactly its three FBX users",
          rename_report["ok"] and len(rename_report["written"]) == 3
          and len(rename_report["skipped"]) == 3
          and rename_report["manifest_changed"]
          and rename_report["manifest_written"],
          f"written={rename_report['written']} "
          f"skipped={rename_report['skipped']}")
    check("material rename cascade is explicit in diff",
          rename_diff["resources"] == expected_rename_resources
          and rename_diff["nodes"] == {},
          f"diff={json.dumps(rename_diff, ensure_ascii=False)}")

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

    # Transport acceptance for a multi-object resource: the aggregate table
    # is useful only if the FBX itself carries the same per-object slot names.
    # Import the pristine first-pass FBX in a clean Blender scene and inspect
    # the actual imported material slots.
    print("== multi-object FBX material-slot round-trip ==")
    # The rename scenario rewrote this target, so restore the golden scene and
    # export once to a separate directory before importing it.
    transport_dir = os.path.join(BUILD_DIR, "transport")
    transport_report = export_blend_to(GOLDEN_BLEND, transport_dir)
    transport_manifest, _transport_composites = load_bundle(transport_dir)
    transport_resources = {
        entry["uid"]: entry for entry in transport_manifest["resources"]}
    pristine_wall_fbx = os.path.join(
        transport_dir, transport_resources[UIDS["col/wall_a"]]["source"])
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=pristine_wall_fbx)
    imported_slots = {
        obj.name: [slot.name for slot in obj.material_slots]
        for obj in bpy.context.scene.objects if obj.type == "MESH"
    }
    check("transport export succeeded", transport_report["ok"])
    check("multi-object FBX preserves per-object material slot names",
          imported_slots == {
              "wall_a": ["m_wall_shared", "m_accent"],
              "wall_a_trim": ["m_accent"],
          },
          f"imported_slots={imported_slots}")

    print()
    if failures:
        print(f"STAGE B ACCEPTANCE: FAILED ({len(failures)} checks)")
        sys.exit(1)
    print("STAGE B ACCEPTANCE: ALL CHECKS PASSED")


if __name__ == "__main__":
    main()
