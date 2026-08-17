"""Minimal addon preferences for standalone source workflows."""

import bpy


class MHAddonPreferences(bpy.types.AddonPreferences):
    bl_idname = __package__

    registry_path: bpy.props.StringProperty(
        name="Registry (from UE)",
        description="Optional UE shader registry used by future validation",
        subtype="FILE_PATH",
        default="",
    )

    def draw(self, _context):
        self.layout.prop(self, "registry_path")


def get_prefs(context):
    return context.preferences.addons[__package__].preferences


def register():
    bpy.utils.register_class(MHAddonPreferences)


def unregister():
    bpy.utils.unregister_class(MHAddonPreferences)
