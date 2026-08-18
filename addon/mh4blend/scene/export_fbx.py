"""Standalone FBX export for one explicitly selected Blender collection.

The collection is treated like dag4blend's ``Col.Joined`` mode: direct mesh
objects and mesh objects in recursive child collections form one static-mesh
resource.  Sibling collections and unrelated scene objects are never selected.
No scene names, bundle directories or texture roots participate in this API.
"""

import contextlib
import os
import re

import bpy
from mathutils import Matrix

from ..core.canonical import nfc, resource_filename, validate_resource_name
from ..core.meshser import MeshAuxRecord, mesh_content_hash
from ..core.model import Manifest, MeshResource
from ..core.source_resolver import (
    assert_source_snapshot_stable,
    pending_writer_uid,
    resolve_resource,
    resolve_resource_for_writer,
    scan_source_root,
)
from ..core.uid import PROP_UID, ensure_uid, find_duplicate_uids
from ..core.validate import MHValidationError, ValidationWarning, validate_manifest
from .composite_extract import _bag
from .export_material import (
    prepare_material_resource_export,
    write_prepared_material,
)
from .material_extract import extract_materials_from_objects
from .mesh_extract import _record_for_object
from .source_manifest import (
    MANIFEST_NAME,
    abandon_staged_manifest,
    commit_staged_manifest,
    prepare_manifest_update,
    stage_manifest,
)

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
EXPORTER_VERSION = "0.5.0"


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


def _material_owners(snapshot, materials):
    owners = {}
    for material in materials:
        resolved = resolve_resource(
            snapshot, material.uid, expected_kind="material")
        if resolved is None:
            continue
        owners[material.uid] = {
            "payload_path": resolved.payload_path,
            "owning_manifest_path": resolved.owning_manifest_path,
            "manifest_row": resolved.manifest_row,
        }
    return owners


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


def _prepare_materials_tolerant(
        materials, output_dir, *, source_root, texture_policy, owners):
    prepared = []
    warnings = []
    for material in materials:
        owner = owners.get(material.uid)
        try:
            prepared.append(prepare_material_resource_export(
                material,
                output_dir,
                source_root=source_root,
                texture_policy=texture_policy,
                target_payload_path=(
                    owner["payload_path"] if owner else None),
                owning_manifest_path=(
                    owner["owning_manifest_path"] if owner else None),
                existing_source=(
                    owner["manifest_row"]["source"] if owner else None),
            ))
        except (OSError, RuntimeError, ValueError) as exc:
            warnings.append(ValidationWarning(
                "MH_W_MATERIAL_NOT_FOUND", [material.uid],
                f"material export preparation failed; geometry will still "
                f"export: {exc}",
            ).disk_dict())
    return prepared, warnings


def _write_material_resources(
        prepared_materials, *, source_root, texture_policy, dry_run):
    updates = []
    warnings = []
    for prepared in prepared_materials:
        owner_dir = os.path.dirname(prepared.owning_manifest_path)
        written = False
        manifest_written = False
        try:
            if not dry_run:
                manifest = prepare_manifest_update(
                    owner_dir,
                    resources=[prepared.resource_row],
                    exporter_version=EXPORTER_VERSION,
                    blend_file=os.path.basename(bpy.data.filepath) or None,
                    source_root=source_root,
                )
                stage_manifest(owner_dir, manifest)
                try:
                    written = write_prepared_material(
                        prepared,
                        source_root=source_root,
                        texture_policy=texture_policy,
                        force=manifest.is_recovery,
                    )
                    commit_staged_manifest(owner_dir)
                    manifest_written = True
                except BaseException:
                    abandon_staged_manifest(owner_dir)
                    raise
            updates.append({
                "ok": True,
                "uid": prepared.document["uid"],
                "path": prepared.payload_path,
                "written": written,
                "manifest_path": prepared.owning_manifest_path,
                "manifest_written": manifest_written,
            })
        except (OSError, RuntimeError, ValueError) as exc:
            uid = prepared.document["uid"]
            warnings.append(ValidationWarning(
                "MH_W_MATERIAL_NOT_FOUND", [uid],
                f"material export failed; geometry remains exported: {exc}",
            ).disk_dict())
            updates.append({
                "ok": False,
                "uid": uid,
                "path": prepared.payload_path,
                "written": False,
                "manifest_path": prepared.owning_manifest_path,
                "manifest_written": False,
                "error": str(exc),
            })
    return updates, warnings


def _export_selected_fbx(filepath):
    """Operator seam kept separate for crash-protocol integration tests."""
    bpy.ops.export_scene.fbx(filepath=filepath, **FBX_EXPORT_KWARGS)


@contextlib.contextmanager
def _temporary_lod_level_properties(levels):
    """Expose combined-LOD membership to FBX and restore authoring data."""
    saved = []
    try:
        for level, _collection, objects in levels:
            for obj in objects:
                existed = "mh_lod_level" in obj
                previous = obj["mh_lod_level"] if existed else None
                saved.append((obj, existed, previous))
                obj["mh_lod_level"] = int(level)
        yield
    finally:
        for obj, existed, previous in saved:
            if obj is None or obj.name not in bpy.data.objects:
                continue
            if existed:
                obj["mh_lod_level"] = previous
            elif "mh_lod_level" in obj:
                del obj["mh_lod_level"]


def _static_mesh_descriptor(row):
    """Return normalized metadata carried by an FBX rewrite decision.

    Optional manifest fields are normalized to their frozen-v1 defaults so
    omission and an explicit default compare equal. Combined LOD membership
    and geometry live in the meshser:2 content hash; ``lod_policy`` remains
    descriptor metadata.
    """
    lod_policy = row.get("lod_policy", "generated")
    return {
        "name": row["name"],
        "material_slots": row.get("material_slots", []),
        "properties": row.get("properties", {}),
        "lod_policy": lod_policy,
    }


def _bind_existing_mesh_location(
        owner, resource_entry, payload_exports):
    """Keep an owned v1 mesh at the path already assigned to its UID."""
    owner_dir = os.path.dirname(owner.owning_manifest_path)
    existing_row = owner.manifest_row
    existing_sources = {0: existing_row["source"]}
    for payload in payload_exports:
        source = existing_sources.get(payload["level"], payload["filename"])
        payload["filename"] = source
        payload["filepath"] = os.path.join(
            owner_dir, *source.split("/"))

    resource_entry["source"] = payload_exports[0]["filename"]
    return owner_dir, owner.payload_path


def export_fbx_collection(
        collection, output_dir, *, dry_run=False, registry_path="",
        export_materials=False, source_root="",
        texture_policy="transitional"):
    """Export one selected collection as one static-mesh resource.

    A regular collection writes one FBX. A recognized dag4blend ``.lods``
    hierarchy also writes one FBX containing every authored level; each model
    node carries a temporary integer ``mh_lod_level`` custom property.

    Returns a structured report containing the domain objects and their
    manifest-ready entries. The exporter stages the incremental manifest
    before replacing the payload and promotes it only after FBX succeeds.
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

    if lod_structure is None:
        materials, material_slots = extract_materials_from_objects(objects)
    else:
        _level0, _collection0, level0_objects = payload_levels[0]
        materials, material_slots = extract_materials_from_objects(
            level0_objects)
        base_slots = _object_material_slot_pairs(level0_objects)
        for level, _child, level_objects in payload_levels[1:]:
            _level_materials, _level_slots = \
                extract_materials_from_objects(level_objects)
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
    filename = resource_filename(
        resource_name, resource_uid, ".mesh.fbx")
    resolved_output_dir = os.path.abspath(
        bpy.path.abspath(os.fspath(output_dir)))
    filepath = os.path.join(resolved_output_dir, filename)
    payload_exports = [{
        "level": 0,
        "objects": export_objects,
        "filename": filename,
        "content_hash": combined_hash,
    }]
    for payload in payload_exports:
        payload["filepath"] = os.path.join(
            resolved_output_dir, payload["filename"])

    prepared_materials = []
    material_updates = []
    source_snapshot = None
    material_owners = {}
    missing_material_uids = []
    material_prepare_warnings = []
    recovery_material_updates = []
    recovery_material_entries = []
    deferred_materials = []
    normalized_policy = str(texture_policy).lower()
    resolved_source_root = None
    if isinstance(source_root, (str, os.PathLike)) and str(source_root).strip():
        resolved_source_root = _resolved_source_root(source_root)
        _assert_output_under_root(resolved_output_dir, resolved_source_root)
    if export_materials:
        if resolved_source_root is None:
            resolved_source_root = _resolved_source_root(source_root)
    materials_to_prepare = list(materials)
    pending = pending_writer_uid(resolved_source_root) \
        if resolved_source_root is not None else None
    if pending is not None:
        if pending.kind == "material":
            if not export_materials:
                raise ValueError(
                    "MH_E_INVALID_EXPORT_MANIFEST: pending material "
                    f"{pending.uid} blocks geometry-only FBX export")
            if dry_run:
                raise ValueError(
                    "MH_E_INVALID_EXPORT_MANIFEST: dry-run cannot recover a "
                    "pending material transaction")
            material_by_uid = {material.uid: material for material in materials}
            recovery_material = material_by_uid.get(pending.uid)
            if recovery_material is None:
                raise ValueError(
                    "MH_E_INVALID_EXPORT_MANIFEST: pending material recovery "
                    f"{pending.uid} is not used by the selected FBX")
            resolution = resolve_resource_for_writer(
                resolved_source_root,
                pending.uid,
                expected_kind="material",
                registry_path=(
                    os.path.abspath(bpy.path.abspath(registry_path))
                    if registry_path else None),
                texture_policy=normalized_policy,
            )
            source_snapshot = resolution.snapshot
            recovery_owners = {
                pending.uid: {
                    "payload_path": resolution.owner.payload_path,
                    "owning_manifest_path": (
                        resolution.owner.owning_manifest_path),
                    "manifest_row": resolution.owner.manifest_row,
                },
            }
            recovery_prepared, recovery_prepare_warnings = \
                _prepare_materials_tolerant(
                    [recovery_material],
                    resolved_output_dir,
                    source_root=resolved_source_root,
                    texture_policy=normalized_policy,
                    owners=recovery_owners,
                )
            if recovery_prepare_warnings or not recovery_prepared:
                detail = recovery_prepare_warnings[0]["message"] \
                    if recovery_prepare_warnings else "recovery preparation failed"
                raise ValueError(
                    "MH_E_INVALID_EXPORT_MANIFEST: pending material "
                    f"{pending.uid} cannot recover: {detail}")
            recovery_material_entries = [
                recovery_prepared[0].resource_row]
            recovery_material_updates, recovery_write_warnings = \
                _write_material_resources(
                    recovery_prepared,
                    source_root=resolved_source_root,
                    texture_policy=normalized_policy,
                    dry_run=False,
                )
            if recovery_write_warnings or not recovery_material_updates[0]["ok"]:
                detail = recovery_material_updates[0].get(
                    "error", "recovery write failed")
                raise ValueError(
                    "MH_E_INVALID_EXPORT_MANIFEST: pending material "
                    f"{pending.uid} recovery failed: {detail}")
            materials_to_prepare = [
                material for material in materials
                if material.uid != pending.uid]
        elif pending.kind == "static_mesh":
            if pending.uid != resource_uid:
                raise ValueError(
                    "MH_E_INVALID_EXPORT_MANIFEST: pending static mesh "
                    f"{pending.uid} is not the selected FBX {resource_uid}")
            if dry_run:
                raise ValueError(
                    "MH_E_INVALID_EXPORT_MANIFEST: dry-run cannot recover a "
                    "pending static mesh transaction")
            if export_materials:
                deferred_materials = materials_to_prepare
            materials_to_prepare = []
        else:
            raise ValueError(
                "MH_E_INVALID_EXPORT_MANIFEST: pending "
                f"{pending.kind} {pending.uid} blocks FBX export")
    if materials_to_prepare and resolved_source_root is not None:
        resolved_registry_path = (
            os.path.abspath(bpy.path.abspath(registry_path))
            if registry_path else None)
        source_snapshot = scan_source_root(
            resolved_source_root,
            registry_path=resolved_registry_path,
            texture_policy=normalized_policy)
        material_owners = _material_owners(
            source_snapshot, materials_to_prepare)
    if export_materials:
        prepared_materials, material_prepare_warnings = \
            _prepare_materials_tolerant(
                materials_to_prepare,
                resolved_output_dir,
                source_root=resolved_source_root,
                texture_policy=normalized_policy,
                owners=material_owners,
            )
    else:
        missing_material_uids = sorted(
            material.uid for material in materials
            if material.uid not in material_owners)

    resource_entry = {
        "uid": resource.uid,
        "kind": "static_mesh",
        "name": nfc(resource.name),
        "source": filename,
        "content_hash": resource.content_hash,
    }
    if material_slots:
        resource_entry["material_slots"] = [
            {"slot_name": nfc(slot.slot_name),
             "material_uid": slot.material_uid}
            for slot in material_slots
        ]
    if lod_structure is not None:
        resource_entry["lod_policy"] = "authored"
    if resource.properties:
        resource_entry["properties"] = resource.properties
    material_entries = recovery_material_entries + [
        prepared.resource_row for prepared in prepared_materials]

    registry = None
    adapter_warnings = []
    if registry_path:
        resolved_registry = os.path.abspath(bpy.path.abspath(registry_path))
        try:
            with open(resolved_registry, encoding="utf-8") as stream:
                registry = stream.read()
        except (OSError, UnicodeError) as exc:
            adapter_warnings.append(ValidationWarning(
                "MH_W_REGISTRY_INVALID", [resource_uid], str(exc)).disk_dict())
    validation = validate_manifest(Manifest(
        bundle_uid=resource_uid,
        bundle_name=resource_name,
        blend_file=os.path.basename(bpy.data.filepath) or "untitled.blend",
        exporter_version=EXPORTER_VERSION,
        meshes=[resource],
        materials=materials,
    ), registry=registry)
    validation["warnings"] = adapter_warnings + validation.get("warnings", [])
    if lod_structure is not None and lod_structure["ignored_aux"]:
        ignored = ", ".join(
            f"LOD{level} '{name}'"
            for level, name in lod_structure["ignored_aux"])
        validation["warnings"].append(ValidationWarning(
            "MH_W_LOD_AUX_NODE_IGNORED",
            [resource_uid],
            f"higher-LOD auxiliary nodes were ignored: {ignored}",
        ).disk_dict())
    validation["warnings"].extend(material_prepare_warnings)
    if source_snapshot is not None:
        validation["warnings"].extend({
            "code": diagnostic.code,
            "subjects": ([diagnostic.uid] if diagnostic.uid else []),
            "message": diagnostic.message,
        } for diagnostic in source_snapshot.diagnostics)
    if missing_material_uids:
        validation["warnings"].append(ValidationWarning(
            "MH_W_MATERIAL_NOT_FOUND",
            missing_material_uids,
            "FBX references materials that are not exported under Project "
            "Source Root; enable Export Materials or use Export materials",
        ).disk_dict())
    for prepared in prepared_materials:
        validation["warnings"].extend(
            diagnostic.disk_dict() for diagnostic in prepared.diagnostics)
    if validation["errors"]:
        return {
            "ok": False,
            "filepath": filepath,
            "written": False,
            "manifest_path": os.path.join(resolved_output_dir, MANIFEST_NAME),
            "manifest_written": False,
            "validation": validation,
            "resource": resource,
            "resource_entry": resource_entry,
            "materials": materials,
            "material_entries": material_entries,
            "material_updates": material_updates,
        }

    written = False
    manifest_written = False
    manifest_path = os.path.join(resolved_output_dir, MANIFEST_NAME)
    payload_updates = [{
        "level": payload["level"],
        "filepath": payload["filepath"],
        "written": False,
    } for payload in payload_exports]
    if not dry_run:
        resolved_registry_path = (
            os.path.abspath(bpy.path.abspath(registry_path))
            if registry_path else None)
        mesh_resolution = resolve_resource_for_writer(
            resolved_source_root,
            resource_uid,
            expected_kind="static_mesh",
            expected_source=None,
            registry_path=resolved_registry_path,
            texture_policy=normalized_policy,
        )
        mesh_owner = mesh_resolution.owner
        mesh_snapshot = mesh_resolution.snapshot
        mesh_recovery = mesh_resolution.recovery

        if mesh_owner is not None:
            if "lods" in mesh_owner.manifest_row:
                raise ValueError(
                    "MH_E_DEPRECATED_LOD_ROWS: static_mesh rows with lods[] "
                    "must be migrated before combined-LOD export")
            resolved_output_dir, filepath = _bind_existing_mesh_location(
                mesh_owner, resource_entry, payload_exports)
            manifest_path = mesh_owner.owning_manifest_path
            for payload, update in zip(payload_exports, payload_updates):
                update["filepath"] = payload["filepath"]

        existing_payloads = {}
        descriptor_changed = False
        if mesh_owner is not None:
            existing_row = mesh_owner.manifest_row
            descriptor_changed = (
                _static_mesh_descriptor(existing_row)
                != _static_mesh_descriptor(resource_entry)
            )
            existing_payloads[0] = {
                "source": existing_row["source"],
                "content_hash": existing_row["content_hash"],
            }
        for payload, update in zip(payload_exports, payload_updates):
            existing = existing_payloads.get(payload["level"])
            hash_matches = (
                existing is not None
                and existing["source"] == payload["filename"]
                and existing["content_hash"] == payload["content_hash"]
                and os.path.isfile(payload["filepath"])
            )
            payload["write"] = (
                mesh_recovery
                or not hash_matches
                or (payload["level"] == 0 and descriptor_changed)
            )
        assert_source_snapshot_stable(mesh_snapshot)
        manifest = prepare_manifest_update(
            resolved_output_dir,
            resources=[resource_entry],
            exporter_version=EXPORTER_VERSION,
            blend_file=os.path.basename(bpy.data.filepath) or None,
            source_root=resolved_source_root,
        )
        stage_manifest(
            resolved_output_dir,
            manifest,
            snapshot_guard=lambda: assert_source_snapshot_stable(
                mesh_snapshot),
        )
        tmp_paths = [payload["filepath"] + ".tmp"
                     for payload in payload_exports]
        for tmp in tmp_paths:
            with contextlib.suppress(OSError):
                os.remove(tmp)
        try:
            for payload, update in zip(payload_exports, payload_updates):
                if not payload["write"]:
                    continue
                tmp = payload["filepath"] + ".tmp"
                if lod_structure is not None:
                    lod_levels = [
                        (level, child, level_objects)
                        for level, child, level_objects in payload_levels
                    ]
                else:
                    lod_levels = []
                with _temporary_lod_level_properties(lod_levels):
                    with _temporary_selection_context(
                            scene, payload["objects"]):
                        with _temporary_ue_centimeter_export_state(
                                payload["objects"]):
                            _export_selected_fbx(tmp)
                os.replace(tmp, payload["filepath"])
                update["written"] = True
                written = True
            manifest_path = commit_staged_manifest(resolved_output_dir)
            manifest_written = True
        except BaseException:
            abandon_staged_manifest(resolved_output_dir)
            raise
        finally:
            for tmp in tmp_paths:
                with contextlib.suppress(OSError):
                    os.remove(tmp)
        if deferred_materials:
            resolved_registry_path = (
                os.path.abspath(bpy.path.abspath(registry_path))
                if registry_path else None)
            source_snapshot = scan_source_root(
                resolved_source_root,
                registry_path=resolved_registry_path,
                texture_policy=normalized_policy)
            deferred_owners = _material_owners(
                source_snapshot, deferred_materials)
            deferred_prepared, deferred_warnings = \
                _prepare_materials_tolerant(
                    deferred_materials,
                    resolved_output_dir,
                    source_root=resolved_source_root,
                    texture_policy=normalized_policy,
                    owners=deferred_owners,
                )
            prepared_materials.extend(deferred_prepared)
            material_entries.extend(
                prepared.resource_row for prepared in deferred_prepared)
            validation["warnings"].extend(deferred_warnings)
            validation["warnings"].extend({
                "code": diagnostic.code,
                "subjects": ([diagnostic.uid] if diagnostic.uid else []),
                "message": diagnostic.message,
            } for diagnostic in source_snapshot.diagnostics)
        if export_materials:
            normal_updates, material_write_warnings = \
                _write_material_resources(
                    prepared_materials,
                    source_root=resolved_source_root,
                    texture_policy=normalized_policy,
                    dry_run=False,
                )
            material_updates = recovery_material_updates + normal_updates
            validation["warnings"].extend(material_write_warnings)

    if not dry_run:
        successful_material_uids = {
            row["uid"] for row in material_updates if row["ok"]}
        material_entries = [
            row for row in material_entries
            if row["uid"] in successful_material_uids]

    return {
        "ok": True,
        "filepath": filepath,
        "written": written,
        "manifest_path": manifest_path,
        "manifest_written": manifest_written,
        "objects_exported": len(export_objects),
        "payload_updates": payload_updates,
        "resource": resource,
        "resource_entry": resource_entry,
        "materials": materials,
        "material_entries": material_entries,
        "material_updates": material_updates,
        "materials_exported": (
            len(prepared_materials) if dry_run else
            sum(1 for row in material_updates if row["ok"])),
        "validation": validation,
    }
