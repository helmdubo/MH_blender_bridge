"""Standalone FBX export for one explicitly selected Blender collection.

The collection is treated like dag4blend's ``Col.Joined`` mode: direct mesh
objects and mesh objects in recursive child collections form one static-mesh
resource.  Sibling collections and unrelated scene objects are never selected.
No scene names, bundle directories or texture roots participate in this API.
"""

import contextlib
import os
import re
import uuid

import bpy
from mathutils import Matrix

from ..core.canonical import nfc, sanitize_name, validate_resource_name
from ..core.fbx_passport import (
    PASSPORT_PROPERTY,
    canonical_passport,
    make_fbx_passport,
    read_fbx_passport,
)
from ..core.meshser import MeshAuxRecord, mesh_content_hash
from ..core.model import MaterialSlot, MeshResource
from ..core.payload_publish_v2 import payload_lock
from ..core.uid import PROP_UID, ensure_uid, find_duplicate_uids
from ..core.validate import MHValidationError
from .composite_extract import _bag
from .mesh_extract import _record_for_object

__all__ = [
    "FBX_EXPORT_KWARGS",
    "collect_collection_mesh_objects",
    "export_fbx_collection",
]


# Canonical UE transport settings.  This module is the standalone owner of the
# settings; the legacy aggregate exporter can import them while it is retired.
FBX_EXPORT_KWARGS = dict(
    use_selection=True,
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
EXPORTER_VERSION = "0.6.0"


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
    upper_name = obj.name.upper()
    return (
        obj.type == "MESH" and upper_name.startswith("UCX_")
    ) or (
        obj.type == "EMPTY" and upper_name.startswith("SOCKET_")
    )


def _collection_resource_objects(collection):
    members = _collection_objects(collection)
    aux = [obj for obj in members if _is_static_mesh_aux(obj)]
    aux_ids = {obj.as_pointer() for obj in aux}
    geometry = [
        obj for obj in members
        if obj.type == "MESH" and obj.as_pointer() not in aux_ids
    ]
    return geometry, aux


def _collection_mesh_objects(collection):
    """Return recursive semantic geometry, excluding FBX aux nodes."""
    geometry, _aux = _collection_resource_objects(collection)
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
            raise ValueError(
                "MH_E_INVALID_LOD_HIERARCHY: selected collection "
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
    validate_resource_name(resource_name)

    direct_root_meshes = [
        obj for obj in collection.objects if obj.type == "MESH"]
    if direct_root_meshes:
        names = ", ".join(sorted(obj.name for obj in direct_root_meshes))
        raise ValueError(
            "MH_E_INVALID_LOD_HIERARCHY: .lods group contains mesh objects "
            f"outside a direct .lodNN collection: {names}")

    child_pattern = re.compile(
        _LOD_CHILD_RE_TEMPLATE.format(base=re.escape(base)))
    levels = {}
    for child in collection.children:
        match = child_pattern.fullmatch(child.name)
        if match is None:
            raise ValueError(
                "MH_E_INVALID_LOD_HIERARCHY: every direct child of "
                f"'{collection.name}' must be named '{base}.lodNN' "
                f"(optional Blender .NNN duplicate suffix); got "
                f"'{child.name}'")
        level = int(match.group("level"))
        if level in levels:
            raise ValueError(
                "MH_E_INVALID_LOD_HIERARCHY: duplicate authored LOD level "
                f"{level}: '{levels[level].name}' and '{child.name}'")
        levels[level] = child

    if 0 not in levels:
        raise ValueError(
            "MH_E_LOD_LEVELS_SPARSE: .lods group requires one direct "
            f"'{base}.lod00' collection")
    missing_levels = sorted(set(range(max(levels) + 1)) - set(levels))
    if missing_levels:
        missing = ", ".join(f"lod{level:02d}" for level in missing_levels)
        raise ValueError(
            "MH_E_LOD_LEVELS_SPARSE: authored LOD levels must be "
            f"contiguous from lod00; missing {missing}")

    level_objects = []
    level0_aux = []
    ignored_aux = []
    object_level = {}
    for level, child in sorted(levels.items()):
        objects, aux = _collection_resource_objects(child)
        if level == 0:
            level0_aux.extend(aux)
        elif aux:
            ignored_aux.extend((level, obj.name) for obj in aux)
        if not objects:
            raise ValueError(
                "MH_E_INVALID_LOD_HIERARCHY: authored LOD collection "
                f"'{child.name}' contains no mesh objects")
        for obj in objects:
            identity = obj.as_pointer()
            previous = object_level.get(identity)
            if previous is not None:
                raise ValueError(
                    "MH_E_INVALID_LOD_HIERARCHY: mesh object "
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
                raise ValueError(
                    "MH_E_INVALID_LOD_HIERARCHY: mesh object "
                    f"'{obj.name}' in LOD {level} is parented to "
                    f"'{parent.name}' in LOD {parent_level}")

    return {
        "resource_name": resource_name,
        "levels": level_objects,
        "level0_aux": level0_aux,
        "ignored_aux": ignored_aux,
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
    """Scale geometry/translations x100 for UE and restore them exactly."""
    scene = bpy.context.scene
    unit_settings = scene.unit_settings
    saved_unit_state = (
        unit_settings.system,
        unit_settings.scale_length,
        unit_settings.length_unit,
    )

    object_states = []
    mesh_data = []
    seen_mesh_data = set()
    for obj in objects:
        if obj is None or obj.name not in bpy.data.objects:
            continue
        obj = bpy.data.objects[obj.name]
        object_states.append(
            (obj, obj.matrix_parent_inverse.copy(), obj.matrix_basis.copy()))
        if (obj.type == "MESH" and obj.data is not None
                and obj.data not in seen_mesh_data):
            seen_mesh_data.add(obj.data)
            mesh_data.append(obj.data)

    scale = BLENDER_METERS_TO_UE_CENTIMETERS
    scale_matrix = Matrix.Scale(scale, 4)
    inverse_scale_matrix = Matrix.Scale(1.0 / scale, 4)
    scaled_mesh_data = []
    try:
        for mesh in mesh_data:
            _transform_mesh_data(mesh, scale_matrix)
            scaled_mesh_data.append(mesh)
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
        for mesh in scaled_mesh_data:
            if mesh and mesh.name in bpy.data.meshes:
                _transform_mesh_data(mesh, inverse_scale_matrix)
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


def _object_material_slot_pairs(objects):
    """Return every authored FBX slot mapping, without UID de-duplication."""
    pairs = set()
    for obj in objects:
        for slot in obj.material_slots:
            material = slot.material
            if material is None:
                continue
            pairs.add((
                str(slot.name or material.name),
                material[PROP_UID],
            ))
    return pairs


def _extract_material_slots(objects):
    """Extract only FBX descriptor bindings, never material payload data."""
    slots = []
    uid_by_slot_name = {}
    for obj in sorted(objects, key=lambda item: item[PROP_UID]):
        for index, slot in enumerate(obj.material_slots):
            material = slot.material
            if material is None:
                raise MHValidationError(
                    "MH_E_EMPTY_MATERIAL_SLOT", [obj[PROP_UID]],
                    f"'{obj.name}' material slot {index} is empty")
            material_uid = ensure_uid(material)
            slot_name = str(slot.name or material.name)
            previous = uid_by_slot_name.get(slot_name)
            if previous is not None and previous != material_uid:
                raise MHValidationError(
                    "MH_E_MATERIAL_SLOT_CONFLICT",
                    [previous, material_uid],
                    f"slot name '{slot_name}' maps to different materials")
            if previous is None:
                uid_by_slot_name[slot_name] = material_uid
                slots.append(MaterialSlot(
                    slot_name=slot_name, material_uid=material_uid))
    return slots


def _export_selected_fbx(filepath):
    """Operator seam kept separate for crash-protocol integration tests."""
    bpy.ops.export_scene.fbx(filepath=filepath, **FBX_EXPORT_KWARGS)


@contextlib.contextmanager
def _temporary_lod_level_properties(levels, implicit_level0=()):
    """Expose combined-LOD membership to FBX and restore authoring data."""
    saved = []
    try:
        for level, _collection, objects in levels:
            for obj in objects:
                existed = "mh_lod_level" in obj
                previous = obj["mh_lod_level"] if existed else None
                saved.append((obj, existed, previous))
                obj["mh_lod_level"] = int(level)
        for obj in implicit_level0:
            existed = "mh_lod_level" in obj
            previous = obj["mh_lod_level"] if existed else None
            saved.append((obj, existed, previous))
            if existed:
                del obj["mh_lod_level"]
        yield
    finally:
        for obj, existed, previous in saved:
            if obj is None or obj.name not in bpy.data.objects:
                continue
            if existed:
                obj["mh_lod_level"] = previous
            elif "mh_lod_level" in obj:
                del obj["mh_lod_level"]


@contextlib.contextmanager
def _temporary_passport_properties(objects, passport_text):
    """Put Carrier B on every exported MESH Model and restore ID properties."""
    saved = []
    try:
        for obj in objects:
            if obj.type != "MESH":
                continue
            existed = PASSPORT_PROPERTY in obj
            previous = obj[PASSPORT_PROPERTY] if existed else None
            saved.append((obj, existed, previous))
            obj[PASSPORT_PROPERTY] = passport_text
        yield
    finally:
        for obj, existed, previous in saved:
            if obj is None or obj.name not in bpy.data.objects:
                continue
            if existed:
                obj[PASSPORT_PROPERTY] = previous
            elif PASSPORT_PROPERTY in obj:
                del obj[PASSPORT_PROPERTY]


def _clean_fbx_filename(resource_name):
    validate_resource_name(resource_name)
    return f"{sanitize_name(resource_name)}.mesh.fbx"


def _passport_material_slots(materials, objects):
    names = {material[PROP_UID]: material.name for material in materials}
    rows = [{
        "slot_name": nfc(slot_name),
        "material_uid": material_uid,
        "material_name_hint": nfc(names[material_uid]),
    } for slot_name, material_uid in _object_material_slot_pairs(objects)]
    return sorted(rows, key=lambda row: row["slot_name"])


def _assert_existing_target_uid(filepath, resource_uid):
    """Validate an existing clean-name target and reject foreign identity."""
    if not os.path.lexists(filepath):
        return None
    if not os.path.isfile(filepath):
        raise ValueError(
            "MH_E_PASSPORT_INVALID: FBX target exists but is not a file: "
            f"{filepath}")
    current = read_fbx_passport(filepath)
    current_uid = current.document["resource_uid"]
    if current_uid != resource_uid:
        raise ValueError(
            "MH_E_NAME_COLLISION_DIFFERENT_UID: clean FBX target "
            f"'{filepath}' owns resource {current_uid}, not {resource_uid}")
    return current


def export_fbx_collection(
        collection, output_dir, *, dry_run=False, source_root=""):
    """Export one selected collection as one static-mesh resource.

    A regular collection writes one FBX. A recognized dag4blend ``.lods``
    hierarchy also writes one FBX containing every authored level; each model
    node carries a temporary integer ``mh_lod_level`` custom property.

    The FBX is the v2 source-of-truth payload. Its canonical passport is copied
    to every exported MESH Model. No export manifest is read or written here.
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
    else:
        _geometry, aux_objects = _collection_resource_objects(collection)
    export_objects = objects + aux_objects
    payload_levels = (
        lod_structure["levels"] if lod_structure is not None
        else [(0, collection, objects)])
    if not objects:
        raise ValueError(
            f"MH_E_EMPTY_RESOURCE_COLLECTION: '{collection.name}' has no "
            "mesh objects in itself or recursive child collections")

    resource_uid = ensure_uid(collection)
    filename = _clean_fbx_filename(resource_name)
    resolved_output_dir = os.path.abspath(
        bpy.path.abspath(os.fspath(output_dir)))
    filepath = os.path.join(resolved_output_dir, filename)
    resolved_source_root = None
    if isinstance(source_root, (str, os.PathLike)) and str(source_root).strip():
        resolved_source_root = _resolved_source_root(source_root)
        _assert_output_under_root(resolved_output_dir, resolved_source_root)
    _assert_existing_target_uid(filepath, resource_uid)

    for obj in objects:
        ensure_uid(obj)
        if obj.data is not None:
            ensure_uid(obj.data)
    duplicate_object_uids = find_duplicate_uids(objects)
    if duplicate_object_uids:
        uid = sorted(duplicate_object_uids)[0]
        raise MHValidationError(
            "MH_E_DUPLICATE_NODE_UID", [uid],
            "mesh objects in the selected collection share one mh_uid")

    used_materials = []
    seen_materials = set()
    for obj in objects:
        for slot in obj.material_slots:
            material = slot.material
            if material is None or material.as_pointer() in seen_materials:
                continue
            seen_materials.add(material.as_pointer())
            ensure_uid(material)
            used_materials.append(material)
    duplicate_material_uids = find_duplicate_uids(used_materials)
    if duplicate_material_uids:
        uid = sorted(duplicate_material_uids)[0]
        raise MHValidationError(
            "MH_E_DUPLICATE_RESOURCE_UID", [uid],
            "materials in the selected collection share one mh_uid")

    materials = used_materials
    if lod_structure is None:
        material_slots = _extract_material_slots(objects)
    else:
        _level0, _collection0, level0_objects = payload_levels[0]
        material_slots = _extract_material_slots(level0_objects)
        base_slots = _object_material_slot_pairs(level0_objects)
        for level, _child, level_objects in payload_levels[1:]:
            _extract_material_slots(level_objects)
            missing_slots = sorted(
                _object_material_slot_pairs(level_objects) - base_slots
            )
            if missing_slots:
                detail = ", ".join(
                    f"{name} ({uid})" for name, uid in missing_slots)
                raise MHValidationError(
                    "MH_E_LOD_SLOT_NOT_IN_BASE",
                    [uid for _name, uid in missing_slots],
                    f"LOD{level} uses slots absent from LOD0: {detail}")
    scene = _find_export_scene(collection)
    window = bpy.context.window
    original_scene = window.scene if window is not None else None
    try:
        if window is not None:
            window.scene = scene
        depsgraph = bpy.context.evaluated_depsgraph_get()
        records = []
        for level, _child, level_objects in payload_levels:
            records.extend(
                (level, obj[PROP_UID], _record_for_object(obj, depsgraph))
                for obj in level_objects)
        aux_records = []
        for obj in aux_objects:
            if obj.type == "MESH":
                aux_records.append(MeshAuxRecord(
                    kind="collision",
                    name=obj.name,
                    mesh=_record_for_object(obj, depsgraph),
                ))
            else:
                transform = tuple(
                    component for row in obj.matrix_world for component in row)
                aux_records.append(MeshAuxRecord(
                    kind="socket",
                    name=obj.name,
                    transform=transform,
                ))
        combined_hash = mesh_content_hash(records, aux_records)
    finally:
        if window is not None and original_scene is not None:
            window.scene = original_scene

    resource = MeshResource(
        uid=resource_uid,
        name=resource_name,
        content_hash=combined_hash,
        material_slots=material_slots,
        properties=_bag(collection),
    )
    lod_policy = "authored" if lod_structure is not None else "generated"
    passport_slot_objects = (
        payload_levels[0][2] if lod_structure is not None else objects)
    passport_slots = _passport_material_slots(
        materials, passport_slot_objects)
    passport = make_fbx_passport(
        resource_uid=resource_uid,
        name=nfc(resource_name),
        lod_levels=[level for level, _child, _objects in payload_levels],
        lod_policy=lod_policy,
        geometry_hash=combined_hash,
        material_slots=passport_slots,
        properties=resource.properties,
        exporter=f"mh4blend {EXPORTER_VERSION}",
    )
    passport_text = canonical_passport(passport)
    validation = {"errors": [], "warnings": []}
    if lod_structure is not None and lod_structure["ignored_aux"]:
        ignored = ", ".join(
            f"LOD{level} '{name}'"
            for level, name in lod_structure["ignored_aux"])
        validation["warnings"].append({
            "code": "MH_W_LOD_AUX_NODE_IGNORED",
            "subjects": [resource_uid],
            "message": f"higher-LOD auxiliary nodes were ignored: {ignored}",
        })

    payload_updates = [{"filepath": filepath, "written": False}]
    written = False
    tmp = os.path.join(
        resolved_output_dir,
        f".{filename}.mh-tmp-{os.getpid()}-{uuid.uuid4().hex}")
    if not dry_run:
        os.makedirs(resolved_output_dir, exist_ok=True)
        lod_properties = payload_levels if lod_structure is not None else []
        implicit_lod0 = aux_objects if lod_structure is not None \
            else export_objects
        with payload_lock(filepath, source_root=resolved_source_root):
            # The preflight above gives an early artist-facing error; this
            # locked recheck closes the two-writer collision race.
            _assert_existing_target_uid(filepath, resource_uid)
            try:
                with _temporary_passport_properties(
                        export_objects, passport_text):
                    with _temporary_lod_level_properties(
                            lod_properties, implicit_lod0):
                        with _temporary_selection_context(
                                scene, export_objects):
                            with _temporary_ue_centimeter_export_state(
                                    export_objects):
                                _export_selected_fbx(tmp)
                staged = read_fbx_passport(tmp)
                mesh_model_count = sum(
                    obj.type == "MESH" for obj in export_objects)
                if (staged.canonical_text != passport_text
                        or staged.copy_count != mesh_model_count):
                    raise ValueError(
                        "MH_E_PASSPORT_INVALID: staged FBX Carrier B does "
                        "not match the intended passport/model count")
                _assert_existing_target_uid(filepath, resource_uid)
                os.replace(tmp, filepath)
                payload_updates[0]["written"] = True
                written = True
            finally:
                with contextlib.suppress(OSError):
                    os.remove(tmp)

    return {
        "ok": True,
        "filepath": filepath,
        "written": written,
        "objects_exported": len(export_objects),
        "payload_updates": payload_updates,
        "resource": resource,
        "materials": materials,
        "passport": passport,
        "passport_text": passport_text,
        "validation": validation,
    }
