"""Standalone FBX export for one explicitly selected Blender collection.

The collection is treated like dag4blend's ``Col.Joined`` mode: direct mesh
objects and mesh objects in recursive child collections form one static-mesh
resource.  Sibling collections and unrelated scene objects are never selected.
No scene names, bundle directories or texture roots participate in this API.
"""

import contextlib
from dataclasses import dataclass, replace
import hashlib
import os
from pathlib import Path
import re
import tempfile

import bpy
from mathutils import Matrix

from ..core.canonical import validate_resource_name
from ..core.mesh_nodes import (
    COLLISION_TRANSPORT_KINDS,
    FBX_COLLISION_KIND_KEY,
    FBX_COLLISION_SHAPE_KEY,
    FBX_PHMAT_KEY,
    normalize_collision_transport,
    strip_blender_duplicate_suffix,
    validate_node_markers,
)
from ..core.payload_publish_v2 import atomic_publish_bytes
from ..core.source_inventory import scan_source_inventory
from ..core.validate import MHValidationError
from .export_material import (
    _active_material_export_session,
    is_technical_material,
    material_export_session,
    resolve_material_binding,
)
from .resource_markers import (
    COLLECTION_KIND_KEY,
    COLLECTION_RESOURCE_KEY,
    INCOMPLETE_IMPORT_KEY,
    stamp_resource_collection,
)

__all__ = [
    "FBX_EXPORT_KWARGS",
    "PreparedFBXExport",
    "StagedFBXExport",
    "collect_collection_mesh_objects",
    "export_fbx_collection",
    "prepare_fbx_collection",
    "stage_prepared_fbx",
]


# Canonical UE transport settings.  This module is the standalone owner of the
# settings; the legacy aggregate exporter can import them while it is retired.
FBX_EXPORT_KWARGS = dict(
    use_selection=True,
    # V5-S6.1.2 (docs/15 §3.4): the collision carrier travels as FBX Model user
    # properties, which Blender only writes with this flag.  The writer strips
    # the reserved `mh_` object-property namespace from every transported node
    # first, so authored properties can neither leak nor forge a claim.
    use_custom_props=True,
    use_mesh_modifiers=True,
    object_types={"MESH", "EMPTY"},
    bake_anim=False,
    apply_unit_scale=True,
    apply_scale_options="FBX_SCALE_UNITS",
    global_scale=1.0,
    use_space_transform=True,
    bake_space_transform=False,
    add_leaf_bones=False,
    axis_forward="X",
    axis_up="Z",
)

BLENDER_METERS_TO_UE_CENTIMETERS = 100.0
_LODS_ROOT_RE = re.compile(
    r"^(?P<base>.+)\.lods(?:\.\d{3})?$")
_LOD_CHILD_RE_TEMPLATE = r"^{base}\.lod(?P<level>\d{{2}})(?:\.\d{{3}})?$"
_LOD_LEAF_RE = re.compile(
    r"^(?P<base>.+)\.lod(?P<level>\d{2})(?:\.\d{3})?$")
# dag4blend spells the Dagor collision marker with any of `_`, `.` or ` ` as
# the separator (`body_cls_phys`, `gaz53_a_bumper.lod01 cls.002`).
_DAGOR_COLLISION_TOKEN_RE = re.compile(r"(?:^|[ ._])cls(?:[ ._]|$)")
_DAGOR_COLLISION_ROLE_RES = {
    role: re.compile(rf"(?:^|[ ._]){role}(?:[ ._]|$)")
    for role in COLLISION_TRANSPORT_KINDS
}
# Dagor script{} keys, spelled exactly as dag4blend preserves them (docs/15
# §3.1/§3.4).  Lookup is case-insensitive and `dagorprops` outranks a plain ID
# property, mirroring the ratified `place_type` claim mechanism.
_DAGOR_SHAPE_KEY = "collision:t"
_DAGOR_PHMAT_KEY = "phmat:t"
_DAGOR_PHYS_COLLIDABLE_KEY = "isphyscollidable:b"
_DAGOR_TRACEABLE_KEY = "istraceable:b"
_DAGOR_COLLISION_KEYS = frozenset({
    _DAGOR_SHAPE_KEY, _DAGOR_PHMAT_KEY,
    _DAGOR_PHYS_COLLIDABLE_KEY, _DAGOR_TRACEABLE_KEY,
})
_TRUE_TEXTS = frozenset({"yes", "true", "on", "1"})
_FALSE_TEXTS = frozenset({"no", "false", "off", "0"})


@dataclass(frozen=True)
class DagorCollisionTransport:
    """One recognized Dagor collision mesh and its resolved FBX carrier."""

    obj: object
    kind: str
    shape: str
    phmat: str | None
    notes: tuple


def _grammar_error(subjects, message):
    return MHValidationError("MH_E_COMPOSITE_GRAMMAR", subjects, message)


def _mapping_keys(mapping):
    if mapping is None:
        return ()
    try:
        return tuple(key for key in mapping.keys() if isinstance(key, str))
    except (AttributeError, TypeError):
        return ()


def _dagor_settings(obj):
    """Return the saved dag4blend property bag, or None.

    ``dagorprops`` is a dynamic key/value bag rather than an MH typed carrier,
    and it survives in a saved ``.blend`` even when dag4blend RNA is absent.
    """
    value = obj.get("dagorprops")
    if value is not None and not hasattr(value, "keys"):
        raise MHValidationError(
            "MH_E_INVALID_RESOURCE_SOURCE", [obj.name, "dagorprops"],
            "saved dagorprops must be a key/value mapping")
    return value


def _dagor_collision_claims(obj):
    """Collect the collision declarations, ``dagorprops`` winning."""
    claims = {}
    for mapping in (obj, _dagor_settings(obj)):
        for key in _mapping_keys(mapping):
            folded = key.casefold()
            if folded not in _DAGOR_COLLISION_KEYS:
                continue
            try:
                claims[folded] = (key, mapping[key])
            except (KeyError, TypeError):
                continue
    return claims


def _dagor_text(obj, claim):
    """Return one Dagor `:t` declaration as a bare token.

    dag4blend preserves BLK text values with their source quoting intact
    (`collision:t` reads literally as ``"mesh"``), so one surrounding pair of
    double quotes is part of the wire format rather than of the token.
    """
    key, value = claim
    if not isinstance(value, str):
        raise _grammar_error(
            [obj.name, key],
            f"'{obj.name}' declares {key} as {type(value).__name__}; a Dagor "
            "text declaration must be a string")
    token = value.strip()
    if len(token) >= 2 and token.startswith('"') and token.endswith('"'):
        token = token[1:-1].strip()
    if not token:
        raise _grammar_error(
            [obj.name, key], f"'{obj.name}' declares an empty {key}")
    return token


def _dagor_flag(obj, claim):
    key, value = claim
    if isinstance(value, bool):
        return value
    if isinstance(value, int) and value in (0, 1):
        return bool(value)
    if isinstance(value, str):
        folded = value.strip().strip('"').casefold()
        if folded in _TRUE_TEXTS:
            return True
        if folded in _FALSE_TEXTS:
            return False
    raise _grammar_error(
        [obj.name, key],
        f"'{obj.name}' declares {key}={value!r}; a Dagor boolean declaration "
        "must be yes/no")


def _dagor_collision_name_role(obj):
    """Return the collision role spelled in the node name, if exactly one is."""
    name = strip_blender_duplicate_suffix(obj.name)
    roles = [role for role, pattern in _DAGOR_COLLISION_ROLE_RES.items()
             if pattern.search(name) is not None]
    return roles[0] if len(roles) == 1 else None


def _classify_dagor_collision(obj):
    """Resolve one recognized `cls` mesh into transport, or None to drop it.

    Owner semantics (docs/15 §3.2) ratify exactly two roles: ``phys`` becomes
    UE Simple Collision, ``trace`` becomes UE Complex Collision.  The doc
    spells them as name tokens, but real dag4blend content proves the name is
    only a convention: 52 of the 54 GAZ-53 collision meshes carry no role token
    and declare the role through the equally ratified `isPhysCollidable:b` /
    `isTraceable:b` script keys instead (docs/15 §3.1).  The name token stays
    authoritative where both exist; the declaration classifies where the name
    is silent.  **Executor rule** — reported with the slice.

    A `cls` mesh that resolves to neither role keeps the S6.1.1
    drop-with-warning: no third collision meaning is invented here.
    """
    claims = _dagor_collision_claims(obj)
    phys_claim = claims.get(_DAGOR_PHYS_COLLIDABLE_KEY)
    trace_claim = claims.get(_DAGOR_TRACEABLE_KEY)
    is_phys = None if phys_claim is None else _dagor_flag(obj, phys_claim)
    is_trace = None if trace_claim is None else _dagor_flag(obj, trace_claim)
    declared = None
    if bool(is_phys) != bool(is_trace):
        declared = "phys" if is_phys else "trace"

    notes = []
    named = _dagor_collision_name_role(obj)
    kind = named or declared
    if kind is None:
        if is_phys or is_trace:
            notes.append(
                f"'{obj.name}' declares isPhysCollidable:b={is_phys} and "
                f"isTraceable:b={is_trace}; that combination has no ratified "
                "UE collision meaning")
        return None, tuple(notes)
    if named is not None:
        contradicting = (
            (named == "phys" and is_phys is False)
            or (named == "trace" and is_trace is False)
            or (declared is not None and declared != named))
        if contradicting:
            notes.append(
                f"'{obj.name}' name declares '{named}' collision while "
                f"isPhysCollidable:b={is_phys} / isTraceable:b={is_trace} "
                "declare otherwise; the name token is authoritative")

    shape_claim = claims.get(_DAGOR_SHAPE_KEY)
    phmat_claim = claims.get(_DAGOR_PHMAT_KEY)
    kind, shape, phmat = normalize_collision_transport(
        obj.name,
        kind,
        None if shape_claim is None else _dagor_text(obj, shape_claim),
        None if phmat_claim is None else _dagor_text(obj, phmat_claim),
    )
    return (
        DagorCollisionTransport(
            obj=obj, kind=kind, shape=shape, phmat=phmat, notes=tuple(notes)),
        tuple(notes),
    )


@dataclass(frozen=True)
class PreparedFBXExport:
    """Fully validated, write-free snapshot of one mesh export request.

    Blender datablocks remain referenced because the FBX operator consumes
    them, but the carrier itself is immutable and every sequence is frozen.
    Callers must stage promptly; a closure-level preflight owns the policy for
    preventing artist edits between prepare and stage.
    """

    collection: object
    scene: object
    target: Path
    source_root: object
    resource_name: str
    export_objects: tuple
    payload_levels: tuple
    lod_levels: tuple
    uses_lod_hierarchy: bool
    materials: tuple
    prepared_materials: tuple
    warnings: tuple
    authority_fingerprint: tuple
    # V5-S6.1.2: the ``DagorCollisionTransport`` rows staged into the FBX.
    collision_transport: tuple = ()


@dataclass(frozen=True)
class StagedFBXExport:
    """An exact-byte-read-back FBX staged outside source authority."""

    filepath: Path
    payload: bytes


def _collection_objects(collection):
    """Return recursive, pointer-deduplicated collection membership."""
    objects = getattr(collection, "all_objects", collection.objects)
    result = []
    seen = set()
    for obj in objects:
        identity = obj.as_pointer()
        if identity in seen:
            continue
        seen.add(identity)
        result.append(obj)
    return result


def _is_static_mesh_aux(obj):
    """Recognize the UE-native aux nodes this dialect still transports."""
    return (
        obj.type == "MESH" and obj.name.startswith("UCX_")
    ) or (
        obj.type == "EMPTY" and obj.name.startswith("SOCKET_")
    )


def _is_dagor_collision_object(obj):
    """Recognize a Dagor collision mesh by its name marker or its paint.

    Real dag4blend content proves the name is not a reliable marker on its
    own: collision meshes arrive as ``gaz53_a_body.lod01 cls phys.001`` but
    also as plain ``Cube.774``.  What every one of them shares is the
    technical ``cls`` paint (Dagor shader ``gi_black``), so both facts
    classify (docs/15 §1.3/§2.2).
    """
    if obj.type != "MESH":
        return False
    if _DAGOR_COLLISION_TOKEN_RE.search(
            strip_blender_duplicate_suffix(obj.name)) is not None:
        return True
    materials = [slot.material for slot in obj.material_slots]
    return bool(materials) and all(
        is_technical_material(material) for material in materials)


def _collection_resource_objects(collection):
    """Split recursive membership into (geometry, aux, groups, collision).

    ``groups`` are organizational EMPTY objects (everything that is not a
    ``SOCKET_*`` aux). AMENDMENT_node_hierarchy: they are part of the FBX
    transport as plain null nodes so the authored hierarchy survives instead
    of Blender silently re-rooting children with baked world transforms.
    ``collision`` is recognized Dagor collision, which never joins the render
    payload; V5-S6.1.2 transports the ``phys``/``trace`` part of it as a
    separate collision carrier and still drops the rest.
    """
    members = _collection_objects(collection)
    aux = [obj for obj in members if _is_static_mesh_aux(obj)]
    aux_ids = {obj.as_pointer() for obj in aux}
    collision = [
        obj for obj in members
        if obj.as_pointer() not in aux_ids and _is_dagor_collision_object(obj)
    ]
    excluded_ids = aux_ids | {obj.as_pointer() for obj in collision}
    geometry = [
        obj for obj in members
        if obj.type == "MESH" and obj.as_pointer() not in excluded_ids
    ]
    groups = [
        obj for obj in members
        if obj.type == "EMPTY" and obj.as_pointer() not in excluded_ids
    ]
    return geometry, aux, groups, collision


def _collection_mesh_objects(collection):
    """Return recursive semantic geometry, excluding FBX aux nodes."""
    geometry, _aux, _groups, _collision = _collection_resource_objects(
        collection)
    return geometry


def _dagor_lod_structure(collection):
    """Recognize one dag4blend ``.lods`` collection resource.

    The dots in ``.lods``/``.lodNN`` belong to the Blender authoring
    hierarchy, not to the frozen resource-name grammar.  Only this exact
    adapter strips them; generic resource names remain strict.
    """
    root_match = _LODS_ROOT_RE.fullmatch(collection.name)
    if root_match is None:
        leaf_match = _LOD_LEAF_RE.fullmatch(collection.name)
        if leaf_match is not None:
            root_name = f"{leaf_match.group('base')}.lods"
            raise MHValidationError(
                "MH_E_INVALID_LOD_HIERARCHY",
                [collection.name, root_name],
                "selected collection "
                f"'{collection.name}' is one LOD level; select its group "
                f"collection '{root_name}' for FBX export")
        return None

    base = root_match.group("base")
    custom_name = collection.get("name")
    resource_name = base
    if isinstance(custom_name, str):
        # dag4blend may preserve a full source path ending in ``.lodNN.dag``
        # here. It is useful authoring metadata, but not a frozen-v1 resource
        # name. Only an already-valid plain name is an intentional override.
        try:
            validate_resource_name(custom_name)
        except (TypeError, ValueError):
            pass
        else:
            resource_name = custom_name
    # Keep the frozen canonical diagnostic and regex. This adapter only
    # removes the structural suffix from the fallback Blender name.
    try:
        validate_resource_name(resource_name)
    except (TypeError, ValueError) as exc:
        message = str(exc).partition(": ")[2] or str(exc)
        raise MHValidationError(
            "MH_E_NONCANONICAL_RESOURCE_NAME",
            [resource_name],
            message,
        ) from exc

    direct_root_meshes = [
        obj for obj in collection.objects if obj.type == "MESH"]
    if direct_root_meshes:
        names = ", ".join(sorted(obj.name for obj in direct_root_meshes))
        raise MHValidationError(
            "MH_E_INVALID_LOD_HIERARCHY",
            [collection.name, *(obj.name for obj in direct_root_meshes)],
            ".lods group contains mesh objects "
            f"outside a direct .lodNN collection: {names}")

    child_pattern = re.compile(
        _LOD_CHILD_RE_TEMPLATE.format(base=re.escape(base)))
    levels = {}
    for child in collection.children:
        match = child_pattern.fullmatch(child.name)
        if match is None:
            raise MHValidationError(
                "MH_E_INVALID_LOD_HIERARCHY",
                [collection.name, child.name],
                "every direct child of "
                f"'{collection.name}' must be named '{base}.lodNN' "
                f"(optional Blender .NNN duplicate suffix); got "
                f"'{child.name}'")
        level = int(match.group("level"))
        if level in levels:
            raise MHValidationError(
                "MH_E_INVALID_LOD_HIERARCHY",
                [levels[level].name, child.name],
                "duplicate authored LOD level "
                f"{level}: '{levels[level].name}' and '{child.name}'")
        levels[level] = child

    if 0 not in levels:
        raise MHValidationError(
            "MH_E_LOD_LEVELS_SPARSE",
            [collection.name],
            ".lods group requires one direct "
            f"'{base}.lod00' collection")
    missing_levels = sorted(set(range(max(levels) + 1)) - set(levels))
    if missing_levels:
        missing = ", ".join(f"lod{level:02d}" for level in missing_levels)
        raise MHValidationError(
            "MH_E_LOD_LEVELS_SPARSE",
            [collection.name, missing],
            "authored LOD levels must be "
            f"contiguous from lod00; missing {missing}")

    level_objects = []
    level0_aux = []
    ignored_aux = []
    excluded_collision = []
    excluded_collision_ids = set()
    level_aux_ids = set()
    object_level = {}
    for level, child in sorted(levels.items()):
        objects, aux, _child_groups, collision = _collection_resource_objects(
            child)
        level_aux_ids.update(obj.as_pointer() for obj in aux)
        for obj in collision:
            if obj.as_pointer() not in excluded_collision_ids:
                excluded_collision_ids.add(obj.as_pointer())
                excluded_collision.append(obj)
        if level == 0:
            level0_aux.extend(aux)
        elif aux:
            ignored_aux.extend((level, obj.name) for obj in aux)
        if not objects:
            raise MHValidationError(
                "MH_E_INVALID_LOD_HIERARCHY",
                [child.name],
                "authored LOD collection "
                f"'{child.name}' contains no mesh objects")
        for obj in objects:
            identity = obj.as_pointer()
            previous = object_level.get(identity)
            if previous is not None:
                raise MHValidationError(
                    "MH_E_INVALID_LOD_HIERARCHY",
                    [obj.name],
                    "mesh object "
                    f"'{obj.name}' belongs to both LOD {previous} and "
                    f"LOD {level}")
            object_level[identity] = level
        level_objects.append((level, child, objects))

    # Cross-level object parenting crosses independent FBX payload boundaries.
    # Make that authoring error explicit instead of letting the FBX exporter
    # silently drop or externalize the parent relationship.
    for level, _child, objects in level_objects:
        for obj in objects:
            parent = obj.parent
            if parent is None or parent.type != "MESH":
                continue
            parent_level = object_level.get(parent.as_pointer())
            if parent_level is not None and parent_level != level:
                raise MHValidationError(
                    "MH_E_INVALID_LOD_HIERARCHY",
                    [obj.name, parent.name],
                    "mesh object "
                    f"'{obj.name}' in LOD {level} is parented to "
                    f"'{parent.name}' in LOD {parent_level}")

    # Groups live at container scope: the recursive root gather also covers
    # empties authored directly in `.lods` or between level collections.
    _root_geometry, root_aux, groups, root_collision = (
        _collection_resource_objects(collection))
    for aux_obj in root_aux:
        if aux_obj.as_pointer() not in level_aux_ids:
            ignored_aux.append(("root", aux_obj.name))
    for obj in root_collision:
        if obj.as_pointer() not in excluded_collision_ids:
            excluded_collision_ids.add(obj.as_pointer())
            excluded_collision.append(obj)

    return {
        "resource_name": resource_name,
        "levels": level_objects,
        "level0_aux": level0_aux,
        "ignored_aux": ignored_aux,
        "excluded_collision": excluded_collision,
        "groups": groups,
    }


def collect_collection_mesh_objects(collection):
    """Return the selected collection's recursive, de-duplicated mesh set.

    Blender's ``Collection.all_objects`` is exactly the membership needed for
    dag4blend ``Col.Joined`` semantics.  Pointer de-duplication also makes the
    intent explicit for collections linked into more than one hierarchy.
    """
    if collection is None:
        raise ValueError("collection is required")
    return _collection_mesh_objects(collection)


def _scene_contains_collection(scene, collection):
    return (collection == scene.collection
            or collection in scene.collection.children_recursive)


def _find_export_scene(collection):
    current = bpy.context.scene
    if current is not None and _scene_contains_collection(current, collection):
        return current
    for scene in bpy.data.scenes:
        if _scene_contains_collection(scene, collection):
            return scene
    raise ValueError(
        f"collection '{collection.name}' is not linked to a Blender scene")


def _scale_matrix_translation(matrix, factor):
    scaled = matrix.copy()
    scaled.translation = scaled.translation * factor
    return scaled


def _transform_mesh_data(mesh, matrix):
    try:
        mesh.transform(matrix, shape_keys=True)
    except TypeError:
        mesh.transform(matrix)
    mesh.update()


@contextlib.contextmanager
def _temporary_ue_centimeter_export_state(objects):
    """Scale disposable geometry copies/translations x100 for UE.

    Mesh coordinates are stored by Blender as float32. Transforming the live
    datablock by x100 and then by x0.01 is therefore not reversible and would
    make a nominally read-only export slowly damage authoring geometry. Export
    objects are temporarily pointed at scaled Mesh copies instead; the exact
    original datablock pointers and object matrices are restored in ``finally``.
    """
    scene = bpy.context.scene
    unit_settings = scene.unit_settings
    saved_unit_state = (
        unit_settings.system,
        unit_settings.scale_length,
        unit_settings.length_unit,
    )

    object_states = []
    original_mesh_data = {}
    for obj in objects:
        if obj is None or obj.name not in bpy.data.objects:
            continue
        obj = bpy.data.objects[obj.name]
        object_states.append(
            (obj, obj.matrix_parent_inverse.copy(), obj.matrix_basis.copy()))
        if obj.type == "MESH" and obj.data is not None:
            original_mesh_data[obj] = obj.data

    scale = BLENDER_METERS_TO_UE_CENTIMETERS
    scale_matrix = Matrix.Scale(scale, 4)
    mesh_copies = {}
    try:
        for obj, original_mesh in original_mesh_data.items():
            export_mesh = mesh_copies.get(original_mesh)
            if export_mesh is None:
                export_mesh = original_mesh.copy()
                mesh_copies[original_mesh] = export_mesh
                # The copy is disposable, so authored Geometry ID properties
                # are removed outright instead of being published.
                for key in _authored_property_keys(export_mesh):
                    del export_mesh[key]
                _transform_mesh_data(export_mesh, scale_matrix)
            obj.data = export_mesh
        for obj, matrix_parent_inverse, matrix_basis in object_states:
            obj.matrix_parent_inverse = _scale_matrix_translation(
                matrix_parent_inverse, scale)
            obj.matrix_basis = _scale_matrix_translation(matrix_basis, scale)

        unit_settings.system = "METRIC"
        unit_settings.scale_length = 0.01
        try:
            unit_settings.length_unit = "CENTIMETERS"
        except TypeError:
            pass
        yield
    finally:
        for obj, matrix_parent_inverse, matrix_basis in object_states:
            if obj and obj.name in bpy.data.objects:
                obj.matrix_parent_inverse = matrix_parent_inverse
                obj.matrix_basis = matrix_basis
        for obj, original_mesh in original_mesh_data.items():
            if obj and obj.name in bpy.data.objects:
                obj.data = original_mesh
        for export_mesh in mesh_copies.values():
            if export_mesh and export_mesh.name in bpy.data.meshes:
                bpy.data.meshes.remove(export_mesh)
        unit_settings.system = saved_unit_state[0]
        unit_settings.scale_length = saved_unit_state[1]
        unit_settings.length_unit = saved_unit_state[2]


def _exit_local_view_isolation():
    """Leave every 3D viewport Local View (isolate mode) before staging.

    A Local View session makes everything outside the isolated set invisible
    to the artist, so a full-package export started in that state silently
    disagrees with what the scene authors.  Publication must always see the
    complete scene; any viewport still in Local View is switched back.
    Windowless sessions and viewports without Local View are no-ops.
    """
    window_manager = getattr(bpy.context, "window_manager", None)
    if window_manager is None:
        return
    for window in window_manager.windows:
        screen = window.screen
        if screen is None:
            continue
        for area in screen.areas:
            if area.type != "VIEW_3D":
                continue
            space = area.spaces.active
            if space is None or space.local_view is None:
                continue
            region = next(
                (region for region in area.regions if region.type == "WINDOW"),
                None)
            if region is None:
                continue
            with contextlib.suppress(RuntimeError):
                with bpy.context.temp_override(
                        window=window, area=area, region=region):
                    bpy.ops.view3d.localview(frame_selected=False)


@contextlib.contextmanager
def _temporary_fbx_batch_scene(scene):
    """Keep one shared export scene active across a closure mesh batch."""

    window = bpy.context.window
    if window is None:
        raise RuntimeError("FBX export requires an active Blender window")
    original_scene = window.scene
    if scene == original_scene:
        yield
        return

    original_view_layer = window.view_layer
    original_active = original_view_layer.objects.active
    original_mode = getattr(original_active, "mode", "OBJECT")
    if original_mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    window.scene = scene
    try:
        yield
    finally:
        window.scene = original_scene
        if (original_active is not None
                and original_active.name in original_view_layer.objects):
            original_view_layer.objects.active = original_active
        if (original_mode != "OBJECT" and original_active is not None
                and original_active.name in original_view_layer.objects):
            with contextlib.suppress(RuntimeError):
                bpy.ops.object.mode_set(mode=original_mode)


@contextlib.contextmanager
def _temporary_selection_context(scene, objects):
    """Select only ``objects`` for FBX and restore host interaction state."""
    window = bpy.context.window
    if window is None:
        raise RuntimeError("FBX export requires an active Blender window")

    original_scene = window.scene
    original_view_layer = window.view_layer
    original_active = original_view_layer.objects.active
    original_selected = [
        obj for obj in original_view_layer.objects if obj.select_get()
    ]
    original_mode = getattr(original_active, "mode", "OBJECT")
    if original_mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")

    switches_scene = scene != original_scene
    if switches_scene:
        window.scene = scene
    export_view_layer = window.view_layer
    same_view_layer = export_view_layer == original_view_layer
    export_active = export_view_layer.objects.active
    export_selected = (
        original_selected if same_view_layer else
        [obj for obj in export_view_layer.objects if obj.select_get()]
    )
    selectability = [(obj, obj.hide_select) for obj in objects]

    try:
        for obj in export_selected:
            obj.select_set(False)
        for obj in objects:
            if obj.name not in export_view_layer.objects:
                raise ValueError(
                    f"object '{obj.name}' is excluded from the active view layer")
            obj.hide_select = False
            obj.select_set(True)
        # Blender turns select_set(True) into a silent no-op for objects the
        # artist hid (H / isolate around another mesh).  The FBX writer runs
        # with use_selection, so an unverified selection becomes a published
        # mesh file without its geometry.  Field defect 2026-08-31: 731 empty
        # transports out of 732.
        unselected = [obj.name for obj in objects if not obj.select_get()]
        if unselected:
            raise MHValidationError(
                "MH_E_INVALID_RESOURCE_SOURCE", unselected,
                "export objects are hidden in the active view layer and "
                "cannot be selected for FBX transport; unhide them (Alt+H) "
                "or leave local view before exporting")
        export_view_layer.objects.active = objects[0]
        yield
    finally:
        for obj, hide_select in selectability:
            if obj and obj.name in bpy.data.objects:
                obj.hide_select = hide_select
        for obj in tuple(bpy.context.selected_objects):
            obj.select_set(False)
        for obj in export_selected:
            if obj and obj.name in export_view_layer.objects:
                obj.select_set(True)
        if export_active and export_active.name in export_view_layer.objects:
            export_view_layer.objects.active = export_active
        else:
            export_view_layer.objects.active = None

        if switches_scene:
            window.scene = original_scene
        if original_mode != "OBJECT" and original_active \
                and original_active.name in original_view_layer.objects:
            with contextlib.suppress(RuntimeError):
                bpy.ops.object.mode_set(mode=original_mode)


def _resolved_source_root(value):
    if not isinstance(value, (str, os.PathLike)) or not str(value).strip():
        raise ValueError(
            "Configure Project Source Root in the MH addon preferences")
    authored = Path(bpy.path.abspath(os.fspath(value)))
    try:
        root = authored.resolve(strict=True)
    except OSError as exc:
        raise ValueError(
            f"Project Source Root cannot be physically resolved: {authored}"
        ) from exc
    if not root.is_dir():
        raise ValueError(f"Project Source Root does not exist: {root}")
    return str(root)


def _assert_output_under_root(output_dir, source_root):
    physical_root = Path(source_root).resolve(strict=True)
    physical_output = Path(output_dir).resolve(strict=False)
    try:
        inside = os.path.commonpath([
            os.path.normcase(str(physical_output)),
            os.path.normcase(str(physical_root)),
        ]) == os.path.normcase(str(physical_root))
    except ValueError:
        inside = False
    if not inside:
        raise ValueError(
            "FBX output folder must be inside Project Source Root")


def _transport_material_binding(obj, index, slot, resource_name):
    """Return the binding one transported slot publishes, or fail closed."""
    material = slot.material
    if material is None:
        raise MHValidationError(
            "MH_E_EMPTY_MATERIAL_SLOT", [obj.name],
            f"'{obj.name}' material slot {index} is empty")
    slot_name = str(slot.name or material.name)
    authored_slot = strip_blender_duplicate_suffix(slot_name)
    authored_material = strip_blender_duplicate_suffix(material.name)
    binding = resolve_material_binding(material, asset_name=resource_name)
    # A normal Blender slot carries the authored material name. At the Dagor
    # adapter boundary that name may project to a canonical transport token;
    # compare the projected binding while retaining fail-closed handling for
    # a genuinely mismatched/custom slot name.
    logical_slot = (
        binding.name if authored_slot == authored_material else authored_slot)
    logical_material = binding.name
    try:
        validate_resource_name(logical_slot)
        validate_resource_name(logical_material)
    except (TypeError, ValueError) as exc:
        raise MHValidationError(
            "MH_E_NONCANONICAL_RESOURCE_NAME",
            [slot_name, material.name], str(exc)) from exc
    if logical_slot != logical_material:
        raise MHValidationError(
            "MH_E_MATERIAL_SLOT_CONFLICT", [slot_name, material.name],
            "FBX material slot name must equal material logical name")
    if is_technical_material(material):
        # A mesh painted entirely with `cls` is recognized collision and never
        # reaches this function. A mesh that mixes technical and render paint
        # is not a construct owner semantics cover, so it fails closed instead
        # of silently publishing or silently dropping a slot.
        raise MHValidationError(
            "MH_E_MATERIAL_SLOT_CONFLICT", [obj.name, material.name],
            f"'{obj.name}' mixes the technical material "
            f"'{material.name}' with render materials; technical paint "
            "belongs to collision-only meshes")
    return binding


def _material_slot_names(objects, resource_name):
    """Validate and return the logical material names transported by FBX."""
    names = set()
    for obj in sorted(objects, key=lambda item: item.name):
        for index, slot in enumerate(obj.material_slots):
            names.add(_transport_material_binding(
                obj, index, slot, resource_name).name)
    return names


def _validate_export_node_markers(
        export_objects, payload_levels, collision_transport=()):
    """Mirror the v4 FBX classifier before Blender writes any bytes."""
    authored_levels = {
        obj.as_pointer(): level
        for level, _collection, objects in payload_levels
        for obj in objects
    }
    carriers = {row.obj.as_pointer(): row for row in collision_transport}
    for obj in export_objects:
        # OPEN-V4-11 forbids any child below a SOCKET_, including a child
        # outside the selected resource.  Parent closure remains a separate
        # transport gate below.
        has_children = bool(obj.children)
        authored_lod = (
            authored_levels.get(obj.as_pointer())
            if obj.type == "MESH" else None)
        carrier = carriers.get(obj.as_pointer())
        validate_node_markers(
            obj.name,
            obj.type,
            has_children=has_children,
            authored_lod=authored_lod,
            collision_kind=None if carrier is None else carrier.kind,
            collision_shape=None if carrier is None else carrier.shape,
            phmat=None if carrier is None else carrier.phmat,
        )


def _validate_unique_mesh_payloads(export_objects):
    """The v4 FBX dialect transports no shared/instanced Geometry."""
    owners = {}
    for obj in export_objects:
        if obj.type != "MESH" or obj.data is None:
            continue
        identity = obj.data.as_pointer()
        previous = owners.get(identity)
        if previous is not None:
            raise MHValidationError(
                "MH_E_UNSUPPORTED_NODE_KIND", [previous.name, obj.name],
                "linked duplicate mesh Geometry is outside the v4 FBX "
                "dialect; use separate mesh datablocks or Composite instances")
        owners[identity] = obj


def _export_selected_fbx(filepath):
    """Operator seam kept separate for crash-protocol integration tests."""
    bpy.ops.export_scene.fbx(filepath=filepath, **FBX_EXPORT_KWARGS)


_LOD_NODE_SUFFIX_RE = re.compile(r"_lod(?P<level>\d{2})$")
_BLENDER_ID_NAME_MAX_BYTES = 63
_LOD_TRANSPORT_NAME_HASH_CHARS = 12


def _utf8_prefix(value: str, max_bytes: int) -> str:
    encoded = value.encode("utf-8")[:max_bytes]
    while encoded:
        try:
            return encoded.decode("utf-8")
        except UnicodeDecodeError:
            encoded = encoded[:-1]
    return ""


def _bounded_lod_node_name(name: str, suffix: str) -> str:
    """Fit a temporary node name while retaining the classifying suffix."""
    desired = f"{name}{suffix}"
    encoded = desired.encode("utf-8")
    if len(encoded) <= _BLENDER_ID_NAME_MAX_BYTES:
        return desired
    digest = hashlib.sha256(encoded).hexdigest()[:_LOD_TRANSPORT_NAME_HASH_CHARS]
    prefix_budget = (
        _BLENDER_ID_NAME_MAX_BYTES
        - len(suffix.encode("ascii"))
        - 1
        - _LOD_TRANSPORT_NAME_HASH_CHARS
    )
    prefix = _utf8_prefix(name, prefix_budget)
    if not prefix:
        raise MHValidationError(
            "MH_E_INVALID_LOD_HIERARCHY", [name],
            "LOD node name cannot retain a non-empty transport prefix")
    return f"{prefix}_{digest}{suffix}"


@contextlib.contextmanager
def _temporary_lod_node_names(levels):
    """Expose authored LOD membership in node names and always restore it."""
    changes = []
    desired_owners = {}
    for level, _collection, objects in levels:
        suffix = f"_lod{level:02d}"
        for obj in objects:
            match = _LOD_NODE_SUFFIX_RE.search(obj.name)
            if match is not None:
                if int(match.group("level")) != level:
                    raise MHValidationError(
                        "MH_E_INVALID_LOD_HIERARCHY", [obj.name],
                        f"mesh object '{obj.name}' is in LOD {level} but "
                        f"carries {match.group(0)}")
                continue
            desired = _bounded_lod_node_name(obj.name, suffix)
            previous = desired_owners.get(desired)
            if previous is not None and previous != obj:
                raise MHValidationError(
                    "MH_E_INVALID_LOD_HIERARCHY", [desired],
                    f"LOD export node name '{desired}' is not unique")
            desired_owners[desired] = obj
            existing = bpy.data.objects.get(desired)
            if existing is not None and existing != obj:
                raise MHValidationError(
                    "MH_E_INVALID_LOD_HIERARCHY", [desired, existing.name],
                    f"temporary LOD node name '{desired}' is already used by "
                    f"'{existing.name}'")
            changes.append((obj, obj.name, desired))
    try:
        for obj, _original, desired in changes:
            obj.name = desired
            if obj.name != desired:
                raise MHValidationError(
                    "MH_E_INVALID_LOD_HIERARCHY", [desired],
                    f"Blender could not assign temporary LOD node name "
                    f"'{desired}'")
        yield
    finally:
        for obj, original, _desired in reversed(changes):
            if obj is not None:
                obj.name = original


def _id_property_snapshot(value):
    """Return a plain-Python copy of one Blender ID property value."""
    if hasattr(value, "to_dict"):
        return value.to_dict()
    if hasattr(value, "to_list"):
        return value.to_list()
    return value


def _authored_property_keys(data_block):
    """Return the ID properties Blender's FBX writer would serialize.

    ``use_custom_props`` publishes every ID property except the ones backed by
    a runtime-registered RNA declaration, so this mirrors the writer's own
    filter exactly (``export_fbx_bin.fbx_data_element_custom_properties``).
    A value with no FBX type maps to ``str(value)``, which for a property
    group embeds a memory address, so leaving authored properties in place
    would publish both authoring noise and non-reproducible bytes.
    """
    runtime = {
        prop.identifier for prop in data_block.bl_rna.properties
        if prop.is_runtime
    }
    return [key for key in data_block.keys() if key not in runtime]


@contextlib.contextmanager
def _temporary_collision_transport(export_objects, collision_transport):
    """Own the FBX transport state of the reserved `mh_` namespace.

    Three things happen for exactly the duration of one FBX write and are all
    reverted in ``finally``:

    * every transported object loses its authored ID properties, so authored
      data can neither leak into a published resource nor forge a collision
      claim in the reserved ``mh_`` namespace that the reader would believe;
    * each transported collision node gains the frozen carrier
      (docs/15 §3.4) ``mh_collision`` / ``mh_collision_shape`` / ``mh_phmat``;
    * each transported collision node loses its technical ``cls`` paint, which
      owner decision 2026-08-30 (docs/15 §1.3) keeps out of the FBX entirely.

    The authoring scene keeps every property, material and slot it had.
    """
    saved_properties = []
    written_properties = []
    saved_materials = []
    try:
        for obj in export_objects:
            for key in _authored_property_keys(obj):
                saved_properties.append(
                    (obj, key, _id_property_snapshot(obj[key])))
                del obj[key]
        for row in collision_transport:
            obj = row.obj
            mesh = obj.data
            if mesh is not None and mesh.materials:
                saved_materials.append((
                    obj, mesh, tuple(mesh.materials),
                    tuple((slot.link, slot.material)
                          for slot in obj.material_slots)))
                mesh.materials.clear()
            carrier = {
                FBX_COLLISION_KIND_KEY: row.kind,
                FBX_COLLISION_SHAPE_KEY: row.shape,
            }
            if row.phmat is not None:
                carrier[FBX_PHMAT_KEY] = row.phmat
            for key, value in carrier.items():
                obj[key] = value
                written_properties.append((obj, key))
        yield
    finally:
        for obj, key in reversed(written_properties):
            if key in obj.keys():
                del obj[key]
        for obj, mesh, materials, slots in reversed(saved_materials):
            mesh.materials.clear()
            for material in materials:
                mesh.materials.append(material)
            for slot, (link, material) in zip(obj.material_slots, slots):
                slot.link = link
                slot.material = material
        for obj, key, value in reversed(saved_properties):
            obj[key] = value


@contextlib.contextmanager
def _temporary_transport_material_names(export_objects, resource_name):
    """Expose logical material names to the FBX writer and always restore them.

    Blender stamps the datablock name into the FBX, so a ``.NNN`` duplicate
    would otherwise transport a noncanonical slot name.  Slots bound to a
    merged duplicate are pointed at their representative first, then the
    representative is renamed if it still carries a suffix of its own
    (docs/15 §2.3).  Every change is reverted in ``finally``; the artist's
    scene keeps both datablocks and both authored names.
    """
    slot_changes = []
    renames = []
    representatives = []
    seen = set()
    try:
        for obj in export_objects:
            if obj.type != "MESH":
                continue
            for slot in obj.material_slots:
                material = slot.material
                if material is None:
                    continue
                binding = resolve_material_binding(
                    material, asset_name=resource_name)
                if binding.name not in seen:
                    seen.add(binding.name)
                    representatives.append(binding)
                if binding.material != material:
                    slot_changes.append((slot, material))
                    slot.material = binding.material
        for binding in representatives:
            if binding.material.name == binding.name:
                continue
            renames.append((binding.material, binding.material.name))
            binding.material.name = binding.name
            if binding.material.name != binding.name:
                raise MHValidationError(
                    "MH_E_AMBIGUOUS_RESOURCE_NAME",
                    [binding.name, binding.material.name],
                    "Blender could not assign the transport material name "
                    f"'{binding.name}'")
        yield
    finally:
        for material, original in reversed(renames):
            material.name = original
        for slot, original in reversed(slot_changes):
            slot.material = original


def _clean_fbx_filename(resource_name):
    validate_resource_name(resource_name)
    return f"{resource_name}.mesh.fbx"


def _assert_existing_target(filepath):
    """Allow file replacement, but never replace a directory target."""
    if os.path.lexists(filepath) and os.path.isdir(filepath):
        raise ValueError(f"FBX target exists as a directory: {filepath}")


def _matrix_fingerprint(matrix):
    return tuple(
        float(matrix[row][column]).hex()
        for row in range(4)
        for column in range(4)
    )


def _mesh_authority_fingerprint(
        collection, scene, resource_name, export_objects, payload_levels,
        lod_levels, uses_lod_hierarchy, materials, warnings,
        collision_transport=()):
    object_rows = []
    for obj in export_objects:
        slots = tuple(
            (
                str(slot.name),
                None if slot.material is None else slot.material.as_pointer(),
                None if slot.material is None else slot.material.name,
            )
            for slot in obj.material_slots
        )
        object_rows.append((
            obj.as_pointer(),
            obj.name,
            obj.type,
            None if obj.data is None else obj.data.as_pointer(),
            None if obj.parent is None else obj.parent.as_pointer(),
            _matrix_fingerprint(obj.matrix_parent_inverse),
            _matrix_fingerprint(obj.matrix_basis),
            slots,
        ))
    return (
        collection.as_pointer(),
        collection.name,
        collection.get(COLLECTION_KIND_KEY),
        collection.get(COLLECTION_RESOURCE_KEY),
        bool(collection.get(INCOMPLETE_IMPORT_KEY, False)),
        scene.as_pointer(),
        resource_name,
        tuple(object_rows),
        tuple(
            (level, level_collection.as_pointer(),
             tuple(obj.as_pointer() for obj in objects))
            for level, level_collection, objects in payload_levels
        ),
        tuple(lod_levels),
        bool(uses_lod_hierarchy),
        tuple(
            (binding.material.as_pointer(), binding.material.name, binding.name)
            for binding in materials
        ),
        tuple(warnings),
        tuple(
            (row.obj.as_pointer(), row.obj.name, row.kind, row.shape, row.phmat)
            for row in collision_transport
        ),
    )


def prepare_fbx_collection(
        collection, output_dir, *, source_root="", export_materials=False):
    """Extract and fully validate one Blender mesh without writing source.

    The returned plan is suitable both for the legacy one-resource publisher
    and for a closure-wide preflight/staging transaction.  In particular the
    incomplete-import guard and every FBX dialect check happen here, before a
    staging directory or source payload can be created.
    """
    if _active_material_export_session() is None:
        with material_export_session():
            return prepare_fbx_collection(
                collection, output_dir, source_root=source_root,
                export_materials=export_materials)
    if collection is None:
        raise ValueError("collection is required")
    linked = []
    for member_collection in (
            collection, *tuple(collection.children_recursive)):
        if member_collection.library is not None:
            linked.append(f"collection:{member_collection.name}")
    for obj in _collection_objects(collection):
        if obj.library is not None:
            linked.append(f"object:{obj.name}")
        data = getattr(obj, "data", None)
        if data is not None and data.library is not None:
            linked.append(f"data:{data.name}")
    if linked:
        raise MHValidationError(
            "MH_E_INVALID_RESOURCE_SOURCE", linked,
            "linked read-only Blender Collections, Objects, or object data "
            "cannot be mesh export authority")
    if bool(collection.get(INCOMPLETE_IMPORT_KEY, False)):
        raise MHValidationError(
            "MH_E_INVALID_RESOURCE_SOURCE", [collection.name],
            "mesh definitions imported in LOD0 or structure-only mode are "
            "incomplete and cannot be exported")
    if not isinstance(output_dir, (str, os.PathLike)) or not str(output_dir).strip():
        raise ValueError("output_dir is required")

    lod_structure = _dagor_lod_structure(collection)
    resource_name = (
        lod_structure["resource_name"]
        if lod_structure is not None else collection.name)
    objects = (
        [obj for _level, _child, level_objects in lod_structure["levels"]
         for obj in level_objects]
        if lod_structure is not None else
        collect_collection_mesh_objects(collection))
    if lod_structure is not None:
        aux_objects = lod_structure["level0_aux"]
        group_objects = lod_structure["groups"]
        collision_candidates = lod_structure["excluded_collision"]
    else:
        _geometry, aux_objects, group_objects, collision_candidates = (
            _collection_resource_objects(collection))
    # Collision is gathered from every authored LOD (real Dagor content keeps
    # it in `.lod01`) and is never bound to a LOD level in transport.
    collision_transport = []
    excluded_collision = []
    collision_notes = []
    for candidate in collision_candidates:
        transport, notes = _classify_dagor_collision(candidate)
        collision_notes.extend(notes)
        if transport is None:
            excluded_collision.append(candidate)
        else:
            collision_transport.append(transport)
    transported_collision = [row.obj for row in collision_transport]
    export_objects = (
        objects + aux_objects + group_objects + transported_collision)
    payload_levels = tuple(
        lod_structure["levels"] if lod_structure is not None
        else [(0, collection, objects)])
    payload_levels = tuple(
        (level, level_collection, tuple(level_objects))
        for level, level_collection, level_objects in payload_levels)
    # Excluded collision still passes the marker gate: dropping a node from
    # the payload must not turn a conflicting authored marker into silence.
    _validate_export_node_markers(
        export_objects + excluded_collision, payload_levels,
        collision_transport)
    _validate_unique_mesh_payloads(export_objects)
    if not objects:
        raise ValueError(
            f"MH_E_EMPTY_RESOURCE_COLLECTION: '{collection.name}' has no "
            "mesh objects in itself or recursive child collections")

    filename = _clean_fbx_filename(resource_name)
    resolved_output_dir = os.path.abspath(
        bpy.path.abspath(os.fspath(output_dir)))
    filepath = os.path.join(resolved_output_dir, filename)
    resolved_source_root = None
    if isinstance(source_root, (str, os.PathLike)) and str(source_root).strip():
        resolved_source_root = _resolved_source_root(source_root)
        _assert_output_under_root(resolved_output_dir, resolved_source_root)
        session = _active_material_export_session()
        if (export_materials and session is not None
                and session.inventory is None):
            session.bind_inventory(scan_source_inventory(resolved_source_root))
    _assert_existing_target(filepath)

    # AMENDMENT_node_hierarchy: Blender silently re-roots children of
    # non-exported parents with baked world transforms. Every transported
    # object's parent must therefore be transported too (or None).
    transported_ids = {obj.as_pointer() for obj in export_objects}
    for obj in export_objects:
        parent = obj.parent
        if parent is not None and parent.as_pointer() not in transported_ids:
            raise MHValidationError(
                "MH_E_PARENT_OUTSIDE_RESOURCE", [resource_name],
                f"'{obj.name}' is parented to '{parent.name}', which is not "
                f"part of resource collection '{collection.name}'; move the "
                "parent into the collection or clear the parenting")

    # UCX_ collision geometry is not render geometry, but Blender still
    # serializes its slots; validate those too so the writer cannot create a
    # file that our own reader rejects.  Dagor collision transports without any
    # material at all, so its technical paint is validated by neither pass.
    collision_ids = {obj.as_pointer() for obj in transported_collision}
    transport_meshes = [
        obj for obj in export_objects
        if obj.type == "MESH" and obj.as_pointer() not in collision_ids]
    _material_slot_names(transport_meshes, resource_name)
    # The material list is the render-only ordered union of every LOD
    # (docs/15 §1.1 and §3.4 last bullet): `objects` is already LOD-major, so
    # first appearance in this walk is the frozen contract order, and a slot
    # owned only by non-render nodes never enters the closure.
    materials = []
    seen_materials = {}
    for obj in objects:
        for index, slot in enumerate(obj.material_slots):
            binding = _transport_material_binding(
                obj, index, slot, resource_name)
            previous = seen_materials.get(binding.name)
            if previous is not None:
                if previous is not binding:
                    raise MHValidationError(
                        "MH_E_AMBIGUOUS_RESOURCE_NAME",
                        [binding.name, previous.material.name,
                         binding.material.name],
                        "different Blender materials project to one material "
                        "token")
                continue
            seen_materials[binding.name] = binding
            materials.append(binding)

    scene = _find_export_scene(collection)
    warnings = []
    # Owner decision 2026-08-30: diverging .NNN duplicates merge into the base
    # logical name; the artist sees exactly what the representative overrode.
    for binding in materials:
        if binding.divergence is not None and binding.divergence not in warnings:
            warnings.append(binding.divergence)
    if lod_structure is not None and lod_structure["ignored_aux"]:
        ignored = ", ".join(
            (f"LOD{level} '{name}'" if level != "root"
             else f"container '{name}'")
            for level, name in lod_structure["ignored_aux"])
        warnings.append((
            "MH_W_LOD_AUX_NODE_IGNORED",
            (resource_name,),
            f"out-of-LOD0 auxiliary nodes were ignored: {ignored}",
        ))
    if excluded_collision:
        dropped_nodes = sorted(obj.name for obj in excluded_collision)
        dropped_materials = sorted({
            slot.material.name
            for obj in excluded_collision
            for slot in obj.material_slots
            if slot.material is not None
        })
        message = (
            "Dagor collision nodes declaring neither 'phys' nor 'trace' have "
            "no ratified UE collision meaning and are not transported "
            f"(docs/15 §3.2): {', '.join(dropped_nodes)}")
        if dropped_materials:
            message += (
                "; technical materials dropped: "
                + ", ".join(dropped_materials))
        warnings.append((
            "MH_W_DAGOR_CONSTRUCT_DROPPED",
            (resource_name, *dropped_nodes),
            message,
        ))
    if collision_notes:
        warnings.append((
            "MH_W_DAGOR_CONSTRUCT_DROPPED",
            (resource_name,),
            "Dagor collision declarations were not used as authored: "
            + "; ".join(sorted(collision_notes)),
        ))
    phmatless = sorted(
        row.obj.name for row in collision_transport if row.phmat is None)
    if phmatless:
        warnings.append((
            "MH_W_MATERIAL_PAYLOAD_FALLBACK",
            (resource_name, *phmatless),
            "Dagor collision nodes carry no 'phmat:t' declaration and are "
            "transported without a UE physical material (scenes imported "
            f"before the dag4blend overlay patch): {', '.join(phmatless)}",
        ))

    prepared_materials = []
    if export_materials:
        if resolved_source_root is None:
            raise ValueError(
                "Project Source Root is required when Export Materials is enabled")
        from .export_material import prepare_blender_material_export
        prepared_materials = tuple(
            prepare_blender_material_export(
                material, resolved_output_dir,
                source_root=resolved_source_root)
            for material in sorted(materials, key=lambda item: item.name)
        )

    lod_levels = tuple(level for level, _child, _objects in payload_levels)
    fingerprint = _mesh_authority_fingerprint(
        collection, scene, resource_name, tuple(export_objects),
        payload_levels, lod_levels, lod_structure is not None,
        tuple(materials), tuple(warnings), tuple(collision_transport))
    return PreparedFBXExport(
        collection=collection,
        scene=scene,
        target=Path(filepath),
        source_root=(
            Path(resolved_source_root)
            if resolved_source_root is not None else None),
        resource_name=resource_name,
        export_objects=tuple(export_objects),
        payload_levels=payload_levels,
        lod_levels=lod_levels,
        uses_lod_hierarchy=lod_structure is not None,
        materials=tuple(materials),
        prepared_materials=tuple(prepared_materials),
        warnings=tuple(warnings),
        authority_fingerprint=fingerprint,
        collision_transport=tuple(collision_transport),
    )


def stage_prepared_fbx(prepared, staged_filepath):
    """Write and read back one prepared FBX without touching its source target.

    ``staged_filepath`` is caller-owned but must use the final canonical
    filename.  Keeping the filename stable preserves the resource identity
    that will be published, while returned bytes are the exact file read-back.
    Existing paths are never overwritten, and a failed stage is removed.
    """
    session = _active_material_export_session()
    if session is None:
        with material_export_session():
            return stage_prepared_fbx(prepared, staged_filepath)
    session.validate_sources()
    _exit_local_view_isolation()
    if not isinstance(prepared, PreparedFBXExport):
        raise TypeError("prepared must be PreparedFBXExport")
    if not isinstance(staged_filepath, (str, os.PathLike)) \
            or not str(staged_filepath).strip():
        raise ValueError("staged_filepath is required")

    # Blender state is live.  Re-run every write-free admission check at the
    # staging edge and require the dependency/structure fingerprint frozen by
    # the closure plan.  Blender operators are single-threaded after this edge.
    refreshed = prepare_fbx_collection(
        prepared.collection,
        prepared.target.parent,
        source_root=(prepared.source_root or ""),
        export_materials=False,
    )
    if refreshed.authority_fingerprint != prepared.authority_fingerprint:
        raise MHValidationError(
            "MH_E_INVALID_RESOURCE_SOURCE", [prepared.resource_name],
            "mesh authoring structure or dependencies changed after preflight")
    prepared = replace(
        refreshed,
        target=prepared.target,
        prepared_materials=prepared.prepared_materials,
    )

    staged = Path(bpy.path.abspath(os.fspath(staged_filepath))).resolve(
        strict=False)
    expected_filename = _clean_fbx_filename(prepared.resource_name)
    if staged.name != expected_filename:
        raise ValueError(
            "staged FBX must preserve canonical filename "
            f"'{expected_filename}'")
    if staged == prepared.target.resolve(strict=False):
        raise ValueError("staged FBX path must differ from source target")
    if prepared.source_root is not None:
        physical_root = prepared.source_root.resolve(strict=True)
        try:
            inside_source = os.path.commonpath([
                os.path.normcase(str(physical_root)),
                os.path.normcase(str(staged)),
            ]) == os.path.normcase(str(physical_root))
        except ValueError:
            inside_source = False
        if inside_source:
            raise ValueError(
                "staged FBX must be outside Project Source Root authority")
    if os.path.lexists(staged):
        raise ValueError(f"staged FBX path already exists: {staged}")

    if not staged.parent.is_dir():
        raise ValueError(
            f"staged FBX parent directory does not exist: {staged.parent}")
    succeeded = False
    try:
        levels = prepared.payload_levels if prepared.uses_lod_hierarchy else ()
        with _temporary_lod_node_names(levels):
            # Collision paint is cleared before transport material names are
            # resolved, so the technical `cls` datablock is never a transport
            # binding.  Both are applied before the disposable mesh copies are
            # taken, so a DATA-linked slot change reaches the copy.
            with _temporary_collision_transport(
                    prepared.export_objects, prepared.collision_transport):
                with _temporary_transport_material_names(
                        prepared.export_objects, prepared.resource_name):
                    with _temporary_selection_context(
                            prepared.scene, prepared.export_objects):
                        with _temporary_ue_centimeter_export_state(
                                prepared.export_objects):
                            _export_selected_fbx(str(staged))
        if not staged.is_file():
            raise RuntimeError("FBX exporter did not create its staged file")
        payload = staged.read_bytes()
        if not payload:
            raise RuntimeError("staged FBX read-back is empty")
        # Read the staged file through Blender's binary FBX tree reader.  This
        # rejects truncated/arbitrary bytes without narrowing the established
        # writer contract to the smaller MH import dialect (shape keys are a
        # valid writer construct but intentionally not import-classified).
        from io_scene_fbx import parse_fbx
        try:
            root, version = parse_fbx.parse(str(staged))
        except Exception as exc:
            raise RuntimeError(
                "staged FBX failed structural read-back validation"
            ) from exc
        section_ids = {item.id for item in root.elems}
        if version < 7100 or not {b"Objects", b"Connections"}.issubset(
                section_ids):
            raise RuntimeError(
                "staged FBX failed structural read-back validation")
        # Every transported object must survive as one FBX Model node.  A
        # structurally valid file with fewer Models is a silently truncated
        # export (empty selection, hidden geometry) and must never publish.
        model_count = sum(
            1
            for item in root.elems
            if item.id == b"Objects"
            for child in item.elems
            if child.id == b"Model")
        expected_models = len(prepared.export_objects)
        if model_count != expected_models:
            raise RuntimeError(
                f"staged FBX transports {model_count} Model nodes, expected "
                f"{expected_models} for '{prepared.resource_name}'")
        succeeded = True
        return StagedFBXExport(filepath=staged, payload=payload)
    finally:
        if not succeeded:
            with contextlib.suppress(OSError):
                staged.unlink()


def _validation_report(prepared):
    return {
        "errors": [],
        "warnings": [
            {"code": code, "subjects": list(subjects), "message": message}
            for code, subjects, message in prepared.warnings
        ],
    }


def export_fbx_collection(
        collection, output_dir, *, dry_run=False, source_root="",
        export_materials=False):
    """Export one selected collection as one static-mesh resource.

    A regular collection writes one FBX. A recognized dag4blend ``.lods``
    hierarchy writes one FBX containing every authored level. Render mesh node
    names temporarily carry their ``_lodNN`` suffix; the only MH properties
    written to the FBX are the collision carrier of docs/15 §3.4.
    """
    if _active_material_export_session() is None:
        with material_export_session() as session:
            report = export_fbx_collection(
                collection, output_dir, dry_run=dry_run,
                source_root=source_root, export_materials=export_materials)
        report["material_export_metrics"] = session.metrics_snapshot()
        return report
    prepared = prepare_fbx_collection(
        collection,
        output_dir,
        source_root=source_root,
        export_materials=export_materials,
    )

    filepath = str(prepared.target)
    payload_updates = [{"filepath": filepath, "written": False}]
    written = False
    material_updates = []
    if not dry_run:
        prepared.target.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(
                prefix=f"{prepared.resource_name}-mh-fbx-stage-") as stage_dir:
            staged_filepath = Path(stage_dir) / prepared.target.name
            staged = stage_prepared_fbx(prepared, staged_filepath)

            def guard():
                _assert_existing_target(filepath)

            def validate_publication_read_back(read_back):
                if read_back != staged.payload:
                    raise RuntimeError(
                        "staged FBX failed exact publication read-back")

            atomic_publish_bytes(
                prepared.target,
                staged.payload,
                source_root=prepared.source_root,
                read_back_validator=validate_publication_read_back,
                pre_replace_guard=guard,
            )
            payload_updates[0]["written"] = True
            written = True
        if prepared.prepared_materials:
            from .export_material import write_prepared_material
            material_updates = [
                write_prepared_material(
                    material, source_root=prepared.source_root)
                for material in prepared.prepared_materials
            ]
        stamp_resource_collection(
            prepared.collection, "mesh", prepared.resource_name)

    return {
        "ok": True,
        "filepath": filepath,
        "written": written,
        "objects_exported": len(prepared.export_objects),
        "payload_updates": payload_updates,
        "resource_name": prepared.resource_name,
        "lod_levels": list(prepared.lod_levels),
        "materials": [binding.name for binding in prepared.materials],
        "material_updates": material_updates,
        "warnings": list(prepared.warnings),
        "validation": _validation_report(prepared),
    }
