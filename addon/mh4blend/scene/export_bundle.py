"""Bundle exporter (B10): gathers the semantic model from the GEOMETRY and
COMPOSITS scenes and writes the bundle per the §1.1 atomicity protocol.

Write order: a prepared export_manifest.json.tmp is the fail-closed marker;
changed files use <name>.tmp + rename; unchanged files (by content_hash
against the previous manifest) are never touched; the prepared manifest is
promoted last; deletions are computed as sources(previous manifest) -
sources(new manifest), never a directory sweep. Validation errors abort
before any file is written.

FBX geometry goes out with the studio-canonical settings and the
meters->centimeters temporary state (ported from
reference/studio_scripts/blender_export_matdata.py::
_temporary_ue_centimeter_export_state).
"""

import contextlib
import json
import os

import bpy
from mathutils import Matrix

from ..core.model import (
    Composite,
    Manifest,
    MeshResource,
    composite_disk_dict,
    composite_hash,
    manifest_disk_dict,
)
from ..core.uid import PROP_UID, ensure_uid, find_duplicate_uids
from ..core.validate import (
    MHValidationError,
    ValidationError,
    ValidationWarning,
    build_report,
    validate_manifest,
)
from .composite_extract import _bag, extract_composites
from .material_extract import extract_collection_materials
from .mesh_extract import extract_mesh_records
from ..core.meshser import mesh_content_hash

__all__ = ["export_bundle", "FBX_EXPORT_KWARGS"]

EXPORTER_VERSION = "0.2.0"
PENDING_MANIFEST_NAME = "export_manifest.json.tmp"

# Canonical FBX settings — the owner's working script, verbatim.
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
    """Port of the reference context manager: geometry and translations are
    temporarily scaled x100 (m -> cm) and rolled back — the deliberate
    alternative to global_scale=100. The reference's skeletal branch
    (only_deform etc.) is out of MVP scope (D29) and not ported."""
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
        if obj.type == "MESH" and obj.data is not None \
                and obj.data not in seen_mesh_data:
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


# ---------------------------------------------------------------------------
# Gathering
# ---------------------------------------------------------------------------


def _used_meshes(geo_scene):
    """Distinct mesh datablocks used by GEOMETRY objects.

    Multiple objects may intentionally share one mesh datablock.  Duplicate
    UID arbitration applies to distinct datablocks, not to each object use.
    """
    meshes = []
    seen = set()
    for obj in geo_scene.objects:
        if obj.type != "MESH" or obj.data is None:
            continue
        identity = obj.data.as_pointer()
        if identity in seen:
            continue
        seen.add(identity)
        meshes.append(obj.data)
    return meshes


def _collect_duplicate_errors(geo_scene, cmp_scene):
    errors = []
    objects = list(geo_scene.objects) + list(cmp_scene.objects)
    for uid, _owners in find_duplicate_uids(objects).items():
        errors.append(ValidationError(
            "MH_E_DUPLICATE_NODE_UID", [uid],
            "objects share one mh_uid (Ctrl+D?)"))
    meshes = _used_meshes(geo_scene)
    collections = (list(geo_scene.collection.children)
                   + list(cmp_scene.collection.children))
    for uid, _owners in find_duplicate_uids(meshes + collections).items():
        errors.append(ValidationError(
            "MH_E_DUPLICATE_RESOURCE_UID", [uid],
            "datablocks/collections share one mh_uid (§4.1 arbitration "
            "needed)"))
    for uid, _owners in find_duplicate_uids(_used_materials(geo_scene)).items():
        errors.append(ValidationError(
            "MH_E_DUPLICATE_RESOURCE_UID", [uid],
            "materials share one mh_uid (§4.1 arbitration needed)"))
    return errors


def _used_materials(geo_scene):
    """Distinct material datablocks used by GEOMETRY mesh objects."""
    materials = []
    seen = set()
    for obj in geo_scene.objects:
        if obj.type != "MESH":
            continue
        for slot in obj.material_slots:
            material = slot.material
            if material is None:
                continue
            identity = material.as_pointer()
            if identity in seen:
                continue
            seen.add(identity)
            materials.append(material)
    return materials


def _assign_uids(geo_scene, cmp_scene):
    for collection in geo_scene.collection.children:
        ensure_uid(collection)
        for obj in collection.objects:
            ensure_uid(obj)
            if obj.type == "MESH" and obj.data is not None:
                ensure_uid(obj.data)
    for material in _used_materials(geo_scene):
        ensure_uid(material)
    for collection in cmp_scene.collection.children:
        ensure_uid(collection)
        for obj in collection.objects:
            ensure_uid(obj)


def _gather(geo_scene, cmp_scene, bundle_name, texture_root=""):
    """-> (Manifest, list[Composite], {col_uid: [objects]}, scene_errors)"""
    errors = _collect_duplicate_errors(geo_scene, cmp_scene)
    if errors:
        return None, None, None, errors

    _assign_uids(geo_scene, cmp_scene)

    depsgraph = bpy.context.evaluated_depsgraph_get()
    meshes = []
    materials_by_uid = {}
    export_objects = {}
    for collection in geo_scene.collection.children:
        records = extract_mesh_records(collection, depsgraph)
        if not records:
            errors.append(ValidationError(
                "MH_E_EMPTY_RESOURCE_COLLECTION", [collection[PROP_UID]],
                f"'{collection.name}' has no mesh objects"))
            continue
        try:
            collection_materials, material_slots = \
                extract_collection_materials(collection, texture_root)
        except MHValidationError as exc:
            errors.append(exc.as_row())
            continue
        for material in collection_materials:
            materials_by_uid.setdefault(material.uid, material)
        meshes.append(MeshResource(
            uid=collection[PROP_UID],
            name=collection.name,
            content_hash=mesh_content_hash(records),
            material_slots=material_slots,
            properties=_bag(collection),
        ))
        export_objects[collection[PROP_UID]] = [
            obj for obj in collection.objects if obj.type == "MESH"]

    try:
        composites = extract_composites(cmp_scene)
    except MHValidationError as exc:
        errors.append(exc.as_row())
        return None, None, None, errors

    # composite_ref targets outside this bundle -> external dependencies
    local_uids = ({m.uid for m in meshes} | {c.uid for c in composites})
    external = {}
    for composite in composites:
        for node in composite.nodes:
            if node.resource_uid and node.resource_uid not in local_uids:
                external[node.resource_uid] = {
                    "uid": node.resource_uid,
                    "kind": "composite" if node.kind == "composite_ref"
                            else "static_mesh",
                    "name": node.display_name,
                }

    manifest = Manifest(
        bundle_uid=ensure_uid(geo_scene),
        bundle_name=bundle_name,
        blend_file=os.path.basename(bpy.data.filepath) or "untitled.blend",
        exporter_version=EXPORTER_VERSION,
        meshes=meshes,
        composites=composites,
        materials=list(materials_by_uid.values()),
        external_dependencies=sorted(external.values(), key=lambda e: e["uid"]),
    )
    return manifest, composites, export_objects, errors


# ---------------------------------------------------------------------------
# Writing (§1.1)
# ---------------------------------------------------------------------------


def _write_json_atomic(path, doc):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=2, ensure_ascii=False)
        f.write("\n")
    os.replace(tmp, path)


def _promote_pending_manifest(pending_path, manifest_path):
    """Commit seam kept separate so crash recovery is testable."""
    os.replace(pending_path, manifest_path)


def _export_collection_fbx(geo_scene, objects, filepath):
    bpy.context.window.scene = geo_scene
    view_layer = bpy.context.view_layer
    for obj in view_layer.objects:
        obj.select_set(False)
    for obj in objects:
        obj.select_set(True)
    tmp = filepath + ".tmp"
    with _temporary_ue_centimeter_export_state(objects):
        bpy.ops.export_scene.fbx(filepath=tmp, **FBX_EXPORT_KWARGS)
    os.replace(tmp, filepath)
    for obj in objects:
        obj.select_set(False)


def _read_previous_manifest(bundle_dir):
    path = os.path.join(bundle_dir, "export_manifest.json")
    if not os.path.exists(path):
        return None
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return None  # corrupt previous state: treat as no manifest


def _cleanup_orphan_tmp(bundle_dir):
    pending_manifest = os.path.normcase(os.path.realpath(
        os.path.join(bundle_dir, PENDING_MANIFEST_NAME)))
    for root, _dirs, files in os.walk(bundle_dir):
        for name in files:
            if name.endswith(".tmp"):
                path = os.path.join(root, name)
                # A pending manifest is the fail-closed transaction marker.
                # Preserve it across a crashed run until a later successful
                # export atomically promotes it to the stable manifest.
                if os.path.normcase(os.path.realpath(path)) == pending_manifest:
                    continue
                with contextlib.suppress(OSError):
                    os.remove(path)


def _safe_bundle_target(bundle_dir, source):
    """Resolve an untrusted previous-manifest source below ``bundle_dir``."""
    if not isinstance(source, str) or not source or os.path.isabs(source):
        return None
    root = os.path.realpath(os.path.abspath(bundle_dir))
    target = os.path.realpath(os.path.abspath(os.path.join(root, source)))
    try:
        inside = os.path.commonpath(
            [os.path.normcase(root), os.path.normcase(target)]) \
            == os.path.normcase(root)
    except ValueError:
        inside = False
    if not inside or target == root:
        return None
    return target


def export_bundle(bundle_dir, geo_scene=None, cmp_scene=None,
                  bundle_name=None, dry_run=False, texture_root="",
                  registry_path=""):
    """Run the full export into `bundle_dir`. Returns a report dict:
    {"validation": mh.validation_report, "written": [...], "skipped": [...],
     "deleted": [...], "manifest_changed": bool, "manifest_written": bool,
     "ok": bool}. On validation errors nothing is written.
    dry_run=True stops after validation (the Validate button).
    """
    geo_scene = geo_scene or bpy.data.scenes["GEOMETRY"]
    cmp_scene = cmp_scene or bpy.data.scenes["COMPOSITS"]
    if bundle_name is None:
        stem = os.path.splitext(os.path.basename(bpy.data.filepath))[0]
        bundle_name = stem or "untitled"

    manifest, composites, export_objects, scene_errors = _gather(
        geo_scene, cmp_scene, bundle_name, texture_root=texture_root)

    if manifest is not None:
        registry = None
        registry_warnings = []
        if registry_path:
            try:
                with open(bpy.path.abspath(registry_path), encoding="utf-8") as f:
                    registry = f.read()
            except (OSError, UnicodeError) as exc:
                registry_warnings.append(ValidationWarning(
                    "MH_W_REGISTRY_INVALID", [manifest.bundle_uid], str(exc)))
        report = validate_manifest(manifest, registry=registry)
        rows = scene_errors + [
            ValidationError(e["code"], e["subjects"], e.get("message", ""))
            for e in report["errors"]]
        warning_rows = registry_warnings + [
            ValidationWarning(
                warning["code"], warning["subjects"],
                warning.get("message", ""))
            for warning in report.get("warnings", [])]
    else:
        rows = scene_errors
        warning_rows = []
    validation = build_report(rows, warning_rows)
    if validation["errors"] or dry_run:
        return {"ok": not validation["errors"], "validation": validation,
                "written": [], "skipped": [], "deleted": [],
                "manifest_changed": False, "manifest_written": False}

    # Prepare every semantic JSON document before touching the destination.
    # Material normalization/hashing and composite canonicalization can reject
    # invalid values; doing that work here preserves the no-partial-write
    # validation contract even if a future adapter misses a model-level check.
    composite_docs = {}
    composite_hashes = {}
    for composite in composites:
        composite_docs[composite.uid] = composite_disk_dict(composite)
        composite_hashes[composite.uid] = composite_hash(composite)
    manifest_doc = manifest_disk_dict(manifest, composite_hashes)

    os.makedirs(os.path.join(bundle_dir, "meshes"), exist_ok=True)
    _cleanup_orphan_tmp(bundle_dir)
    pending_manifest_path = os.path.join(bundle_dir, PENDING_MANIFEST_NAME)
    recovery_in_progress = os.path.exists(pending_manifest_path)
    previous = _read_previous_manifest(bundle_dir)
    prev_hashes = {}
    prev_sources = set()
    if previous:
        for entry in previous.get("resources", []):
            if not isinstance(entry, dict):
                continue
            uid = entry.get("uid")
            source = entry.get("source")
            if isinstance(uid, str):
                prev_hashes[uid] = entry.get("content_hash")
            if isinstance(source, str):
                prev_sources.add(source)
            for lod in entry.get("lods", ()):
                if isinstance(lod, dict) and isinstance(lod.get("source"), str):
                    prev_sources.add(lod["source"])

    written, skipped = [], []

    mesh_needs_write = {
        mesh.uid: recovery_in_progress or not (
            prev_hashes.get(mesh.uid) == mesh.content_hash
            and os.path.exists(os.path.join(bundle_dir, mesh.source())))
        for mesh in manifest.meshes
    }
    composite_needs_write = {
        composite.uid: recovery_in_progress or not (
            prev_hashes.get(composite.uid) == composite_hashes[composite.uid]
            and os.path.exists(os.path.join(bundle_dir, composite.filename())))
        for composite in composites
    }
    manifest_path = os.path.join(bundle_dir, "export_manifest.json")
    manifest_changed = previous != manifest_doc
    new_sources = {r["source"] for r in manifest_doc["resources"]}
    stale_targets = [
        (source, _safe_bundle_target(bundle_dir, source))
        for source in sorted(prev_sources - new_sources)
    ]
    stale_targets = [
        (source, target) for source, target in stale_targets
        if target is not None and os.path.exists(target)
    ]
    manifest_written = (
        manifest_changed
        or any(mesh_needs_write.values())
        or any(composite_needs_write.values())
        or bool(stale_targets)
        or recovery_in_progress
    )

    # The pending manifest is both the prepared commit record and a fail-closed
    # in-progress marker for UE. It is created before the first payload replace
    # and promoted atomically only after every payload write succeeds.
    if manifest_written:
        _write_json_atomic(pending_manifest_path, manifest_doc)

    for mesh in manifest.meshes:
        target = os.path.join(bundle_dir, mesh.source())
        if not mesh_needs_write[mesh.uid]:
            skipped.append(mesh.source())
            continue
        _export_collection_fbx(geo_scene, export_objects[mesh.uid], target)
        written.append(mesh.source())

    for composite in composites:
        doc = composite_docs[composite.uid]
        target = os.path.join(bundle_dir, composite.filename())
        if not composite_needs_write[composite.uid]:
            skipped.append(composite.filename())
            continue
        _write_json_atomic(target, doc)
        written.append(composite.filename())

    if manifest_written:
        _promote_pending_manifest(pending_manifest_path, manifest_path)

    deleted = []
    for source, target in stale_targets:
        os.remove(target)
        deleted.append(source)

    return {"ok": True, "validation": validation, "written": written,
            "skipped": skipped, "deleted": deleted,
            "manifest_changed": manifest_changed,
            "manifest_written": manifest_written}
