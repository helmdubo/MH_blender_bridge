"""Minimal addon preferences for standalone source workflows."""

import bpy


class MHAddonPreferences(bpy.types.AddonPreferences):
    bl_idname = __package__

    source_root: bpy.props.StringProperty(
        name="Project Source Root",
        description="Source Protocol v4 resource-tree root",
        subtype="DIR_PATH",
        default="",
    )

    def draw(self, _context):
        layout = self.layout
        layout.prop(self, "source_root")


def get_prefs(context):
    return context.preferences.addons[__package__].preferences


def register():
    bpy.utils.register_class(MHAddonPreferences)


def unregister():
    bpy.utils.unregister_class(MHAddonPreferences)
