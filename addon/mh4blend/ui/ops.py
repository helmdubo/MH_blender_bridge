"""Blender operators for the active Source Protocol host workflows."""

import json
import os

import bpy

from .. import prefs as prefs_mod
from ..scene.export_composite import export_composite_collection
from ..scene.export_closure import (
    CLOSURE_MODE_COMPOSITES,
    CLOSURE_MODE_INCLUDE_ALL,
    export_composite_closure_collection,
)
from ..scene.export_fbx import export_fbx_collection
from ..scene.export_material import (
    prepare_blender_material_export,
    write_prepared_material,
)
from ..scene.import_composite import import_composite_file
from ..scene.import_dagor_composite import (
    import_dag4blend_composite_collection,
    import_dagor_composite_file,
)
from ..scene.import_fbx import (
    LOAD_MODE_FULL_LOD,
    LOAD_MODE_LOD0,
    LOAD_MODE_STRUCTURE_ONLY,
    import_mesh_fbx,
)
from ..scene.project_textures import (
    copy_all_dagor_textures_to_project,
    remap_all_dagor_textures_to_project,
)

LOG_TEXT_NAME = "mh_export_log"

_LOAD_MODE_VALUES = {
    "FULL_LOD": LOAD_MODE_FULL_LOD,
    "LOD0": LOAD_MODE_LOD0,
    "STRUCTURE_ONLY": LOAD_MODE_STRUCTURE_ONLY,
}


def _load_mode(scene):
    return _LOAD_MODE_VALUES[scene.mh_import_load_mode]


def _definition_policy(scene):
    return scene.mh_import_definition_policy.lower()


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


def _optional_directory(value):
    if not isinstance(value, str) or not value.strip():
        return None
    return _directory(value)


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


def _dagor_composite_filepath(value):
    if not isinstance(value, str) or not value.strip():
        raise ValueError("Choose a .composit.blk file")
    path = os.path.abspath(bpy.path.abspath(value))
    if not path.endswith(".composit.blk"):
        raise ValueError(
            "MH_E_NONCANONICAL_RESOURCE_NAME: import path must point to an "
            "exact .composit.blk file")
    if not os.path.isfile(path):
        raise ValueError(f"Dagor Composite file does not exist: {path}")
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
                load_mode=_load_mode(context.scene),
                definition_policy=_definition_policy(context.scene),
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
               ("VECTOR", "Vector", "UE vector4 parameter"),
               ("STRING", "String", "Opaque source provenance"),
               ("BOOLEAN", "Boolean", "Opaque source provenance")),
        default="SCALAR")
    scalar: bpy.props.FloatProperty(name="Scalar", default=0.0)
    vector: bpy.props.FloatVectorProperty(name="Vector", size=4)
    string: bpy.props.StringProperty(name="String", default="")
    boolean: bpy.props.BoolProperty(name="Boolean", default=False)


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


class MH_OT_copy_all_textures_to_project(bpy.types.Operator):
    bl_idname = "mh.copy_all_textures_to_project"
    bl_label = "Copy All Textures to Project"
    bl_description = (
        "Copy every Dagor texture used by this blend into Project Source "
        "Root while preserving the folder tree below assets")

    def execute(self, context):
        try:
            preferences = prefs_mod.get_prefs(context)
            report = copy_all_dagor_textures_to_project(
                source_root=_directory(preferences.source_root))
        except (OSError, RuntimeError, ValueError) as exc:
            _log("copy_all_textures_to_project", {
                "ok": False, "error": str(exc)})
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log("copy_all_textures_to_project", report)
        self.report(
            {"INFO"},
            f"Textures copied: {report['copied']}; "
            f"already in project: {report['skipped']}")
        return {"FINISHED"}


class MH_OT_remap_all_textures_to_project(bpy.types.Operator):
    bl_idname = "mh.remap_all_textures_to_project"
    bl_label = "Remap All Texture Paths"
    bl_description = (
        "Point every Dagor texture used by this blend at its copied file "
        "inside Project Source Root")
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            preferences = prefs_mod.get_prefs(context)
            report = remap_all_dagor_textures_to_project(
                source_root=_directory(preferences.source_root))
        except (OSError, RuntimeError, ValueError) as exc:
            _log("remap_all_textures_to_project", {
                "ok": False, "error": str(exc)})
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log("remap_all_textures_to_project", report)
        self.report({"INFO"}, f"Texture paths remapped: {report['remapped']}")
        return {"FINISHED"}


def _prefab_lossy_option():
    return bpy.props.BoolProperty(
        name="Allow Prefab as Mesh (Lossy)",
        description="Explicitly export Dagor prefab geometry without collision/gameplay semantics",
        default=False,
    )


def _invoke_composite_export(operator, context):
    from ..scene.composite_scene_adapter import composite_scene_form
    collection = context.scene.mh_composite_export_collection
    try:
        if collection is not None and composite_scene_form(collection) == "dag4blend":
            return context.window_manager.invoke_props_dialog(operator)
    except ValueError:
        # The regular execution path reports the same fail-closed diagnostic.
        pass
    return operator.execute(context)


def _format_export_warning(warning):
    """Render both warning shapes the report can carry.

    Adapter warnings are dicts with code/node_path/message; writer warnings
    (material merges, dropped collision nodes) are (code, subjects, message)
    tuples. Anything else degrades to its repr instead of a formatter crash
    after a successful publication.
    """
    if isinstance(warning, dict):
        return (f"{warning.get('code', 'MH_W')}: "
                f"{warning.get('node_path', '')}: "
                f"{warning.get('message', '')}")
    if isinstance(warning, (tuple, list)) and len(warning) == 3:
        code, subjects, message = warning
        return f"{code}: {', '.join(str(row) for row in subjects)}: {message}"
    return repr(warning)


def _report_export_warnings(operator, report):
    for warning in report.get("warnings", []):
        operator.report({"WARNING"}, _format_export_warning(warning))


def _export_failure_report(exc):
    failure = {"ok": False, "error": str(exc)}
    for field in ("code", "published", "unpublished", "warnings", "compatibility"):
        if hasattr(exc, field):
            failure[field] = getattr(exc, field)
    return failure


class MH_OT_export_composite(bpy.types.Operator):
    bl_idname = "mh.export_composite"
    bl_label = "Export Composite"
    bl_description = "Export one Source Protocol v5 composite"

    allow_prefab_as_mesh_lossy: _prefab_lossy_option()

    def invoke(self, context, _event):
        return _invoke_composite_export(self, context)

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
                source_root=_directory(preferences.source_root),
                allow_prefab_as_mesh_lossy=self.allow_prefab_as_mesh_lossy)
        except (OSError, RuntimeError, ValueError) as exc:
            failure = _export_failure_report(exc)
            _log("export_composite", failure)
            _report_export_warnings(self, failure)
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log("export_composite", report)
        _report_export_warnings(self, report)
        self.report({"INFO"}, "Composite exported")
        return {"FINISHED"}


class _MH_OT_export_composite_closure_base:
    closure_mode = ""

    def invoke(self, context, _event):
        return _invoke_composite_export(self, context)

    def execute(self, context):
        collection = context.scene.mh_composite_export_collection
        if collection is None:
            self.report({"ERROR"}, "Choose a composite collection")
            return {"CANCELLED"}
        try:
            preferences = prefs_mod.get_prefs(context)
            report = export_composite_closure_collection(
                collection,
                _directory(context.scene.mh_composite_export_directory),
                source_root=_directory(preferences.source_root),
                mode=self.closure_mode,
                allow_prefab_as_mesh_lossy=self.allow_prefab_as_mesh_lossy,
            )
        except (OSError, RuntimeError, ValueError) as exc:
            failure = _export_failure_report(exc)
            _log(self.bl_idname.removeprefix("mh."), failure)
            _report_export_warnings(self, failure)
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log(self.bl_idname.removeprefix("mh."), report)
        _report_export_warnings(self, report)
        self.report(
            {"INFO"},
            f"Closure exported: {len(report['published'])} published, "
            f"{len(report['reused'])} reused",
        )
        return {"FINISHED"}


class MH_OT_export_composite_closure(
        _MH_OT_export_composite_closure_base, bpy.types.Operator):
    bl_idname = "mh.export_composite_closure"
    bl_label = "Export Composite + Composite Closure"
    bl_description = (
        "Export root, every nested composite option and placement profiles")
    closure_mode = CLOSURE_MODE_COMPOSITES
    allow_prefab_as_mesh_lossy: _prefab_lossy_option()


class MH_OT_export_composite_include_all(
        _MH_OT_export_composite_closure_base, bpy.types.Operator):
    bl_idname = "mh.export_composite_include_all"
    bl_label = "Export Composite Include All Stuff"
    bl_description = (
        "Export the full all-options closure including meshes and materials")
    closure_mode = CLOSURE_MODE_INCLUDE_ALL
    allow_prefab_as_mesh_lossy: _prefab_lossy_option()


class MH_OT_import_composite(bpy.types.Operator):
    bl_idname = "mh.import_composite"
    bl_label = "Import Composite"
    bl_description = "Import one Source Protocol v5 composite"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            preferences = prefs_mod.get_prefs(context)
            report = import_composite_file(
                _composite_filepath(context.scene.mh_composite_import_path),
                source_root=_directory(preferences.source_root),
                load_mode=_load_mode(context.scene),
                definition_policy=_definition_policy(context.scene))
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


class MH_OT_import_dagor_composite(bpy.types.Operator):
    bl_idname = "mh.import_dagor_composite"
    bl_label = "Import Dagor Composite (Legacy)"
    bl_description = (
        "LEGACY/LIMITED path (a): convert an authoritative .composit.blk "
        "closure with the strict BLK reader. Not the production route and no "
        "parity is planned (owner decision 2026-08-30): it maps gameObj onto "
        "the executable actor kind, admits no empty random variant, and emits "
        "none of the ratified node carriers. Use the dag4blend scene route "
        "(export the imported dag4blend Collection) instead")
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            preferences = prefs_mod.get_prefs(context)
            report = import_dagor_composite_file(
                _dagor_composite_filepath(
                    context.scene.mh_dagor_composite_import_path),
                source_root=_directory(preferences.source_root),
                output_dir=_optional_directory(
                    context.scene.mh_composite_export_directory),
                load_mode=_load_mode(context.scene),
                definition_policy=_definition_policy(context.scene),
            )
        except (OSError, RuntimeError, ValueError) as exc:
            _log("import_dagor_composite", {"ok": False, "error": str(exc)})
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log("import_dagor_composite", report)
        self.report({"INFO"}, "Dagor Composite converted")
        return {"FINISHED"}


class MH_OT_convert_dag4blend_composite(bpy.types.Operator):
    bl_idname = "mh.convert_dag4blend_composite"
    bl_label = "Convert dag4blend Scene Composite"
    bl_description = "Lift an imported dag4blend definition into MH authority"
    bl_options = {"REGISTER", "UNDO"}

    relink_external: bpy.props.BoolProperty(
        name="Relink External Placements",
        description="Explicitly redirect eligible working-scene placements to the converted MH definitions",
        default=False,
    )

    def invoke(self, context, _event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        collection = context.scene.mh_dag4blend_composite_collection
        if collection is None:
            self.report({"ERROR"}, "Choose a dag4blend composite collection")
            return {"CANCELLED"}
        try:
            preferences = prefs_mod.get_prefs(context)
            report = import_dag4blend_composite_collection(
                collection,
                source_root=_directory(preferences.source_root),
                load_mode=_load_mode(context.scene),
                definition_policy=_definition_policy(context.scene),
                relink_external=self.relink_external)
        except (OSError, RuntimeError, ValueError) as exc:
            _log("convert_dag4blend_composite", {
                "ok": False, "error": str(exc)})
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _log("convert_dag4blend_composite", report)
        self.report({"INFO"}, "dag4blend Composite converted")
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
    MH_OT_copy_all_textures_to_project,
    MH_OT_remap_all_textures_to_project,
    MH_OT_export_composite,
    MH_OT_export_composite_closure,
    MH_OT_export_composite_include_all,
    MH_OT_import_composite,
    MH_OT_import_dagor_composite,
    MH_OT_convert_dag4blend_composite,
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
    bpy.types.Scene.mh_import_load_mode = bpy.props.EnumProperty(
        name="Load Mode",
        items=(
            ("FULL_LOD", "Full LOD", "Import every authored LOD"),
            ("LOD0", "LOD0", "Import lod00 plus collisions, sockets and groups"),
            ("STRUCTURE_ONLY", "Structure Only", "Create empty definitions"),
        ),
        default="FULL_LOD")
    bpy.types.Scene.mh_import_definition_policy = bpy.props.EnumProperty(
        name="Definitions",
        items=(
            ("REUSE", "Reuse", "Reuse only complete managed definitions"),
            ("REFRESH", "Refresh", "Replace contents and preserve Collection IDs"),
        ),
        default="REUSE")
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
    bpy.types.Scene.mh_dagor_composite_import_path = bpy.props.StringProperty(
        name="Dagor Composite", subtype="FILE_PATH", default="")
    bpy.types.Scene.mh_dag4blend_composite_collection = bpy.props.PointerProperty(
        name="dag4blend Collection", type=bpy.types.Collection)
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
        "mh_dag4blend_composite_collection", "mh_dagor_composite_import_path",
        "mh_composite_import_path", "mh_composite_mode",
        "mh_import_definition_policy", "mh_import_load_mode",
        "mh_fbx_export_materials", "mh_fbx_import_path",
        "mh_fbx_directory", "mh_fbx_collection",
        "mh_material_directory", "mh_material",
    ):
        if hasattr(bpy.types.Scene, name):
            delattr(bpy.types.Scene, name)
    for cls in reversed(PROPERTY_CLASSES):
        bpy.utils.unregister_class(cls)
