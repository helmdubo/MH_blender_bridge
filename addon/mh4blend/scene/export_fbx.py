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
from ..core.source_resolver import (
    assert_source_snapshot_stable,
    pending_writer_uid,
    resolve_resource,
    resolve_resource_for_import,
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
EXPORTER_VERSION = "0.4.0"


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


def export_fbx_collection(
        collection, output_dir, *, dry_run=False, registry_path="",
        export_materials=False, source_root="",
        texture_policy="transitional"):
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
        bundle_name=collection.name,
        blend_file=os.path.basename(bpy.data.filepath) or "untitled.blend",
        exporter_version=EXPORTER_VERSION,
        meshes=[resource],
        materials=materials,
    ), registry=registry)
    validation["warnings"] = adapter_warnings + validation.get("warnings", [])
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
    if not dry_run:
        resolved_registry_path = (
            os.path.abspath(bpy.path.abspath(registry_path))
            if registry_path else None)
        mesh_recovery = pending is not None \
            and pending.kind == "static_mesh"
        if mesh_recovery:
            mesh_resolution = resolve_resource_for_writer(
                resolved_source_root,
                resource_uid,
                expected_kind="static_mesh",
                expected_source=filename,
                registry_path=resolved_registry_path,
                texture_policy=normalized_policy,
            )
            mesh_owner = mesh_resolution.owner
            mesh_snapshot = mesh_resolution.snapshot
        else:
            mesh_snapshot = scan_source_root(
                resolved_source_root,
                registry_path=resolved_registry_path,
                texture_policy=normalized_policy,
            )
            mesh_owner = resolve_resource_for_import(
                mesh_snapshot, resource_uid, expected_kind="static_mesh")

        if mesh_owner is not None:
            expected_manifest = os.path.normcase(os.path.normpath(
                manifest_path))
            actual_manifest = os.path.normcase(os.path.normpath(
                mesh_owner.owning_manifest_path))
            expected_payload = os.path.normcase(os.path.normpath(filepath))
            actual_payload = os.path.normcase(os.path.normpath(
                mesh_owner.payload_path))
            if (actual_manifest != expected_manifest
                    or actual_payload != expected_payload
                    or mesh_owner.manifest_row["source"] != filename):
                raise ValueError(
                    "MH_E_INVALID_EXPORT_MANIFEST: existing static mesh "
                    f"{resource_uid} must be updated at its exact owning "
                    "manifest and payload path")

        payload_hash_matches = (
            mesh_owner is not None
            and mesh_owner.manifest_row["content_hash"]
            == resource_entry["content_hash"]
            and os.path.isfile(mesh_owner.payload_path)
        )
        write_mesh_payload = mesh_recovery or not payload_hash_matches
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
        tmp = filepath + ".tmp"
        with contextlib.suppress(OSError):
            os.remove(tmp)
        try:
            if write_mesh_payload:
                with _temporary_selection_context(scene, objects):
                    with _temporary_ue_centimeter_export_state(objects):
                        _export_selected_fbx(tmp)
                os.replace(tmp, filepath)
                written = True
            manifest_path = commit_staged_manifest(resolved_output_dir)
            manifest_written = True
        except BaseException:
            abandon_staged_manifest(resolved_output_dir)
            raise
        finally:
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
        "objects_exported": len(objects),
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
