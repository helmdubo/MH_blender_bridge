"""Export Sources / Validate operators (B11).

Log pattern per dag4blend: full machine report goes into the text block
`mh_export_log` (Text Editor), the operator report line carries only the
summary. The export directory is source_root/<scene subdir> (D27); the
subdir is a per-file Scene property defaulting to '<blend name>'.
"""

import json
import os

import bpy

from .. import prefs as prefs_mod
from ..scene.export_bundle import export_bundle
from ..scene.material_extract import remap_dagormat_texture_paths

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
    subdir = context.scene.mh_bundle_subdir or stem
    if os.path.isabs(subdir):
        return None, "Export Subdir must be relative to Source Root"
    source_root = os.path.realpath(os.path.abspath(
        bpy.path.abspath(prefs.source_root)))
    target = os.path.realpath(os.path.abspath(os.path.join(source_root, subdir)))
    try:
        inside_root = os.path.commonpath(
            [os.path.normcase(source_root), os.path.normcase(target)]) \
            == os.path.normcase(source_root)
    except ValueError:  # different drive letters on Windows
        inside_root = False
    if not inside_root or target == source_root:
        return None, "Export Subdir must stay below Source Root"
    return target, None


class MH_OT_export_bundle(bpy.types.Operator):
    bl_idname = "mh.export_bundle"
    bl_label = "Export Sources"
    bl_description = "Export GEOMETRY + COMPOSITS scenes as versioned source files"

    def execute(self, context):
        bundle_dir, problem = _bundle_dir(context)
        if problem:
            self.report({"ERROR"}, problem)
            return {"CANCELLED"}
        prefs = prefs_mod.get_prefs(context)
        report = export_bundle(
            bundle_dir,
            texture_root=bpy.path.abspath(prefs.texture_root),
            registry_path=bpy.path.abspath(prefs.registry_path),
        )
        _log(report)
        if not report["ok"]:
            first = report["validation"]["errors"][0]
            self.report(
                {"ERROR"},
                f"{first['code']} (+{len(report['validation']['errors']) - 1} "
                f"more) — see '{LOG_TEXT_NAME}' text block")
            return {"CANCELLED"}
        warning_count = len(report["validation"].get("warnings", []))
        warning_suffix = (
            f", {warning_count} warning(s) — see '{LOG_TEXT_NAME}'"
            if warning_count else "")
        manifest_state = (
            "updated" if report["manifest_changed"]
            else "refreshed" if report["manifest_written"]
            else "unchanged")
        self.report(
            {"INFO"},
            f"Export OK: {len(report['written'])} written, "
            f"{len(report['skipped'])} unchanged, "
            f"{len(report['deleted'])} deleted, manifest {manifest_state}"
            f"{warning_suffix} -> {bundle_dir}")
        return {"FINISHED"}


class MH_OT_validate(bpy.types.Operator):
    bl_idname = "mh.validate"
    bl_label = "Validate"
    bl_description = "Run export validation without writing anything"

    def execute(self, context):
        prefs = prefs_mod.get_prefs(context)
        report = export_bundle(
            "",
            dry_run=True,
            texture_root=bpy.path.abspath(prefs.texture_root),
            registry_path=bpy.path.abspath(prefs.registry_path),
        )
        _log(report)
        errors = report["validation"]["errors"]
        if errors:
            self.report(
                {"ERROR"},
                f"{len(errors)} problem(s), first: {errors[0]['code']} — "
                f"see '{LOG_TEXT_NAME}' text block")
            return {"CANCELLED"}
        warnings = report["validation"].get("warnings", [])
        if warnings:
            self.report(
                {"WARNING"},
                f"Validation clean with {len(warnings)} warning(s) — "
                f"see '{LOG_TEXT_NAME}' text block")
        else:
            self.report({"INFO"}, "Validation clean")
        return {"FINISHED"}


class MH_OT_remap_texture_root(bpy.types.Operator):
    bl_idname = "mh.remap_texture_root"
    bl_label = "Remap Old Texture Root"
    bl_description = (
        "One-shot rewrite of absolute dagormat texture paths from Old "
        "Texture Root to Texture Root; paths outside the old root are kept")
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        prefs = prefs_mod.get_prefs(context)
        if not prefs.legacy_texture_root:
            self.report({"ERROR"}, "Set Old Texture Root in addon preferences")
            return {"CANCELLED"}
        if not prefs.texture_root:
            self.report({"ERROR"}, "Set Texture Root in addon preferences")
            return {"CANCELLED"}

        old_root = bpy.path.abspath(prefs.legacy_texture_root)
        new_root = bpy.path.abspath(prefs.texture_root)
        try:
            counts = remap_dagormat_texture_paths(old_root, new_root)
        except ValueError as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}

        report = {
            "schema": "mh.texture_remap_report",
            "schema_version": 1,
            "old_root": old_root,
            "new_root": new_root,
            **counts,
        }
        _log(report)
        self.report(
            {"INFO"},
            f"Remap complete: {counts['texture_paths_rewritten']} path(s) "
            f"in {counts['materials_changed']} material(s), "
            f"{counts['materials_skipped_read_only']} read-only skipped — see "
            f"'{LOG_TEXT_NAME}'")
        return {"FINISHED"}

    def invoke(self, context, _event):
        return context.window_manager.invoke_confirm(self, _event)


CLASSES = (MH_OT_export_bundle, MH_OT_validate, MH_OT_remap_texture_root)


def register():
    bpy.types.Scene.mh_bundle_subdir = bpy.props.StringProperty(
        name="Export Subdir",
        description="Export directory under Source Root (D27); empty = "
                    "'<blend name>'",
        default="",
    )
    for cls in CLASSES:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
    del bpy.types.Scene.mh_bundle_subdir
