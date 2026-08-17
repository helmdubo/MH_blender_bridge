"""Minimal addon preferences for standalone source workflows."""

import bpy


class MHAddonPreferences(bpy.types.AddonPreferences):
    bl_idname = __package__

    source_root: bpy.props.StringProperty(
        name="Project Source Root",
        description="Project root used to resolve resource UIDs and texture paths",
        subtype="DIR_PATH",
        default="",
    )

    texture_policy: bpy.props.EnumProperty(
        name="External Textures",
        description="How material export handles texture paths outside Source Root",
        items=(
            ("transitional", "Transitional", "Allow with a warning"),
            ("strict", "Strict", "Block export"),
        ),
        default="transitional",
    )

    registry_path: bpy.props.StringProperty(
        name="Source Registry (from UE)",
        description="Optional UID and shader-class hint registry from UE",
        subtype="FILE_PATH",
        default="",
    )

    def draw(self, _context):
        layout = self.layout
        layout.prop(self, "source_root")
        layout.prop(self, "texture_policy")
        layout.prop(self, "registry_path")


def get_prefs(context):
    return context.preferences.addons[__package__].preferences


def register():
    bpy.utils.register_class(MHAddonPreferences)


def unregister():
    bpy.utils.unregister_class(MHAddonPreferences)
