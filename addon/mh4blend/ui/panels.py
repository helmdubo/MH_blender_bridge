"""One MH sidebar tab with separate source-workflow sections."""

import bpy

from .. import prefs as prefs_mod


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
        project.prop(preferences, "texture_policy", text="External Textures")

        fbx = layout.box()
        fbx.label(text="FBX Export", icon="MESH_CUBE")
        fbx.prop(scene, "mh_fbx_collection", text="Collection")
        fbx.prop(scene, "mh_fbx_directory", text="Folder")
        fbx.prop(scene, "mh_fbx_export_materials", text="Export Materials")
        fbx.operator("mh.export_fbx", icon="EXPORT")

        box = layout.box()
        box.label(text="Composites", icon="OUTLINER_COLLECTION")
        box.prop(scene, "mh_composite_mode", expand=True)
        if scene.mh_composite_mode == "IMPORT":
            box.prop(scene, "mh_composite_import_path", text="File")
            box.operator("mh.import_composite", icon="IMPORT")
            box.label(text="Recursive composites and FBX: always on")
        else:
            box.prop(
                scene, "mh_composite_export_collection", text="Collection")
            box.prop(
                scene, "mh_composite_export_directory", text="Folder")
            box.operator("mh.export_composite", icon="EXPORT")
            box.label(text="Empty Collection Instances = nodes")

        materials = layout.box()
        materials.label(text="Materials", icon="MATERIAL")
        materials.prop(scene, "mh_material", text="Material")
        materials.prop(
            scene, "mh_material_directory", text="Folder (first export)")
        materials.operator("mh.export_material", icon="EXPORT")
        layout.label(text="Log: Text Editor > mh_export_log", icon="TEXT")


CLASSES = (MH_PT_source_tools,)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
