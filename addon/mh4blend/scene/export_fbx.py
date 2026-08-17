"""Standalone FBX export for one explicitly selected Blender collection.

The collection is treated like dag4blend's ``Col.Joined`` mode: direct mesh
objects and mesh objects in recursive child collections form one static-mesh
resource.  Sibling collections and unrelated scene objects are never selected.
No scene names, bundle directories or texture roots participate in this API.
"""

import contextlib
import os

import bpy
from mathutils import Matrix

from ..core.canonical import nfc, resource_filename
from ..core.meshser import mesh_content_hash
from ..core.model import Manifest, MeshResource
from ..core.uid import PROP_UID, ensure_uid, find_duplicate_uids
from ..core.validate import MHValidationError, ValidationWarning, validate_manifest
from .composite_extract import _bag
from .material_extract import extract_materials_from_objects
from .mesh_extract import _record_for_object
from .source_manifest import (
    MANIFEST_NAME,
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
EXPORTER_VERSION = "0.3.0"


def collect_collection_mesh_objects(collection):
    """Return the selected collection's recursive, de-duplicated mesh set.

    Blender's ``Collection.all_objects`` is exactly the membership needed for
    dag4blend ``Col.Joined`` semantics.  Pointer de-duplication also makes the
    intent explicit for collections linked into more than one hierarchy.
    """
    if collection is None:
        raise ValueError("collection is required")
    objects = getattr(collection, "all_objects", collection.objects)
    result = []
    seen = set()
    for obj in objects:
        if obj.type != "MESH":
            continue
        identity = obj.as_pointer()
        if identity in seen:
            continue
        seen.add(identity)
        result.append(obj)
    return result


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


def _material_entry(material):
    payload = material.disk_payload()
    return {
        "uid": material.uid,
        "kind": "material",
        "name": nfc(material.name),
        "shader_class": payload["shader_class"],
        "params": payload["params"],
        "textures": payload["textures"],
        "content_hash": material.content_hash,
    }


def _export_selected_fbx(filepath):
    """Operator seam kept separate for crash-protocol integration tests."""
    bpy.ops.export_scene.fbx(filepath=filepath, **FBX_EXPORT_KWARGS)


def export_fbx_collection(
        collection, output_dir, *, dry_run=False, registry_path=""):
    """Export one selected collection as one FBX static-mesh resource.

    Returns a structured report containing the domain objects and their
    manifest-ready entries. The exporter stages the incremental manifest
    before replacing the payload and promotes it only after FBX succeeds.
    """
    if collection is None:
        raise ValueError("collection is required")
    if not isinstance(output_dir, (str, os.PathLike)) or not str(output_dir).strip():
        raise ValueError("output_dir is required")

    objects = collect_collection_mesh_objects(collection)
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

    materials, material_slots = extract_materials_from_objects(objects)
    scene = _find_export_scene(collection)
    window = bpy.context.window
    original_scene = window.scene if window is not None else None
    try:
        if window is not None:
            window.scene = scene
        depsgraph = bpy.context.evaluated_depsgraph_get()
        records = [
            (obj[PROP_UID], _record_for_object(obj, depsgraph))
            for obj in objects
        ]
    finally:
        if window is not None and original_scene is not None:
            window.scene = original_scene

    resource = MeshResource(
        uid=resource_uid,
        name=collection.name,
        content_hash=mesh_content_hash(records),
        material_slots=material_slots,
        properties=_bag(collection),
    )
    filename = resource_filename(
        collection.name, resource_uid, ".mesh.fbx")
    resolved_output_dir = os.path.abspath(
        bpy.path.abspath(os.fspath(output_dir)))
    filepath = os.path.join(resolved_output_dir, filename)

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
    if resource.properties:
        resource_entry["properties"] = resource.properties
    material_entries = [
        _material_entry(material) for material in materials]

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
        bundle_name=collection.name,
        blend_file=os.path.basename(bpy.data.filepath) or "untitled.blend",
        exporter_version=EXPORTER_VERSION,
        meshes=[resource],
        materials=materials,
    ), registry=registry)
    validation["warnings"] = adapter_warnings + validation.get("warnings", [])
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
        }

    written = False
    manifest_written = False
    manifest_path = os.path.join(resolved_output_dir, MANIFEST_NAME)
    if not dry_run:
        manifest = prepare_manifest_update(
            resolved_output_dir,
            resources=[resource_entry],
            materials=material_entries,
            exporter_version=EXPORTER_VERSION,
            blend_file=os.path.basename(bpy.data.filepath) or "untitled.blend",
        )
        stage_manifest(resolved_output_dir, manifest)
        tmp = filepath + ".tmp"
        with contextlib.suppress(OSError):
            os.remove(tmp)
        try:
            with _temporary_selection_context(scene, objects):
                with _temporary_ue_centimeter_export_state(objects):
                    _export_selected_fbx(tmp)
            os.replace(tmp, filepath)
            written = True
            manifest_path = commit_staged_manifest(resolved_output_dir)
            manifest_written = True
        finally:
            with contextlib.suppress(OSError):
                os.remove(tmp)

    return {
        "ok": True,
        "filepath": filepath,
        "written": written,
        "manifest_path": manifest_path,
        "manifest_written": manifest_written,
        "objects_exported": len(objects),
        "resource": resource,
        "resource_entry": resource_entry,
        "materials": materials,
        "material_entries": material_entries,
        "validation": validation,
    }
