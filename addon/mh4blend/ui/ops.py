"""Export Bundle / Validate operators (B11).

Log pattern per dag4blend: full machine report goes into the text block
`mh_export_log` (Text Editor), the operator report line carries only the
summary. The bundle directory is source_root/<scene subdir> (D27); the
subdir is a per-file Scene property defaulting to '<blend name>.bundle'.
"""

import json
import os

import bpy

from .. import prefs as prefs_mod
from ..scene.export_bundle import export_bundle

LOG_TEXT_NAME = "mh_export_log"


def _log(report):
    text = bpy.data.texts.get(LOG_TEXT_NAME)
    if text is None:
        text = bpy.data.texts.new(LOG_TEXT_NAME)
    text.write(json.dumps(report, indent=2, ensure_ascii=False) + "\n")


def _bundle_dir(context):
    prefs = prefs_mod.get_prefs(context)
    if not prefs.source_root:
        return None, "Set Source Root in the addon preferences first"
    stem = os.path.splitext(os.path.basename(bpy.data.filepath))[0] or "untitled"
    subdir = context.scene.mh_bundle_subdir or f"{stem}.bundle"
    return os.path.join(bpy.path.abspath(prefs.source_root), subdir), None


class MH_OT_export_bundle(bpy.types.Operator):
    bl_idname = "mh.export_bundle"
    bl_label = "Export Bundle"
    bl_description = "Export GEOMETRY + COMPOSITS scenes as a source bundle"

    def execute(self, context):
        bundle_dir, problem = _bundle_dir(context)
        if problem:
            self.report({"ERROR"}, problem)
            return {"CANCELLED"}
        report = export_bundle(bundle_dir)
        _log(report)
        if not report["ok"]:
            first = report["validation"]["errors"][0]
            self.report(
                {"ERROR"},
                f"{first['code']} (+{len(report['validation']['errors']) - 1} "
                f"more) — see '{LOG_TEXT_NAME}' text block")
            return {"CANCELLED"}
        self.report(
            {"INFO"},
            f"Bundle OK: {len(report['written'])} written, "
            f"{len(report['skipped'])} unchanged, "
            f"{len(report['deleted'])} deleted -> {bundle_dir}")
        return {"FINISHED"}


class MH_OT_validate(bpy.types.Operator):
    bl_idname = "mh.validate"
    bl_label = "Validate"
    bl_description = "Run export validation without writing anything"

    def execute(self, context):
        report = export_bundle("", dry_run=True)
        _log(report)
        errors = report["validation"]["errors"]
        if errors:
            self.report(
                {"ERROR"},
                f"{len(errors)} problem(s), first: {errors[0]['code']} — "
                f"see '{LOG_TEXT_NAME}' text block")
            return {"CANCELLED"}
        self.report({"INFO"}, "Validation clean")
        return {"FINISHED"}


CLASSES = (MH_OT_export_bundle, MH_OT_validate)


def register():
    bpy.types.Scene.mh_bundle_subdir = bpy.props.StringProperty(
        name="Bundle Subdir",
        description="Bundle directory under Source Root (D27); empty = "
                    "'<blend name>.bundle'",
        default="",
    )
    for cls in CLASSES:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
    del bpy.types.Scene.mh_bundle_subdir
