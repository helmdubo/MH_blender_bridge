"""mh4blend — Blender addon of the Mimir Composite Pipeline (stage B).

Package layout rule: `mh4blend.core.*` is pure Python (importable without
Blender — pytest runs against it directly); everything that touches bpy
lives in `prefs`/`ui`/`ops` and is imported lazily from register(), so
`import mh4blend.core.canonical` never drags Blender in.
"""

bl_info = {
    "name": "mh4blend (Mimir Composite Pipeline)",
    "author": "MH",
    "version": (0, 3, 0),
    "blender": (4, 5, 0),
    "location": "3D Viewport > Sidebar > MH FBX / MH Composite",
    "description": "Standalone FBX and composite source I/O for UE5",
    "category": "Import-Export",
}


def _bpy_modules():
    from . import prefs
    from .ui import ops, panels
    return (prefs, ops, panels)


def register():
    for module in _bpy_modules():
        module.register()


def unregister():
    for module in reversed(_bpy_modules()):
        module.unregister()
