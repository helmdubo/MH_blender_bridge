"""One MH sidebar tab with separate source-workflow sections."""

import re

import bpy

from .. import prefs as prefs_mod
from ..scene.export_material import material_class_for_export
from .composite_authoring import draw_random_options


_TOKEN_RE = re.compile(r"^[a-z0-9_]+$")


class MH_PT_source_tools(bpy.types.Panel):
    bl_label = "MH Source Tools"
    bl_idname = "MH_PT_source_tools"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "MH"

    def draw(self, context):
        layout = self.layout
        scene = context.scene

        preferences = prefs_mod.get_prefs(context)
        project = layout.box()
        project.label(text="Project", icon="FILE_FOLDER")
        project.prop(preferences, "source_root", text="Source Root")

        fbx = layout.box()
        fbx.label(text="Mesh FBX", icon="MESH_CUBE")
        fbx.prop(scene, "mh_fbx_import_path", text="Import File")
        fbx.prop(scene, "mh_import_load_mode", text="Geometry")
        fbx.prop(scene, "mh_import_definition_policy", text="Definitions")
        fbx.operator("mh.import_mesh_fbx", icon="IMPORT")
        fbx.separator()
        fbx.prop(scene, "mh_fbx_collection", text="Collection")
        fbx.prop(scene, "mh_fbx_directory", text="Folder")
        fbx.prop(scene, "mh_fbx_export_materials", text="Export Materials")
        fbx.operator("mh.export_fbx", icon="EXPORT")

        box = layout.box()
        box.label(text="Composites", icon="OUTLINER_COLLECTION")
        box.prop(scene, "mh_composite_mode", expand=True)
        if scene.mh_composite_mode == "IMPORT":
            box.prop(scene, "mh_composite_import_path", text="File")
            box.prop(scene, "mh_import_load_mode", text="Geometry")
            box.prop(scene, "mh_import_definition_policy", text="Definitions")
            box.operator("mh.import_composite", icon="IMPORT")
            dagor = box.box()
            dagor.label(text="Dagor → MH", icon="FILE_REFRESH")
            dagor.prop(
                scene, "mh_dagor_composite_import_path", text=".composit.blk")
            dagor.prop(
                scene, "mh_composite_export_directory", text="MH Output Folder")
            dagor.operator("mh.import_dagor_composite", icon="IMPORT")
            dagor.separator()
            dagor.prop(
                scene, "mh_dag4blend_composite_collection",
                text="dag4blend Collection")
            dagor.operator(
                "mh.convert_dag4blend_composite",
                icon="OUTLINER_COLLECTION")
        else:
            box.prop(
                scene, "mh_composite_export_collection", text="Collection")
            box.prop(
                scene, "mh_composite_export_directory", text="Folder")
            box.operator("mh.export_composite", icon="EXPORT")
            box.operator("mh.export_composite_closure", icon="EXPORT")
            box.operator("mh.export_composite_include_all", icon="EXPORT")
        draw_random_options(box, context)

        materials = layout.box()
        materials.label(text="Materials", icon="MATERIAL")
        materials.prop(scene, "mh_material", text="Material")
        materials.prop(
            scene, "mh_material_directory", text="Folder (first export)")
        material = scene.mh_material
        if material is not None:
            settings = material.mh4blend
            materials.prop(settings, "mode", expand=True)
            if settings.mode == "LIBRARY":
                library = materials.row()
                library.alert = _TOKEN_RE.fullmatch(settings.library) is None
                library.prop(settings, "library")
                if library.alert:
                    materials.label(
                        text="Library must match [a-z0-9_]+",
                        icon="ERROR")
                materials.label(
                    text="Library form has no local overrides", icon="INFO")
            else:
                effective_class = material_class_for_export(material)
                material_class = materials.row()
                material_class.alert = (
                    _TOKEN_RE.fullmatch(effective_class) is None)
                material_class.prop(
                    settings, "material_class", text="Class Override")
                if material_class.alert:
                    materials.label(
                        text="Class must match [a-z0-9_]+",
                        icon="ERROR")
                elif not settings.material_class:
                    materials.label(
                        text=f"Auto from Dagor: {effective_class} + fields",
                        icon="CHECKMARK")
                row = materials.row(align=True)
                row.prop(settings, "twosided_override")
                value = row.row(align=True)
                value.enabled = settings.twosided_override
                value.prop(settings, "twosided")

                textures = materials.box()
                header = textures.row()
                header.label(text="Texture Overrides", icon="TEXTURE")
                header.operator(
                    "mh.material_texture_add", text="", icon="ADD")
                for index, texture in enumerate(settings.textures):
                    row = textures.row(align=True)
                    row.prop(texture, "slot", text="Slot")
                    row.prop(texture, "image", text="")
                    remove = row.operator(
                        "mh.material_texture_remove", text="", icon="REMOVE")
                    remove.index = index

                params = materials.box()
                header = params.row()
                header.label(text="Parameter Overrides", icon="PROPERTIES")
                header.operator("mh.material_param_add", text="", icon="ADD")
                for index, parameter in enumerate(settings.params):
                    row = params.row(align=True)
                    row.prop(parameter, "name", text="")
                    row.prop(parameter, "kind", text="")
                    if parameter.kind == "VECTOR":
                        row.prop(parameter, "vector", text="")
                    elif parameter.kind == "STRING":
                        row.prop(parameter, "string", text="")
                    elif parameter.kind == "BOOLEAN":
                        row.prop(parameter, "boolean", text="")
                    else:
                        row.prop(parameter, "scalar", text="")
                    remove = row.operator(
                        "mh.material_param_remove", text="", icon="REMOVE")
                    remove.index = index
        materials.operator("mh.export_material", icon="EXPORT")

        misc = layout.box()
        misc.label(text="Misc", icon="TOOL_SETTINGS")
        misc.operator("mh.copy_all_textures_to_project", icon="DUPLICATE")
        misc.operator("mh.remap_all_textures_to_project", icon="FILE_REFRESH")
        misc.label(text="Copy first, then remap paths", icon="INFO")

        layout.label(text="Log: Text Editor > mh_export_log", icon="TEXT")


CLASSES = (MH_PT_source_tools,)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
