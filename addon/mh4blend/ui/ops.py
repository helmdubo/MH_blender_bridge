"""Standalone FBX and composite operators.

Each button acts only on the collection/file selected beside it. There is no
combined scene scan and no bundle export operator in the user interface.
"""

from __future__ import annotations

import json
import os
import re
import uuid

import bpy

from .. import prefs as prefs_mod
from ..core.canonical import validate_resource_name
from ..core.uid import ensure_uid
from ..scene.actualize_texture_paths import actualize_texture_paths
from ..scene.export_composite import export_composite_collection
from ..scene.export_fbx import export_fbx_collection
from ..scene.export_material import (
    prepare_blender_material_export,
    write_prepared_material,
)
from ..scene.import_composite import import_composite_file
from ..scene.migrate_sources_v2 import migrate_sources_v2


LOG_TEXT_NAME = "mh_export_log"
_LODS_ROOT_RE = re.compile(r"^(?P<base>.+)\.lods(?:\.\d{3})?$")


def _json_default(value):
    name = getattr(value, "name", None)
    if isinstance(name, str):
        return name
    disk_dict = getattr(value, "disk_dict", None)
    if callable(disk_dict):
        return disk_dict()
    return str(value)


def _log(operation, report):
    text = bpy.data.texts.get(LOG_TEXT_NAME)
    if text is None:
        text = bpy.data.texts.new(LOG_TEXT_NAME)
    row = {"operation": operation, "report": report}
    text.write(json.dumps(
        row, indent=2, ensure_ascii=False, default=_json_default) + "\n")


def _directory(value):
    if not isinstance(value, str) or not value.strip():
        raise ValueError("Choose an output folder")
    return os.path.abspath(bpy.path.abspath(value))


def _filepath(value):
    if not isinstance(value, str) or not value.strip():
        raise ValueError("Choose a .composite file")
    path = os.path.abspath(bpy.path.abspath(value))
    if not path.lower().endswith(".composite"):
        raise ValueError("Import path must point to a .composite file")
    if not os.path.isfile(path):
        raise ValueError(f"Composite file does not exist: {path}")
    return path


def _collision_resource(scene, kind):
    if kind == "FBX":
        return scene.mh_fbx_collection
    if kind == "COMPOSITE":
        return scene.mh_composite_export_collection
    if kind == "MATERIAL":
        return scene.mh_material
    return None


def _open_collision_dialog(context, kind, message):
    context.scene.mh_collision_kind = kind
    context.scene.mh_collision_message = message
    bpy.ops.mh.resolve_name_collision("INVOKE_DEFAULT")


def _collision_display_name(resource, kind):
    if resource is None:
        return ""
    if kind == "FBX":
        match = _LODS_ROOT_RE.fullmatch(resource.name)
        if match is not None:
            override = resource.get("name")
            if isinstance(override, str):
                try:
                    validate_resource_name(override)
                except (TypeError, ValueError):
                    pass
                else:
                    return override
            return match.group("base")
    return resource.name


class MH_OT_resolve_name_collision(bpy.types.Operator):
    bl_idname = "mh.resolve_name_collision"
    bl_label = "Resolve Source Name Collision"
    bl_description = "Resolve a clean filename already owned by another UID"

    action: bpy.props.EnumProperty(
        name="Action",
        items=(
            ("RENAME", "Rename mine", "Keep UID and choose another name"),
            ("FORK", "Fork existing as new resource",
             "Give the Blender resource a new UID and chosen name"),
            ("CANCEL", "Cancel", "Leave all data unchanged"),
        ),
        default="CANCEL",
    )
    new_name: bpy.props.StringProperty(name="New Resource Name", default="")

    def invoke(self, context, _event):
        resource = _collision_resource(
            context.scene, context.scene.mh_collision_kind)
        self.new_name = _collision_display_name(
            resource, context.scene.mh_collision_kind)
        self.action = "CANCEL"
        return context.window_manager.invoke_props_dialog(self, width=520)

    def draw(self, context):
        layout = self.layout
        message = context.scene.mh_collision_message
        layout.label(text="The requested filename belongs to another UID.",
                     icon="ERROR")
        if message:
            layout.label(text=message[:120])
        layout.prop(self, "action", expand=True)
        if self.action != "CANCEL":
            layout.prop(self, "new_name")
            layout.label(text="Press Export again after confirming.",
                         icon="INFO")

    def execute(self, context):
        if self.action == "CANCEL":
            return {"FINISHED"}
        resource = _collision_resource(
            context.scene, context.scene.mh_collision_kind)
        if resource is None:
            self.report({"ERROR"}, "The colliding Blender resource is missing")
            return {"CANCELLED"}
        try:
            validate_resource_name(self.new_name)
        except (TypeError, ValueError) as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        if self.action == "FORK":
            resource["mh_uid"] = str(uuid.uuid4())
        if (context.scene.mh_collision_kind == "FBX"
                and _LODS_ROOT_RE.fullmatch(resource.name) is not None):
            # Keep the dag4blend authoring hierarchy intact.  The FBX adapter
            # treats this validated property as the logical resource name.
            resource["name"] = self.new_name
        else:
            resource.name = self.new_name
        self.report({"INFO"}, "Resource updated; press Export again")
        return {"FINISHED"}


class MH_OT_export_fbx(bpy.types.Operator):
    bl_idname = "mh.export_fbx"
    bl_label = "Export FBX"
    bl_description = (
        "Export the selected static-mesh resource; a Dagor .lods collection "
        "writes one combined FBX with mh_lod_level-tagged render nodes")

    def execute(self, context):
        collection = context.scene.mh_fbx_collection
        if collection is None:
            self.report({"ERROR"}, "Choose a collection to export")
            return {"CANCELLED"}
        try:
            prefs = prefs_mod.get_prefs(context)
            output_dir = _directory(context.scene.mh_fbx_directory)
            source_root = _directory(prefs.source_root)
            export_materials = context.scene.mh_fbx_export_materials
            report = export_fbx_collection(
                collection, output_dir,
                source_root=source_root)
            material_updates = []
            material_failures = []
            first_material_collision = None
            if report.get("ok") and export_materials:
                for material in report["materials"]:
                    try:
                        prepared = prepare_blender_material_export(
                            material, output_dir, source_root=source_root,
                            texture_policy=prefs.texture_policy)
                        write_prepared_material(
                            prepared, source_root=source_root,
                            texture_policy=prefs.texture_policy)
                    except (OSError, RuntimeError, ValueError) as exc:
                        failure = {
                            "uid": material.get("mh_uid", ""),
                            "name": material.name,
                            "error": str(exc),
                            "quick_action": "Export materials…",
                        }
                        material_failures.append(failure)
                        if (first_material_collision is None
                                and str(exc).startswith(
                                    "MH_E_NAME_COLLISION_DIFFERENT_UID")):
                            first_material_collision = (material, str(exc))
                        continue
                    material_updates.append(prepared.payload_path)
            report["material_updates"] = material_updates
            report["material_failures"] = material_failures
        except (OSError, RuntimeError, ValueError) as exc:
            _log("export_fbx", {"ok": False, "error": str(exc)})
            if str(exc).startswith("MH_E_NAME_COLLISION_DIFFERENT_UID"):
                _open_collision_dialog(context, "FBX", str(exc))
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log("export_fbx", report)
        if not report.get("ok"):
            errors = report.get("validation", {}).get("errors", [])
            message = errors[0].get("code", "FBX validation failed") \
                if errors else "FBX validation failed"
            self.report({"ERROR"}, message)
            return {"CANCELLED"}
        if first_material_collision is not None:
            context.scene.mh_material = first_material_collision[0]
            _open_collision_dialog(
                context, "MATERIAL", first_material_collision[1])
        warning_count = (
            len(report.get("validation", {}).get("warnings", []))
            + len(report.get("material_failures", [])))
        if warning_count:
            self.report(
                {"WARNING"},
                f"FBX exported with {warning_count} warning(s): "
                f"{report['filepath']} — see {LOG_TEXT_NAME}")
            return {"FINISHED"}
        self.report(
            {"INFO"}, f"Combined FBX resource exported: {report['filepath']}")
        return {"FINISHED"}


class MH_OT_export_composite(bpy.types.Operator):
    bl_idname = "mh.export_composite"
    bl_label = "Export Composite"
    bl_description = "Export only the selected composite definition collection"

    def execute(self, context):
        collection = context.scene.mh_composite_export_collection
        if collection is None:
            self.report({"ERROR"}, "Choose a composite collection to export")
            return {"CANCELLED"}
        try:
            prefs = prefs_mod.get_prefs(context)
            report = export_composite_collection(
                collection,
                _directory(context.scene.mh_composite_export_directory),
                source_root=_directory(prefs.source_root),
            )
        except (OSError, RuntimeError, ValueError) as exc:
            _log("export_composite", {"ok": False, "error": str(exc)})
            if str(exc).startswith("MH_E_NAME_COLLISION_DIFFERENT_UID"):
                _open_collision_dialog(context, "COMPOSITE", str(exc))
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log("export_composite", report)
        if not report.get("ok"):
            errors = report.get("validation", {}).get("errors", [])
            message = errors[0].get("code", "Composite validation failed") \
                if errors else "Composite validation failed"
            self.report({"ERROR"}, message)
            return {"CANCELLED"}
        self.report({"INFO"}, f"Composite exported: {report['path']}")
        return {"FINISHED"}


class MH_OT_export_material(bpy.types.Operator):
    bl_idname = "mh.export_material"
    bl_label = "Export Material"
    bl_description = "Export or update one material source by its stable UID"

    def execute(self, context):
        material = context.scene.mh_material
        if material is None:
            self.report({"ERROR"}, "Choose a material to export")
            return {"CANCELLED"}
        try:
            prefs = prefs_mod.get_prefs(context)
            source_root = _directory(prefs.source_root)
            if not os.path.isdir(source_root):
                raise ValueError(
                    f"Project Source Root does not exist: {source_root}")
            uid = ensure_uid(material)
            output_dir = _directory(context.scene.mh_material_directory)
            prepared = prepare_blender_material_export(
                material,
                output_dir,
                source_root=source_root,
                texture_policy=prefs.texture_policy,
            )
            written = write_prepared_material(
                prepared, source_root=source_root,
                texture_policy=prefs.texture_policy)
            report = {
                "ok": True,
                "uid": uid,
                "path": prepared.payload_path,
                "written": written,
                "warnings": [
                    row.disk_dict() for row in prepared.diagnostics
                ],
            }
        except (OSError, RuntimeError, ValueError) as exc:
            _log("export_material", {"ok": False, "error": str(exc)})
            if str(exc).startswith("MH_E_NAME_COLLISION_DIFFERENT_UID"):
                _open_collision_dialog(context, "MATERIAL", str(exc))
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log("export_material", report)
        if report["warnings"]:
            self.report(
                {"WARNING"},
                f"Material exported with {len(report['warnings'])} warning(s): "
                f"{report['path']} — see {LOG_TEXT_NAME}")
        else:
            self.report({"INFO"}, f"Material exported: {report['path']}")
        return {"FINISHED"}


class MH_OT_actualize_texture_paths(bpy.types.Operator):
    bl_idname = "mh.actualize_texture_paths"
    bl_label = "Actualize Texture Paths"
    bl_description = (
        "Repair stale paths in exported .material files when exactly one "
        "matching texture basename exists under Project Source Root")

    def execute(self, context):
        try:
            prefs = prefs_mod.get_prefs(context)
            source_root = _directory(prefs.source_root)
            if not os.path.isdir(source_root):
                raise ValueError(
                    f"Project Source Root does not exist: {source_root}")
            report = actualize_texture_paths(
                source_root,
                texture_policy=prefs.texture_policy,
            )
        except (OSError, RuntimeError, ValueError) as exc:
            _log("actualize_texture_paths", {"ok": False, "error": str(exc)})
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log("actualize_texture_paths", report)
        warning_count = len(report["ambiguous"]) + len(report["missing"])
        message = (
            f"Texture paths: {len(report['fixed'])} fixed, "
            f"{len(report['ambiguous'])} ambiguous, "
            f"{len(report['missing'])} missing")
        if warning_count:
            self.report({"WARNING"}, message + f" — see {LOG_TEXT_NAME}")
        else:
            self.report({"INFO"}, message)
        return {"FINISHED"}


class MH_OT_import_composite(bpy.types.Operator):
    bl_idname = "mh.import_composite"
    bl_label = "Import Composite"
    bl_description = (
        "Import a composite and its reachable passport dependencies into "
        "the GEOMETRY scene")
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            prefs = prefs_mod.get_prefs(context)
            source_root = _directory(prefs.source_root)
            if not os.path.isdir(source_root):
                raise ValueError(
                    f"Project Source Root does not exist: {source_root}")
            report = import_composite_file(
                _filepath(context.scene.mh_composite_import_path),
                source_root=source_root,
                texture_policy=prefs.texture_policy,
            )
        except (OSError, RuntimeError, ValueError) as exc:
            _log("import_composite", {"ok": False, "error": str(exc)})
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log("import_composite", report)
        unresolved = len(report.get("unresolved", []))
        warning_count = len(report.get("warnings", []))
        suffix = f", {unresolved} placeholder(s)" if unresolved else ""
        if warning_count:
            suffix += f", {warning_count} warning(s) — see {LOG_TEXT_NAME}"
        self.report(
            {"INFO"},
            f"Composite imported: {report['root_collection'].name}{suffix}")
        return {"FINISHED"}


class MH_OT_migrate_sources_v2(bpy.types.Operator):
    bl_idname = "mh.migrate_sources_v2"
    bl_label = "Migrate v1 Sources to v2"
    bl_description = (
        "One-shot conversion of legacy UID-suffixed manifest sources to "
        "clean self-contained v2 payloads")

    dry_run: bpy.props.BoolProperty(
        name="Preview only",
        description="Validate and report every operation without writing files",
        default=True,
    )

    def invoke(self, context, _event):
        self.dry_run = True
        return context.window_manager.invoke_props_dialog(self, width=520)

    def draw(self, _context):
        layout = self.layout
        layout.label(
            text="This scans the entire Project Source Root.", icon="INFO")
        layout.prop(self, "dry_run")
        if not self.dry_run:
            layout.label(
                text="Legacy manifests and UID-suffixed payloads will be removed.",
                icon="ERROR")

    def execute(self, context):
        try:
            prefs = prefs_mod.get_prefs(context)
            source_root = _directory(prefs.source_root)
            report = migrate_sources_v2(
                source_root, dry_run=self.dry_run)
        except (OSError, RuntimeError, ValueError) as exc:
            _log("migrate_sources_v2", {"ok": False, "error": str(exc)})
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log("migrate_sources_v2", report)
        migrated = len(report.get("migrated", []))
        failed = len(report.get("failed", []))
        if report.get("blocked") or failed:
            self.report(
                {"ERROR"},
                f"Migration blocked: {failed} failure(s) — see {LOG_TEXT_NAME}")
            return {"CANCELLED"}
        verb = "would migrate" if self.dry_run else "migrated"
        self.report(
            {"INFO"}, f"v2 migration {verb} {migrated} resource(s)")
        return {"FINISHED"}


CLASSES = (
    MH_OT_resolve_name_collision,
    MH_OT_export_fbx,
    MH_OT_export_material,
    MH_OT_actualize_texture_paths,
    MH_OT_export_composite,
    MH_OT_import_composite,
    MH_OT_migrate_sources_v2,
)


def register():
    bpy.types.Scene.mh_collision_kind = bpy.props.StringProperty(
        name="Collision Resource Kind", default="")
    bpy.types.Scene.mh_collision_message = bpy.props.StringProperty(
        name="Collision Message", default="")
    bpy.types.Scene.mh_fbx_collection = bpy.props.PointerProperty(
        name="Collection",
        description=(
            "Static-mesh resource collection; a Dagor .lods hierarchy "
            "exports one combined FBX for all .lodNN children"),
        type=bpy.types.Collection,
    )
    bpy.types.Scene.mh_fbx_directory = bpy.props.StringProperty(
        name="Output Folder", subtype="DIR_PATH", default="")
    bpy.types.Scene.mh_fbx_export_materials = bpy.props.BoolProperty(
        name="Export Materials",
        description=(
            "Update every material used by the exported collection; "
            "textures remain at their authored paths"),
        default=True,
    )
    bpy.types.Scene.mh_material = bpy.props.PointerProperty(
        name="Material",
        description="Material exported as one .material resource",
        type=bpy.types.Material,
    )
    bpy.types.Scene.mh_material_directory = bpy.props.StringProperty(
        name="Output Folder", subtype="DIR_PATH", default="")
    bpy.types.Scene.mh_composite_mode = bpy.props.EnumProperty(
        name="Mode",
        items=(
            ("IMPORT", "Import", "Import a .composite source"),
            ("EXPORT", "Export", "Export a composite collection"),
        ),
        default="IMPORT",
    )
    bpy.types.Scene.mh_composite_import_path = bpy.props.StringProperty(
        name="Composite", subtype="FILE_PATH", default="")
    bpy.types.Scene.mh_composite_export_collection = bpy.props.PointerProperty(
        name="Collection",
        description="Collection whose Empty instances are composite nodes",
        type=bpy.types.Collection,
    )
    bpy.types.Scene.mh_composite_export_directory = bpy.props.StringProperty(
        name="Output Folder", subtype="DIR_PATH", default="")
    for cls in CLASSES:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
    for name in (
        "mh_composite_export_directory",
        "mh_composite_export_collection",
        "mh_composite_import_path",
        "mh_composite_mode",
        "mh_fbx_export_materials",
        "mh_fbx_directory",
        "mh_fbx_collection",
        "mh_material_directory",
        "mh_material",
        "mh_collision_message",
        "mh_collision_kind",
    ):
        if hasattr(bpy.types.Scene, name):
            delattr(bpy.types.Scene, name)
