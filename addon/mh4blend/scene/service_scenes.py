"""Explicit helpers for the four Source Protocol v5 Blender service scenes.

Importing or registering mh4blend must never mutate the current blend.  Scene
creation therefore happens only through the explicit ``ensure_*`` call.
"""

import bpy


SERVICE_SCENE_NAMES = (
    "COMPOSITE",
    "MESH",
    "ACTOR_PLACEHOLDERS",
    "TECH",
)


def _validated_name(name):
    if name not in SERVICE_SCENE_NAMES:
        raise ValueError(
            "MH_E_COMPOSITE_GRAMMAR: unsupported service scene "
            f"{name!r}; expected one of {SERVICE_SCENE_NAMES!r}")
    return name


def get_service_scene(name, *, create=False):
    """Return one exact service scene, optionally creating it explicitly."""

    name = _validated_name(name)
    scene = bpy.data.scenes.get(name)
    if scene is None and create:
        scene = bpy.data.scenes.new(name)
    return scene


def ensure_service_scenes():
    """Explicitly ensure and return the exact four named service scenes."""

    return {
        name: get_service_scene(name, create=True)
        for name in SERVICE_SCENE_NAMES
    }
