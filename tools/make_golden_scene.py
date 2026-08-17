"""Generate golden/golden.blend — the deterministic golden scene for stage A.

Structure (docs/02_mvp_plan.md §A2):

    GEOMETRY:  wall_a, wall_b, window_a (asymmetric!), decal_leak (role=decal)
    COMPOSITS: CA_WindowSet  (3 x window_a placements, one with scale+rotation)
               CA_Building   (wall_a, wall_b, decal_leak, 2 x ref CA_WindowSet,
                              Empty-group with children, nesting depth 2)

Run headless either way:
    blender -b -P tools/make_golden_scene.py
    python3 tools/make_golden_scene.py        # pip `bpy` module, Blender 4.5+

The scene is fully deterministic: fixed geometry, fixed transforms, UIDs from
tools/golden_uids.py (UUID5 table — test-only mechanism, production uses lazy
UUID4 per docs/01_bundle_schema_v1.md §4).
"""

import math
import os
import sys

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from golden_uids import UIDS  # noqa: E402

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTPUT_PATH = os.path.join(REPO_ROOT, "golden", "golden.blend")

# Control vertex of the asymmetric window_a mesh, local space, meters.
# This is the reference point of the R1 axis go/no-go test (docs/00 §4):
# its world position after any placement must match Blender == UE.
CONTROL_VERTEX = (0.37, 0.11, 1.93)


def make_box_mesh(name, sx, sy, sz):
    """Axis-aligned box, origin at bottom center, dimensions in meters."""
    hx, hy = sx / 2.0, sy / 2.0
    verts = [
        (-hx, -hy, 0.0), (hx, -hy, 0.0), (hx, hy, 0.0), (-hx, hy, 0.0),
        (-hx, -hy, sz), (hx, -hy, sz), (hx, hy, sz), (-hx, hy, sz),
    ]
    faces = [
        (0, 1, 2, 3), (7, 6, 5, 4), (0, 4, 5, 1),
        (1, 5, 6, 2), (2, 6, 7, 3), (3, 7, 4, 0),
    ]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.validate()
    return mesh


def make_window_mesh(name):
    """Asymmetric mesh: box frame 1.0 x 0.1 x 1.2 plus a prong pulled out of
    one top corner. Asymmetric along all three axes — mirrored or axis-swapped
    imports cannot silently look correct. Vertex 8 is CONTROL_VERTEX."""
    verts = [
        (-0.5, -0.05, 0.0), (0.5, -0.05, 0.0), (0.5, 0.05, 0.0), (-0.5, 0.05, 0.0),
        (-0.5, -0.05, 1.2), (0.5, -0.05, 1.2), (0.5, 0.05, 1.2), (-0.5, 0.05, 1.2),
        CONTROL_VERTEX,  # index 8: prong tip
    ]
    faces = [
        (0, 1, 2, 3), (7, 6, 5, 4), (0, 4, 5, 1),
        (1, 5, 6, 2), (2, 6, 7, 3), (3, 7, 4, 0),
        (5, 6, 8),  # prong triangle off the top edge, +X +Y side
    ]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.validate()
    return mesh


def make_decal_mesh(name):
    """Single quad 0.5 x 0.8 in the XZ plane (faces -Y), leak stain decal."""
    verts = [
        (-0.25, 0.0, 0.0), (0.25, 0.0, 0.0), (0.25, 0.0, 0.8), (-0.25, 0.0, 0.8),
    ]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], [(0, 1, 2, 3)])
    mesh.validate()
    return mesh


def add_geometry_resource(scene, col_name, mesh, key):
    """One GEOMETRY collection = one mesh resource (one FBX = one UStaticMesh)."""
    col = bpy.data.collections.new(col_name)
    scene.collection.children.link(col)
    col["mh_uid"] = UIDS[f"col/{key}"]
    obj = bpy.data.objects.new(col_name, mesh)
    obj["mh_uid"] = UIDS[f"obj/{key}"]
    mesh["mh_uid"] = UIDS[f"mesh/{key}"]
    col.objects.link(obj)
    return col


def make_material(name, key):
    """Create a deterministic plain Blender material.

    The golden scene deliberately does not register the reference dag4blend
    addon: B4 must export ordinary Blender materials as rendinst_simple
    placeholders as well as parse dagormat when that addon is enabled.
    """
    material = bpy.data.materials.new(name)
    material["mh_uid"] = UIDS[f"material/{key}"]
    return material


def add_placement(comp_col, name, uid_key, instance_col=None, location=(0, 0, 0),
                  rotation_euler=(0, 0, 0), scale=(1, 1, 1), parent=None):
    """Empty placement node. instance_col=None -> kind=group (transform-only)."""
    obj = bpy.data.objects.new(name, None)
    obj["mh_uid"] = UIDS[uid_key]
    if instance_col is not None:
        obj.instance_type = "COLLECTION"
        obj.instance_collection = instance_col
    obj.location = location
    obj.rotation_euler = rotation_euler
    obj.scale = scale
    if parent is not None:
        # Plain parenting, identity matrix_parent_inverse: the node's local
        # transform relative to its parent is exactly obj.location/rotation/scale.
        obj.parent = parent
    comp_col.objects.link(obj)
    return obj


def build():
    # Start from an empty file: no default cube/camera/light, no default scene.
    bpy.ops.wm.read_factory_settings(use_empty=True)

    scene_geo = bpy.data.scenes.new("GEOMETRY")
    scene_cmp = bpy.data.scenes.new("COMPOSITS")
    # Drop the factory default scene, keep only ours.
    for scene in list(bpy.data.scenes):
        if scene not in (scene_geo, scene_cmp):
            bpy.data.scenes.remove(scene)

    # --- GEOMETRY ---------------------------------------------------------
    col_wall_a = add_geometry_resource(
        scene_geo, "wall_a", make_box_mesh("wall_a", 4.0, 0.3, 3.0), "wall_a")
    col_wall_b = add_geometry_resource(
        scene_geo, "wall_b", make_box_mesh("wall_b", 2.0, 0.3, 3.0), "wall_b")
    col_window_a = add_geometry_resource(
        scene_geo, "window_a", make_window_mesh("window_a"), "window_a")
    col_decal = add_geometry_resource(
        scene_geo, "decal_leak", make_decal_mesh("decal_leak"), "decal_leak")

    # Second object in one resource: B4 must keep a deterministic aggregate
    # material-slot table for multi-object FBX export.
    trim_mesh = make_box_mesh("wall_a_trim", 0.5, 0.35, 0.25)
    trim_mesh["mh_uid"] = UIDS["mesh/wall_a_trim"]
    trim_obj = bpy.data.objects.new("wall_a_trim", trim_mesh)
    trim_obj["mh_uid"] = UIDS["obj/wall_a_trim"]
    trim_obj.location = (0.0, 0.0, 3.1)
    col_wall_a.objects.link(trim_obj)

    # Material coverage: shared identity across two mesh resources and two
    # ordered slots on one mesh resource.  Face assignment makes the slot
    # order semantically relevant to the FBX while metadata remains external.
    mat_wall = make_material("m_wall_shared", "wall_shared")
    mat_accent = make_material("m_accent", "accent")
    col_wall_a.objects[0].data.materials.append(mat_wall)
    col_wall_a.objects[0].data.materials.append(mat_accent)
    for index, polygon in enumerate(col_wall_a.objects[0].data.polygons):
        polygon.material_index = index % 2
    col_wall_b.objects[0].data.materials.append(mat_wall)
    col_window_a.objects[0].data.materials.append(mat_accent)
    col_decal.objects[0].data.materials.append(mat_accent)
    trim_mesh.materials.append(mat_accent)
    # Properties bag storage per QUESTION-6 decision: one custom prop per
    # schema key, prefixed mh_p_, full key with dots as is (mh_p_role,
    # mh_p_render.cast_shadow, ...). Everything under mh_p_ exports verbatim.
    col_decal["mh_p_role"] = "decal"

    # --- COMPOSITS: CA_WindowSet -----------------------------------------
    ca_windowset = bpy.data.collections.new("CA_WindowSet")
    scene_cmp.collection.children.link(ca_windowset)
    ca_windowset["mh_uid"] = UIDS["col/ca_windowset"]

    add_placement(ca_windowset, "window_1", "node/ca_windowset/window_1",
                  col_window_a, location=(0.0, 0.0, 1.0))
    add_placement(ca_windowset, "window_2", "node/ca_windowset/window_2",
                  col_window_a, location=(1.5, 0.0, 1.0))
    # The one with scale+rotation (02_mvp_plan.md §A2).
    add_placement(ca_windowset, "window_3", "node/ca_windowset/window_3",
                  col_window_a, location=(3.0, 0.2, 1.1),
                  rotation_euler=(0.0, 0.0, math.radians(15.0)),
                  scale=(1.2, 1.2, 1.2))

    # --- COMPOSITS: CA_Building ------------------------------------------
    ca_building = bpy.data.collections.new("CA_Building")
    scene_cmp.collection.children.link(ca_building)
    ca_building["mh_uid"] = UIDS["col/ca_building"]

    add_placement(ca_building, "wall_main", "node/ca_building/wall_main",
                  col_wall_a, location=(0.0, 0.0, 0.0))
    add_placement(ca_building, "wall_side", "node/ca_building/wall_side",
                  col_wall_b, location=(2.0, 2.0, 0.0),
                  rotation_euler=(0.0, 0.0, math.radians(90.0)))
    add_placement(ca_building, "leak_decal", "node/ca_building/leak_decal",
                  col_decal, location=(1.0, -0.16, 1.4))
    # Two composite_ref placements of the nested composite.
    add_placement(ca_building, "windows_front", "node/ca_building/windows_front",
                  ca_windowset, location=(0.0, -0.2, 0.0))
    add_placement(ca_building, "windows_back", "node/ca_building/windows_back",
                  ca_windowset, location=(4.0, 4.2, 0.0),
                  rotation_euler=(0.0, 0.0, math.radians(180.0)))
    # Empty-group (kind=group) with a NON-identity transform, so that
    # reparenting under it changes children's local transforms — the
    # reparent_node mutation must produce {REPARENT, UPDATE_TRANSFORM}.
    props_group = add_placement(
        ca_building, "props_group", "node/ca_building/props_group",
        None, location=(5.0, 1.0, 0.5),
        rotation_euler=(0.0, 0.0, math.radians(45.0)))
    # Children of the group: nesting depth 2 inside CA_Building.
    add_placement(ca_building, "gp_wall", "node/ca_building/gp_wall",
                  col_wall_b, location=(0.5, 0.0, 0.0), parent=props_group)
    add_placement(ca_building, "gp_decal", "node/ca_building/gp_decal",
                  col_decal, location=(0.0, 0.4, 0.3), parent=props_group)

    # --- Save -------------------------------------------------------------
    os.makedirs(os.path.dirname(OUTPUT_PATH), exist_ok=True)
    bpy.context.window.scene = scene_cmp if bpy.context.window else None
    bpy.ops.wm.save_as_mainfile(filepath=OUTPUT_PATH, compress=False)
    print(f"golden scene written: {OUTPUT_PATH}")
    print(f"scenes: {[s.name for s in bpy.data.scenes]}")
    print(f"collections: {sorted(c.name for c in bpy.data.collections)}")
    print(f"objects: {sorted(o.name for o in bpy.data.objects)}")


if __name__ == "__main__":
    build()
