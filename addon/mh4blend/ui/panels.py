"""N-panel skeleton (pattern reference: dag4blend cmp_panels).

Export/Validate operators arrive in B10/B11; until then the panel shows
configuration state so the addon is installable from B1 on.
"""

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
        box.label(text="Project roots", icon="FILE_FOLDER")
        box.prop(prefs, "source_root", text="Source")
        box.prop(prefs, "texture_root", text="Textures")
        col = layout.column()
        col.enabled = False
        col.label(text="Export Bundle / Validate: stage B10-B11")


def register():
    bpy.utils.register_class(MH_PT_main)


def unregister():
    bpy.utils.unregister_class(MH_PT_main)
