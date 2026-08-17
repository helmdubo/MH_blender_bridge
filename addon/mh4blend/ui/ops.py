"""Standalone FBX and composite operators.

Each button acts only on the collection/file selected beside it. There is no
combined scene scan and no bundle export operator in the user interface.
"""

from __future__ import annotations

import json
import os

import bpy

from .. import prefs as prefs_mod
from ..core.source_resolver import (
    resolve_resource_for_writer,
    scan_source_root,
)
from ..core.uid import ensure_uid
from ..scene.export_composite import export_composite_collection
from ..scene.export_fbx import export_fbx_collection
from ..scene.export_material import (
    prepare_blender_material_export,
    write_prepared_material,
)
from ..scene.import_composite import import_composite_file
from ..scene.source_manifest import (
    abandon_staged_manifest,
    commit_staged_manifest,
    prepare_manifest_update,
    stage_manifest,
)


LOG_TEXT_NAME = "mh_export_log"


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


class MH_OT_export_fbx(bpy.types.Operator):
    bl_idname = "mh.export_fbx"
    bl_label = "Export FBX"
    bl_description = (
        "Export the selected static-mesh resource; a Dagor .lods collection "
        "writes one FBX payload per authored LOD")

    def execute(self, context):
        collection = context.scene.mh_fbx_collection
        if collection is None:
            self.report({"ERROR"}, "Choose a collection to export")
            return {"CANCELLED"}
        try:
            prefs = prefs_mod.get_prefs(context)
            report = export_fbx_collection(
                collection, _directory(context.scene.mh_fbx_directory),
                registry_path=prefs.registry_path,
                export_materials=context.scene.mh_fbx_export_materials,
                source_root=prefs.source_root,
                texture_policy=prefs.texture_policy)
        except (OSError, RuntimeError, ValueError) as exc:
            _log("export_fbx", {"ok": False, "error": str(exc)})
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log("export_fbx", report)
        if not report.get("ok"):
            errors = report.get("validation", {}).get("errors", [])
            message = errors[0].get("code", "FBX validation failed") \
                if errors else "FBX validation failed"
            self.report({"ERROR"}, message)
            return {"CANCELLED"}
        warning_count = len(report.get("validation", {}).get("warnings", []))
        if warning_count:
            self.report(
                {"WARNING"},
                f"FBX exported with {warning_count} warning(s): "
                f"{report['filepath']} — see {LOG_TEXT_NAME}")
            return {"FINISHED"}
        lod_count = max(0, len(report.get("payload_updates", ())) - 1)
        suffix = f" + {lod_count} LOD payload(s)" if lod_count else ""
        self.report(
            {"INFO"}, f"FBX resource exported: {report['filepath']}{suffix}")
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
                registry_path=prefs.registry_path,
                texture_policy=prefs.texture_policy,
            )
        except (OSError, RuntimeError, ValueError) as exc:
            _log("export_composite", {"ok": False, "error": str(exc)})
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
            registry_path = (
                os.path.abspath(bpy.path.abspath(prefs.registry_path))
                if prefs.registry_path else None)
            uid = ensure_uid(material)
            resolution = resolve_resource_for_writer(
                source_root,
                uid,
                expected_kind="material",
                registry_path=registry_path,
                texture_policy=prefs.texture_policy)
            snapshot = resolution.snapshot
            owner = resolution.owner
            output_dir = (
                os.path.dirname(owner.payload_path)
                if owner is not None
                else _directory(context.scene.mh_material_directory))
            prepared = prepare_blender_material_export(
                material,
                output_dir,
                source_root=source_root,
                texture_policy=prefs.texture_policy,
                target_payload_path=(owner.payload_path if owner else None),
                owning_manifest_path=(
                    owner.owning_manifest_path if owner else None),
                existing_source=(
                    owner.manifest_row["source"] if owner else None),
            )
            owner_dir = os.path.dirname(prepared.owning_manifest_path)
            manifest = prepare_manifest_update(
                owner_dir,
                resources=[prepared.resource_row],
                exporter_version="0.4.1",
                blend_file=os.path.basename(bpy.data.filepath) or None,
                source_root=source_root,
            )
            stage_manifest(owner_dir, manifest)
            try:
                written = write_prepared_material(
                    prepared,
                    source_root=source_root,
                    texture_policy=prefs.texture_policy,
                    force=manifest.is_recovery,
                )
                manifest_path = commit_staged_manifest(owner_dir)
            except BaseException:
                abandon_staged_manifest(owner_dir)
                raise
            report = {
                "ok": True,
                "uid": uid,
                "path": prepared.payload_path,
                "written": written,
                "manifest_path": manifest_path,
                "warnings": [
                    row.disk_dict() for row in prepared.diagnostics
                ] + [{
                    "code": row.code,
                    "subjects": ([row.uid] if row.uid else []),
                    "message": row.message,
                } for row in snapshot.diagnostics],
            }
        except (OSError, RuntimeError, ValueError) as exc:
            _log("export_material", {"ok": False, "error": str(exc)})
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


class MH_OT_import_composite(bpy.types.Operator):
    bl_idname = "mh.import_composite"
    bl_label = "Import Composite"
    bl_description = (
        "Import a composite and its reachable manifest dependencies into "
        "the GEOMETRY scene")
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            prefs = prefs_mod.get_prefs(context)
            source_root = _directory(prefs.source_root)
            if not os.path.isdir(source_root):
                raise ValueError(
                    f"Project Source Root does not exist: {source_root}")
            registry_path = (
                os.path.abspath(bpy.path.abspath(prefs.registry_path))
                if prefs.registry_path else None)
            report = import_composite_file(
                _filepath(context.scene.mh_composite_import_path),
                source_root=source_root,
                registry_path=registry_path,
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


CLASSES = (
    MH_OT_export_fbx,
    MH_OT_export_material,
    MH_OT_export_composite,
    MH_OT_import_composite,
)


def register():
    bpy.types.Scene.mh_fbx_collection = bpy.props.PointerProperty(
        name="Collection",
        description=(
            "Static-mesh resource collection; a Dagor .lods hierarchy "
            "exports separate FBX payloads for its .lodNN children"),
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
    ):
        if hasattr(bpy.types.Scene, name):
            delattr(bpy.types.Scene, name)
