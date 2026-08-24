"""Blender operators for Source Protocol v4 workflows."""

import json
import os

import bpy

from .. import prefs as prefs_mod
from ..scene.export_composite import export_composite_collection
from ..scene.export_fbx import export_fbx_collection
from ..scene.export_material import (
    prepare_blender_material_export,
    write_prepared_material,
)
from ..scene.import_composite import import_composite_file

LOG_TEXT_NAME = "mh_export_log"


def _json_default(value):
    if hasattr(value, "name"):
        return value.name
    return str(value)


def _log(operation, report):
    text = bpy.data.texts.get(LOG_TEXT_NAME) or bpy.data.texts.new(LOG_TEXT_NAME)
    text.write(json.dumps(
        {"operation": operation, "report": report},
        ensure_ascii=False, sort_keys=True, default=_json_default) + "\n")


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
        "Export one static-mesh source as plain FBX; .lods collections use "
        "temporary _lodNN node names")

    def execute(self, context):
        collection = context.scene.mh_fbx_collection
        if collection is None:
            self.report({"ERROR"}, "Choose a collection to export")
            return {"CANCELLED"}
        try:
            preferences = prefs_mod.get_prefs(context)
            if context.scene.mh_fbx_export_materials:
                raise RuntimeError(
                    "MH_E_INVALID_MATERIAL_VALUE: Export Materials is "
                    "unavailable until slice S2")
            report = export_fbx_collection(
                collection,
                _directory(context.scene.mh_fbx_directory),
                source_root=_directory(preferences.source_root),
            )
        except (OSError, RuntimeError, ValueError) as exc:
            _log("export_fbx", {"ok": False, "error": str(exc)})
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log("export_fbx", report)
        self.report({"INFO"}, f"FBX exported: {report['filepath']}")
        return {"FINISHED"}


class MH_OT_export_material(bpy.types.Operator):
    bl_idname = "mh.export_material"
    bl_label = "Export Material"
    bl_description = "Export one Source Protocol v4 material"

    def execute(self, context):
        material = context.scene.mh_material
        if material is None:
            self.report({"ERROR"}, "Choose a material to export")
            return {"CANCELLED"}
        try:
            preferences = prefs_mod.get_prefs(context)
            source_root = _directory(preferences.source_root)
            prepared = prepare_blender_material_export(
                material, _directory(context.scene.mh_material_directory),
                source_root=source_root)
            report = write_prepared_material(prepared, source_root=source_root)
        except (OSError, RuntimeError, ValueError) as exc:
            _log("export_material", {"ok": False, "error": str(exc)})
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log("export_material", report)
        self.report({"INFO"}, "Material exported")
        return {"FINISHED"}


class MH_OT_export_composite(bpy.types.Operator):
    bl_idname = "mh.export_composite"
    bl_label = "Export Composite"
    bl_description = "Export one Source Protocol v4 composite"

    def execute(self, context):
        collection = context.scene.mh_composite_export_collection
        if collection is None:
            self.report({"ERROR"}, "Choose a composite collection")
            return {"CANCELLED"}
        try:
            preferences = prefs_mod.get_prefs(context)
            report = export_composite_collection(
                collection,
                _directory(context.scene.mh_composite_export_directory),
                source_root=_directory(preferences.source_root))
        except (OSError, RuntimeError, ValueError) as exc:
            _log("export_composite", {"ok": False, "error": str(exc)})
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log("export_composite", report)
        self.report({"INFO"}, "Composite exported")
        return {"FINISHED"}


class MH_OT_import_composite(bpy.types.Operator):
    bl_idname = "mh.import_composite"
    bl_label = "Import Composite"
    bl_description = "Import one Source Protocol v4 composite"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            preferences = prefs_mod.get_prefs(context)
            report = import_composite_file(
                _filepath(context.scene.mh_composite_import_path),
                source_root=_directory(preferences.source_root))
        except (OSError, RuntimeError, ValueError) as exc:
            _log("import_composite", {"ok": False, "error": str(exc)})
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log("import_composite", report)
        self.report({"INFO"}, "Composite imported")
        return {"FINISHED"}


CLASSES = (
    MH_OT_export_fbx,
    MH_OT_export_material,
    MH_OT_export_composite,
    MH_OT_import_composite,
)


def register():
    bpy.types.Scene.mh_fbx_collection = bpy.props.PointerProperty(
        name="Collection", type=bpy.types.Collection)
    bpy.types.Scene.mh_fbx_directory = bpy.props.StringProperty(
        name="Output Folder", subtype="DIR_PATH", default="")
    bpy.types.Scene.mh_fbx_export_materials = bpy.props.BoolProperty(
        name="Export Materials",
        description="Enabled by the Source Protocol v4 S2 material slice",
        default=False)
    bpy.types.Scene.mh_material = bpy.props.PointerProperty(
        name="Material", type=bpy.types.Material)
    bpy.types.Scene.mh_material_directory = bpy.props.StringProperty(
        name="Output Folder", subtype="DIR_PATH", default="")
    bpy.types.Scene.mh_composite_mode = bpy.props.EnumProperty(
        name="Mode", items=(("IMPORT", "Import", "Import a composite"),
                             ("EXPORT", "Export", "Export a composite")),
        default="IMPORT")
    bpy.types.Scene.mh_composite_import_path = bpy.props.StringProperty(
        name="Composite", subtype="FILE_PATH", default="")
    bpy.types.Scene.mh_composite_export_collection = bpy.props.PointerProperty(
        name="Collection", type=bpy.types.Collection)
    bpy.types.Scene.mh_composite_export_directory = bpy.props.StringProperty(
        name="Output Folder", subtype="DIR_PATH", default="")
    for cls in CLASSES:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
    for name in (
        "mh_composite_export_directory", "mh_composite_export_collection",
        "mh_composite_import_path", "mh_composite_mode",
        "mh_fbx_export_materials", "mh_fbx_directory", "mh_fbx_collection",
        "mh_material_directory", "mh_material",
    ):
        if hasattr(bpy.types.Scene, name):
            delattr(bpy.types.Scene, name)
