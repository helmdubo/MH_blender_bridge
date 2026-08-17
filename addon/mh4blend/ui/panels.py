"""Two explicit N-panel workflows: FBX and Composite sources."""

import bpy


class MH_PT_fbx_export(bpy.types.Panel):
    bl_label = "FBX Export"
    bl_idname = "MH_PT_fbx_export"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "MH FBX"

    def draw(self, context):
        layout = self.layout
        layout.prop(context.scene, "mh_fbx_collection", text="Collection")
        layout.prop(context.scene, "mh_fbx_directory", text="Folder")
        layout.separator()
        layout.operator("mh.export_fbx", icon="EXPORT")
        layout.label(text="Log: Text Editor > mh_export_log", icon="TEXT")


class MH_PT_composite_io(bpy.types.Panel):
    bl_label = "Composite Sources"
    bl_idname = "MH_PT_composite_io"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "MH Composite"

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        row = layout.row(align=True)
        row.prop(scene, "mh_composite_mode", expand=True)
        box = layout.box()
        if scene.mh_composite_mode == "IMPORT":
            box.prop(scene, "mh_composite_import_path", text="File")
            box.operator("mh.import_composite", icon="IMPORT")
            box.label(text="Dependencies: sibling export_manifest.json")
        else:
            box.prop(
                scene, "mh_composite_export_collection", text="Collection")
            box.prop(
                scene, "mh_composite_export_directory", text="Folder")
            box.operator("mh.export_composite", icon="EXPORT")
            box.label(text="Empty Collection Instances = nodes")
        layout.label(text="Log: Text Editor > mh_export_log", icon="TEXT")


CLASSES = (MH_PT_fbx_export, MH_PT_composite_io)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
