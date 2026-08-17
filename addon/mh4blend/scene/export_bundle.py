"""Bundle exporter (B10): gathers the semantic model from the GEOMETRY and
COMPOSITS scenes and writes the bundle per the §1.1 atomicity protocol.

Write order: changed files via <name>.tmp + rename; unchanged files (by
content_hash against the previous manifest) are never touched;
export_manifest.json goes last (the commit point); deletions are computed
as sources(previous manifest) - sources(new manifest), never a directory
sweep. Validation errors abort before any file is written.

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
    build_report,
    validate_manifest,
)
from .composite_extract import _bag, extract_composites
from .mesh_extract import extract_mesh_records
from ..core.meshser import mesh_content_hash

__all__ = ["export_bundle", "FBX_EXPORT_KWARGS"]

EXPORTER_VERSION = "0.1.0"

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


def _collect_duplicate_errors(geo_scene, cmp_scene):
    errors = []
    objects = list(geo_scene.objects) + list(cmp_scene.objects)
    for uid, _owners in find_duplicate_uids(objects).items():
        errors.append(ValidationError(
            "MH_E_DUPLICATE_NODE_UID", [uid],
            "objects share one mh_uid (Ctrl+D?)"))
    meshes = [obj.data for obj in geo_scene.objects
              if obj.type == "MESH" and obj.data is not None]
    collections = (list(geo_scene.collection.children)
                   + list(cmp_scene.collection.children))
    for uid, _owners in find_duplicate_uids(meshes + collections).items():
        errors.append(ValidationError(
            "MH_E_DUPLICATE_RESOURCE_UID", [uid],
            "datablocks/collections share one mh_uid (§4.1 arbitration "
            "needed)"))
    return errors


def _assign_uids(geo_scene, cmp_scene):
    for collection in geo_scene.collection.children:
        ensure_uid(collection)
        for obj in collection.objects:
            ensure_uid(obj)
            if obj.type == "MESH" and obj.data is not None:
                ensure_uid(obj.data)
    for collection in cmp_scene.collection.children:
        ensure_uid(collection)
        for obj in collection.objects:
            ensure_uid(obj)


def _gather(geo_scene, cmp_scene, bundle_name):
    """-> (Manifest, list[Composite], {col_uid: [objects]}, scene_errors)"""
    errors = _collect_duplicate_errors(geo_scene, cmp_scene)
    if errors:
        return None, None, None, errors

    _assign_uids(geo_scene, cmp_scene)

    depsgraph = bpy.context.evaluated_depsgraph_get()
    meshes = []
    export_objects = {}
    for collection in geo_scene.collection.children:
        records = extract_mesh_records(collection, depsgraph)
        if not records:
            errors.append(ValidationError(
                "MH_E_EMPTY_RESOURCE_COLLECTION", [collection[PROP_UID]],
                f"'{collection.name}' has no mesh objects"))
            continue
        meshes.append(MeshResource(
            uid=collection[PROP_UID],
            name=collection.name,
            content_hash=mesh_content_hash(records),
            material_slots=[],  # populated by B4-mat (materials extraction)
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
    for root, _dirs, files in os.walk(bundle_dir):
        for name in files:
            if name.endswith(".tmp"):
                with contextlib.suppress(OSError):
                    os.remove(os.path.join(root, name))


def export_bundle(bundle_dir, geo_scene=None, cmp_scene=None,
                  bundle_name=None, dry_run=False):
    """Run the full export into `bundle_dir`. Returns a report dict:
    {"validation": mh.validation_report, "written": [...], "skipped": [...],
     "deleted": [...], "ok": bool}. On validation errors nothing is written.
    dry_run=True stops after validation (the Validate button).
    """
    geo_scene = geo_scene or bpy.data.scenes["GEOMETRY"]
    cmp_scene = cmp_scene or bpy.data.scenes["COMPOSITS"]
    if bundle_name is None:
        stem = os.path.splitext(os.path.basename(bpy.data.filepath))[0]
        bundle_name = stem or "untitled"

    manifest, composites, export_objects, scene_errors = _gather(
        geo_scene, cmp_scene, bundle_name)

    if manifest is not None:
        report = validate_manifest(manifest)
        rows = scene_errors + [
            ValidationError(e["code"], e["subjects"], e.get("message", ""))
            for e in report["errors"]]
    else:
        rows = scene_errors
    validation = build_report(rows)
    if validation["errors"] or dry_run:
        return {"ok": not validation["errors"], "validation": validation,
                "written": [], "skipped": [], "deleted": []}

    os.makedirs(os.path.join(bundle_dir, "meshes"), exist_ok=True)
    _cleanup_orphan_tmp(bundle_dir)
    previous = _read_previous_manifest(bundle_dir)
    prev_hashes = {}
    prev_sources = set()
    if previous:
        for entry in previous.get("resources", []):
            prev_hashes[entry["uid"]] = entry.get("content_hash")
            prev_sources.add(entry["source"])
            for lod in entry.get("lods", ()):
                prev_sources.add(lod["source"])

    written, skipped = [], []

    for mesh in manifest.meshes:
        target = os.path.join(bundle_dir, mesh.source())
        if (prev_hashes.get(mesh.uid) == mesh.content_hash
                and os.path.exists(target)):
            skipped.append(mesh.source())
            continue
        _export_collection_fbx(geo_scene, export_objects[mesh.uid], target)
        written.append(mesh.source())

    composite_hashes = {}
    for composite in composites:
        doc = composite_disk_dict(composite)
        content_hash = composite_hash(composite)
        composite_hashes[composite.uid] = content_hash
        target = os.path.join(bundle_dir, composite.filename())
        if (prev_hashes.get(composite.uid) == content_hash
                and os.path.exists(target)):
            skipped.append(composite.filename())
            continue
        _write_json_atomic(target, doc)
        written.append(composite.filename())

    manifest_doc = manifest_disk_dict(manifest, composite_hashes)
    _write_json_atomic(
        os.path.join(bundle_dir, "export_manifest.json"), manifest_doc)

    new_sources = {r["source"] for r in manifest_doc["resources"]}
    deleted = []
    for source in sorted(prev_sources - new_sources):
        path = os.path.join(bundle_dir, source)
        if os.path.exists(path):
            os.remove(path)
            deleted.append(source)

    return {"ok": True, "validation": validation, "written": written,
            "skipped": skipped, "deleted": deleted}
