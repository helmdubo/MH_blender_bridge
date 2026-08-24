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
from ..scene.import_fbx import import_mesh_fbx

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


def _composite_filepath(value):
    if not isinstance(value, str) or not value.strip():
        raise ValueError("Choose a .composite file")
    path = os.path.abspath(bpy.path.abspath(value))
    if not path.endswith(".composite"):
        raise ValueError(
            "MH_E_NONCANONICAL_RESOURCE_NAME: import path must point to a "
            "lowercase .composite file")
    if not os.path.isfile(path):
        raise ValueError(f"Composite file does not exist: {path}")
    return path


# Kept as a compatibility seam for the existing UI gate; new code should use
# the workflow-specific validators so compound extensions remain explicit.
_filepath = _composite_filepath


def _mesh_filepath(value):
    if not isinstance(value, str) or not value.strip():
        raise ValueError("Choose a .mesh.fbx file")
    path = os.path.abspath(bpy.path.abspath(value))
    if not path.endswith(".mesh.fbx"):
        raise ValueError(
            "MH_E_NONCANONICAL_RESOURCE_NAME: import path must point to a "
            "lowercase .mesh.fbx file")
    stem = os.path.basename(path)[:-len(".mesh.fbx")]
    from ..core.canonical import validate_resource_name
    validate_resource_name(stem)
    if not os.path.isfile(path):
        raise ValueError(f"Mesh FBX file does not exist: {path}")
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
            report = export_fbx_collection(
                collection,
                _directory(context.scene.mh_fbx_directory),
                source_root=_directory(preferences.source_root),
                export_materials=context.scene.mh_fbx_export_materials,
            )
        except (OSError, RuntimeError, ValueError) as exc:
            _log("export_fbx", {"ok": False, "error": str(exc)})
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log("export_fbx", report)
        self.report({"INFO"}, f"FBX exported: {report['filepath']}")
        return {"FINISHED"}


class MH_OT_import_mesh_fbx(bpy.types.Operator):
    bl_idname = "mh.import_mesh_fbx"
    bl_label = "Import Mesh FBX"
    bl_description = (
        "Import one Source Protocol v4 mesh FBX as an editable Blender "
        "resource")
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            preferences = prefs_mod.get_prefs(context)
            report = import_mesh_fbx(
                _mesh_filepath(context.scene.mh_fbx_import_path),
                source_root=_directory(preferences.source_root),
            )
        except (OSError, RuntimeError, ValueError) as exc:
            _log("import_mesh_fbx", {"ok": False, "error": str(exc)})
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log("import_mesh_fbx", report)
        self.report({"INFO"}, "Mesh FBX imported")
        return {"FINISHED"}


class MHMaterialTextureProperty(bpy.types.PropertyGroup):
    slot: bpy.props.IntProperty(name="Slot", min=0, max=15, default=0)
    image: bpy.props.PointerProperty(name="Image", type=bpy.types.Image)


class MHMaterialParamProperty(bpy.types.PropertyGroup):
    kind: bpy.props.EnumProperty(
        name="Kind",
        items=(("SCALAR", "Scalar", "UE scalar parameter"),
               ("VECTOR", "Vector", "UE vector4 parameter")),
        default="SCALAR")
    scalar: bpy.props.FloatProperty(name="Scalar", default=0.0)
    vector: bpy.props.FloatVectorProperty(name="Vector", size=4)


class MHMaterialProperties(bpy.types.PropertyGroup):
    mode: bpy.props.EnumProperty(
        name="Mode",
        items=(("CLASS", "Class", "Material master class"),
               ("LIBRARY", "Library", "Strict library material reference")),
        default="CLASS")
    material_class: bpy.props.StringProperty(name="Class", default="")
    library: bpy.props.StringProperty(name="Library", default="")
    twosided_override: bpy.props.BoolProperty(
        name="Override Two Sided", default=False)
    twosided: bpy.props.BoolProperty(name="Two Sided", default=False)
    textures: bpy.props.CollectionProperty(type=MHMaterialTextureProperty)
    params: bpy.props.CollectionProperty(type=MHMaterialParamProperty)


def _selected_material(context):
    material = context.scene.mh_material
    if material is None:
        raise ValueError("Choose a material")
    return material


class MH_OT_material_texture_add(bpy.types.Operator):
    bl_idname = "mh.material_texture_add"
    bl_label = "Add Texture Slot"
    bl_options = {"UNDO"}

    def execute(self, context):
        try:
            settings = _selected_material(context).mh4blend
            if settings.mode != "CLASS":
                raise ValueError(
                    "MH_E_MATERIAL_GRAMMAR: library materials cannot have overrides")
            used = {row.slot for row in settings.textures}
            slot = next((value for value in range(16) if value not in used), None)
            if slot is None:
                raise ValueError(
                    "MH_E_MATERIAL_GRAMMAR: all texture slots tex0..tex15 are used")
            settings.textures.add().slot = slot
        except ValueError as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        return {"FINISHED"}


class MH_OT_material_texture_remove(bpy.types.Operator):
    bl_idname = "mh.material_texture_remove"
    bl_label = "Remove Texture Slot"
    bl_options = {"UNDO"}

    index: bpy.props.IntProperty(options={"HIDDEN"}, default=-1)

    def execute(self, context):
        try:
            rows = _selected_material(context).mh4blend.textures
            if not 0 <= self.index < len(rows):
                raise ValueError("Texture row index is out of range")
            rows.remove(self.index)
        except ValueError as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        return {"FINISHED"}


class MH_OT_material_param_add(bpy.types.Operator):
    bl_idname = "mh.material_param_add"
    bl_label = "Add Parameter"
    bl_options = {"UNDO"}

    def execute(self, context):
        try:
            settings = _selected_material(context).mh4blend
            if settings.mode != "CLASS":
                raise ValueError(
                    "MH_E_MATERIAL_GRAMMAR: library materials cannot have overrides")
            used = {row.name for row in settings.params}
            index = 0
            while True:
                name = "param" if index == 0 else f"param_{index}"
                if name not in used:
                    break
                index += 1
            row = settings.params.add()
            row.name = name
            row.kind = "SCALAR"
            row.scalar = 0.0
        except ValueError as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        return {"FINISHED"}


class MH_OT_material_param_remove(bpy.types.Operator):
    bl_idname = "mh.material_param_remove"
    bl_label = "Remove Parameter"
    bl_options = {"UNDO"}

    index: bpy.props.IntProperty(options={"HIDDEN"}, default=-1)

    def execute(self, context):
        try:
            rows = _selected_material(context).mh4blend.params
            if not 0 <= self.index < len(rows):
                raise ValueError("Parameter row index is out of range")
            rows.remove(self.index)
        except ValueError as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
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
                _composite_filepath(context.scene.mh_composite_import_path),
                source_root=_directory(preferences.source_root))
        except (OSError, RuntimeError, ValueError) as exc:
            _log("import_composite", {"ok": False, "error": str(exc)})
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log("import_composite", report)
        for warning in report.get("warnings", []):
            self.report(
                {"WARNING"},
                f"{warning['code']}: {warning.get('message', '')}")
        self.report({"INFO"}, "Composite imported")
        return {"FINISHED"}


PROPERTY_CLASSES = (
    MHMaterialTextureProperty,
    MHMaterialParamProperty,
    MHMaterialProperties,
)


CLASSES = (
    MH_OT_material_texture_add,
    MH_OT_material_texture_remove,
    MH_OT_material_param_add,
    MH_OT_material_param_remove,
    MH_OT_export_fbx,
    MH_OT_import_mesh_fbx,
    MH_OT_export_material,
    MH_OT_export_composite,
    MH_OT_import_composite,
)


def register():
    for cls in PROPERTY_CLASSES:
        bpy.utils.register_class(cls)
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.mh_fbx_collection = bpy.props.PointerProperty(
        name="Collection", type=bpy.types.Collection)
    bpy.types.Scene.mh_fbx_directory = bpy.props.StringProperty(
        name="Output Folder", subtype="DIR_PATH", default="")
    bpy.types.Scene.mh_fbx_import_path = bpy.props.StringProperty(
        name="Mesh FBX", subtype="FILE_PATH", default="")
    bpy.types.Scene.mh_fbx_export_materials = bpy.props.BoolProperty(
        name="Export Materials",
        description="Write every material used by the exported mesh resource",
        default=False)
    bpy.types.Scene.mh_material = bpy.props.PointerProperty(
        name="Material", type=bpy.types.Material)
    bpy.types.Scene.mh_material_directory = bpy.props.StringProperty(
        name="Output Folder", subtype="DIR_PATH", default="")
    bpy.types.Material.mh4blend = bpy.props.PointerProperty(
        name="MH Material", type=MHMaterialProperties)
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
def unregister():
    if hasattr(bpy.types.Material, "mh4blend"):
        delattr(bpy.types.Material, "mh4blend")
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
    for name in (
        "mh_composite_export_directory", "mh_composite_export_collection",
        "mh_composite_import_path", "mh_composite_mode",
        "mh_fbx_export_materials", "mh_fbx_import_path",
        "mh_fbx_directory", "mh_fbx_collection",
        "mh_material_directory", "mh_material",
    ):
        if hasattr(bpy.types.Scene, name):
            delattr(bpy.types.Scene, name)
    for cls in reversed(PROPERTY_CLASSES):
        bpy.utils.unregister_class(cls)
