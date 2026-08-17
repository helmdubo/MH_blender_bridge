"""N-panel (pattern reference: dag4blend cmp_panels)."""

import bpy

from .. import prefs as prefs_mod


class MH_PT_main(bpy.types.Panel):
    bl_label = "MH Composite"
    bl_idname = "MH_PT_main"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "MH"

    def draw(self, context):
        layout = self.layout
        prefs = prefs_mod.get_prefs(context)

        box = layout.box()
        box.label(text="Project", icon="FILE_FOLDER")
        box.prop(prefs, "source_root", text="Source")
        box.prop(context.scene, "mh_bundle_subdir")

        texture_box = layout.box()
        texture_box.label(text="Materials", icon="MATERIAL")
        texture_box.prop(prefs, "texture_root", text="Textures")
        texture_box.prop(prefs, "legacy_texture_root", text="Old Root")
        texture_box.operator("mh.remap_texture_root", icon="FILE_REFRESH")

        col = layout.column(align=True)
        col.operator("mh.export_bundle", icon="EXPORT")
        col.operator("mh.validate", icon="CHECKMARK")
        layout.label(text="Log: Text Editor > mh_export_log", icon="TEXT")


def register():
    bpy.utils.register_class(MH_PT_main)


def unregister():
    bpy.utils.unregister_class(MH_PT_main)
