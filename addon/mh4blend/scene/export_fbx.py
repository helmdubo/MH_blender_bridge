"""Standalone FBX export for one explicitly selected Blender collection.

The collection is treated like dag4blend's ``Col.Joined`` mode: direct mesh
objects and mesh objects in recursive child collections form one static-mesh
resource.  Sibling collections and unrelated scene objects are never selected.
No scene names, bundle directories or texture roots participate in this API.
"""

import contextlib
import os
import re
import tempfile

import bpy
from mathutils import Matrix

from ..core.canonical import validate_resource_name
from ..core.payload_publish_v2 import payload_lock
from ..core.validate import MHValidationError

__all__ = [
    "FBX_EXPORT_KWARGS",
    "collect_collection_mesh_objects",
    "export_fbx_collection",
]


# Canonical UE transport settings.  This module is the standalone owner of the
# settings; the legacy aggregate exporter can import them while it is retired.
FBX_EXPORT_KWARGS = dict(
    use_selection=True,
    use_custom_props=False,
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
    return (
        obj.type == "MESH" and (
            obj.name.startswith("UCX_")
            or obj.name.endswith(("_cls_phys", "_cls_trace", "_cls_both"))
        )
    ) or (
        obj.type == "EMPTY" and obj.name.startswith("SOCKET_")
    )


def _collection_resource_objects(collection):
    """Split recursive membership into (geometry, aux, groups).

    ``groups`` are organizational EMPTY objects (everything that is not a
    ``SOCKET_*`` aux). AMENDMENT_node_hierarchy: they are part of the FBX
    transport as plain null nodes so the authored hierarchy survives instead
    of Blender silently re-rooting children with baked world transforms.
    """
    members = _collection_objects(collection)
    aux = [obj for obj in members if _is_static_mesh_aux(obj)]
    aux_ids = {obj.as_pointer() for obj in aux}
    geometry = [
        obj for obj in members
        if obj.type == "MESH" and obj.as_pointer() not in aux_ids
    ]
    groups = [
        obj for obj in members
        if obj.type == "EMPTY" and obj.as_pointer() not in aux_ids
    ]
    return geometry, aux, groups


def _collection_mesh_objects(collection):
    """Return recursive semantic geometry, excluding FBX aux nodes."""
    geometry, _aux, _groups = _collection_resource_objects(collection)
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
    level_aux_ids = set()
    object_level = {}
    for level, child in sorted(levels.items()):
        objects, aux, _child_groups = _collection_resource_objects(child)
        level_aux_ids.update(obj.as_pointer() for obj in aux)
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
    _root_geometry, root_aux, groups = _collection_resource_objects(collection)
    for aux_obj in root_aux:
        if aux_obj.as_pointer() not in level_aux_ids:
            ignored_aux.append(("root", aux_obj.name))

    return {
        "resource_name": resource_name,
        "levels": level_objects,
        "level0_aux": level0_aux,
        "ignored_aux": ignored_aux,
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
        for obj in export_view_layer.objects:
            obj.select_set(False)
        for obj in objects:
            if obj.name not in export_view_layer.objects:
                raise ValueError(
                    f"object '{obj.name}' is excluded from the active view layer")
            obj.hide_select = False
            obj.select_set(True)
        export_view_layer.objects.active = objects[0]
        yield
    finally:
        for obj, hide_select in selectability:
            if obj and obj.name in bpy.data.objects:
                obj.hide_select = hide_select
        for obj in export_view_layer.objects:
            obj.select_set(False)
        for obj in export_selected:
            if obj and obj.name in export_view_layer.objects:
                obj.select_set(True)
        if export_active and export_active.name in export_view_layer.objects:
            export_view_layer.objects.active = export_active
        else:
            export_view_layer.objects.active = None

        window.scene = original_scene
        if not same_view_layer:
            for obj in original_view_layer.objects:
                obj.select_set(False)
            for obj in original_selected:
                if obj and obj.name in original_view_layer.objects:
                    obj.select_set(True)
            if original_active and original_active.name in original_view_layer.objects:
                original_view_layer.objects.active = original_active
        if original_mode != "OBJECT" and original_active \
                and original_active.name in original_view_layer.objects:
            with contextlib.suppress(RuntimeError):
                bpy.ops.object.mode_set(mode=original_mode)


def _resolved_source_root(value):
    if not isinstance(value, (str, os.PathLike)) or not str(value).strip():
        raise ValueError(
            "Configure Project Source Root in the MH addon preferences")
    root = os.path.abspath(bpy.path.abspath(os.fspath(value)))
    if not os.path.isdir(root):
        raise ValueError(f"Project Source Root does not exist: {root}")
    return root


def _assert_output_under_root(output_dir, source_root):
    try:
        inside = os.path.commonpath([
            os.path.normcase(os.path.abspath(output_dir)),
            os.path.normcase(os.path.abspath(source_root)),
        ]) == os.path.normcase(os.path.abspath(source_root))
    except ValueError:
        inside = False
    if not inside:
        raise ValueError(
            "FBX output folder must be inside Project Source Root")


def _material_slot_names(objects):
    """Validate and return the logical material names transported by FBX."""
    names = set()
    for obj in sorted(objects, key=lambda item: item.name):
        for index, slot in enumerate(obj.material_slots):
            material = slot.material
            if material is None:
                raise MHValidationError(
                    "MH_E_EMPTY_MATERIAL_SLOT", [obj.name],
                    f"'{obj.name}' material slot {index} is empty")
            slot_name = str(slot.name or material.name)
            try:
                validate_resource_name(slot_name)
                validate_resource_name(material.name)
            except (TypeError, ValueError) as exc:
                raise MHValidationError(
                    "MH_E_NONCANONICAL_RESOURCE_NAME",
                    [slot_name, material.name], str(exc)) from exc
            if slot_name != material.name:
                raise MHValidationError(
                    "MH_E_MATERIAL_SLOT_CONFLICT", [slot_name, material.name],
                    "FBX material slot name must equal material logical name")
            names.add(slot_name)
    return names


def _export_selected_fbx(filepath):
    """Operator seam kept separate for crash-protocol integration tests."""
    bpy.ops.export_scene.fbx(filepath=filepath, **FBX_EXPORT_KWARGS)


_LOD_NODE_SUFFIX_RE = re.compile(r"_lod(?P<level>\d{2})$")


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
            desired = f"{obj.name}{suffix}"
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


def _clean_fbx_filename(resource_name):
    validate_resource_name(resource_name)
    return f"{resource_name}.mesh.fbx"


def _assert_existing_target(filepath):
    """Allow file replacement, but never replace a directory target."""
    if os.path.lexists(filepath) and os.path.isdir(filepath):
        raise ValueError(f"FBX target exists as a directory: {filepath}")


def export_fbx_collection(
        collection, output_dir, *, dry_run=False, source_root="",
        export_materials=False):
    """Export one selected collection as one static-mesh resource.

    A regular collection writes one FBX. A recognized dag4blend ``.lods``
    hierarchy writes one FBX containing every authored level. Render mesh node
    names temporarily carry their ``_lodNN`` suffix; no MH properties are
    written to the FBX.
    """
    if collection is None:
        raise ValueError("collection is required")
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
    else:
        _geometry, aux_objects, group_objects = (
            _collection_resource_objects(collection))
    export_objects = objects + aux_objects + group_objects
    payload_levels = (
        lod_structure["levels"] if lod_structure is not None
        else [(0, collection, objects)])
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

    used_materials = []
    seen_materials = set()
    for obj in objects:
        for slot in obj.material_slots:
            material = slot.material
            if material is None or material.as_pointer() in seen_materials:
                continue
            seen_materials.add(material.as_pointer())
            used_materials.append(material)

    materials = used_materials
    if lod_structure is None:
        _material_slot_names(objects)
    else:
        _level0, _collection0, level0_objects = payload_levels[0]
        base_slots = _material_slot_names(level0_objects)
        for level, _child, level_objects in payload_levels[1:]:
            missing_slots = sorted(_material_slot_names(level_objects) - base_slots)
            if missing_slots:
                raise MHValidationError(
                    "MH_E_LOD_SLOT_NOT_IN_BASE",
                    missing_slots,
                    f"LOD{level} uses slots absent from LOD0: "
                    f"{', '.join(missing_slots)}")
    scene = _find_export_scene(collection)
    validation = {"errors": [], "warnings": []}
    if lod_structure is not None and lod_structure["ignored_aux"]:
        ignored = ", ".join(
            (f"LOD{level} '{name}'" if level != "root"
             else f"container '{name}'")
            for level, name in lod_structure["ignored_aux"])
        validation["warnings"].append({
            "code": "MH_W_LOD_AUX_NODE_IGNORED",
            "subjects": [resource_name],
            "message": f"out-of-LOD0 auxiliary nodes were ignored: {ignored}",
        })

    prepared_materials = []
    if export_materials:
        if resolved_source_root is None:
            raise ValueError(
                "Project Source Root is required when Export Materials is enabled")
        from .export_material import prepare_blender_material_export
        prepared_materials = [
            prepare_blender_material_export(
                material, resolved_output_dir,
                source_root=resolved_source_root)
            for material in sorted(materials, key=lambda item: item.name)
        ]

    payload_updates = [{"filepath": filepath, "written": False}]
    written = False
    material_updates = []
    if not dry_run:
        os.makedirs(resolved_output_dir, exist_ok=True)
        descriptor, tmp = tempfile.mkstemp(
            prefix=f".{filename}.mh-tmp-", dir=resolved_output_dir)
        os.close(descriptor)
        os.remove(tmp)
        with payload_lock(filepath, source_root=resolved_source_root):
            _assert_existing_target(filepath)
            try:
                levels = payload_levels if lod_structure is not None else []
                with _temporary_lod_node_names(levels):
                    with _temporary_selection_context(scene, export_objects):
                        with _temporary_ue_centimeter_export_state(export_objects):
                            _export_selected_fbx(tmp)
                if not os.path.isfile(tmp):
                    raise RuntimeError("FBX exporter did not create its staged file")
                _assert_existing_target(filepath)
                os.replace(tmp, filepath)
                payload_updates[0]["written"] = True
                written = True
            finally:
                with contextlib.suppress(OSError):
                    os.remove(tmp)
        if prepared_materials:
            from .export_material import write_prepared_material
            material_updates = [
                write_prepared_material(
                    prepared, source_root=resolved_source_root)
                for prepared in prepared_materials
            ]

    return {
        "ok": True,
        "filepath": filepath,
        "written": written,
        "objects_exported": len(export_objects),
        "payload_updates": payload_updates,
        "resource_name": resource_name,
        "lod_levels": [level for level, _child, _objects in payload_levels],
        "materials": materials,
        "material_updates": material_updates,
        "validation": validation,
    }
