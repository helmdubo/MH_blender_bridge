"""Read-only DTO planning for direct dag4blend Source Protocol publication.

The adapter supplies documents and original mesh inputs. This planner uses
the canonical writers and S4 publisher, never adoption or Blender ID planning.
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace
import os

import bpy

from ..core.composites import composite_json_bytes, iter_resource_references
from ..core.source_closure import ResourceKey, build_composite_source_closure
from ..core.source_inventory import scan_source_inventory
from ..core.validate import MHValidationError
from .export_closure import (
    CLOSURE_MODE_ROOT,
    CLOSURE_MODE_COMPOSITES,
    CLOSURE_MODE_INCLUDE_ALL,
    ClosureExportPlan,
    PlannedClosurePayload,
    _resolve_excluded_source,
    _resolve_texture_source,
    _resolved_output,
    _reuse_row,
    _source_composite,
    _source_material,
    _source_profile,
    _target_for,
)
from .export_composite import _collection_instance_identity
from .export_fbx import prepare_fbx_collection
from .export_material import (
    _active_material_export_session,
    material_export_session,
    prepare_blender_material_export,
)
from .import_composite import _validate_document_mapping
from .import_fbx import parse_mesh_fbx
from .resource_markers import (
    COLLECTION_KIND_KEY,
    COLLECTION_RESOURCE_KEY,
    INCOMPLETE_IMPORT_KEY,
)

__all__ = ["prepare_dag4blend_publication"]


_TEXTURE_SNAPSHOT_WORKERS = min(
    8, max(2, (os.cpu_count() or 2) // 2))


def _fail(code, subjects, message):
    raise MHValidationError(code, [str(subject) for subject in subjects], message)


def _validate_mesh_input(key, collection):
    """Validate this read-only input, not future adoption or competing IDs."""
    if (collection is None
            or bpy.data.collections.get(collection.name) is not collection):
        _fail("MH_E_INVALID_RESOURCE_SOURCE", [key],
              "mesh input must be a live Blender Collection")
    markers = [name for name in (COLLECTION_KIND_KEY, COLLECTION_RESOURCE_KEY)
               if name in collection]
    if markers:
        _fail("MH_E_INVALID_RESOURCE_SOURCE", [key, collection.name, *markers],
              "direct dag4blend mesh input cannot carry MH identity markers")
    if not ("type" in collection and "name" in collection):
        _fail("MH_E_INVALID_RESOURCE_SOURCE", [key, collection.name],
              "mesh input requires explicit dag4blend type/name identity")
    # This planner is the dag4blend route itself, so it is one of the two
    # callers allowed to read Dagor identity (doc 15 2.5); the generic MH
    # reader passes dagor_identity=False and refuses the same collection.
    if _collection_instance_identity(
            collection, dagor_identity=True) != ("mesh", key.name):
        _fail("MH_E_RESOURCE_KIND_MISMATCH", [key, collection.name],
              "mesh input disagrees with its ResourceKey")
    if bool(collection.get(INCOMPLETE_IMPORT_KEY, False)):
        _fail("MH_E_INVALID_RESOURCE_SOURCE", [key, collection.name],
              "incomplete mesh definitions cannot be published")


def _json_row(inventory, output, key, extension, payload, prepared=None):
    """Reuse exact canonical Source Root bytes, never scene stamps."""
    target, existing = _target_for(inventory, output, key, extension)
    if existing is not None and existing.read_bytes() == payload:
        return _reuse_row(key, existing, payload)
    if prepared is not None:
        prepared = replace(prepared, target=target)
    return PlannedClosurePayload(
        key, target, "publish", payload,
        existing.snapshot() if existing is not None else None, prepared)


def _generated_profile_row(inventory, output, key, payload):
    """Admit content-addressed p2 output without overwriting a collision."""

    if not isinstance(payload, bytes):
        _fail("MH_E_INVALID_RESOURCE_SOURCE", [key],
              "generated placement profile payload must be canonical bytes")
    target, existing = _target_for(inventory, output, key, ".placement")
    if existing is not None and existing.read_bytes() != payload:
        _fail(
            "MH_E_AMBIGUOUS_RESOURCE_NAME", [key, existing.path],
            "content-addressed dag4blend p2 profile collides with different "
            "existing source bytes")
    return _json_row(inventory, output, key, ".placement", payload)


def _snapshot_texture_candidates(candidates):
    """Hash unique immutable texture candidates without touching Blender."""

    ordered = sorted(candidates.items())
    if len(ordered) < 2 or _TEXTURE_SNAPSHOT_WORKERS < 2:
        return {key: candidate.snapshot() for key, candidate in ordered}
    with ThreadPoolExecutor(
            max_workers=min(_TEXTURE_SNAPSHOT_WORKERS, len(ordered)),
            thread_name_prefix="MHTextureHash") as executor:
        snapshots = executor.map(
            lambda row: row[1].snapshot(), ordered)
        return {
            key: snapshot
            for (key, _candidate), snapshot in zip(ordered, snapshots)
        }


def prepare_dag4blend_publication(
        documents, mesh_inputs, *, root_name, source_root, output_dir,
        mode=CLOSURE_MODE_INCLUDE_ALL,
        generated_profiles=None) -> ClosureExportPlan:
    """Prepare requested writes and validate the complete source closure.

    Root-only reads excluded composites from Source Root, as the MH adapter
    does. Excluded meshes/materials are also source authorities. GameObj tokens,
    like actor tokens, have no filesystem payload. The caller stages/publishes
    through S4, without a Blender finalizer.
    """
    if _active_material_export_session() is None:
        with material_export_session():
            return prepare_dag4blend_publication(
                documents, mesh_inputs, root_name=root_name,
                source_root=source_root, output_dir=output_dir, mode=mode,
                generated_profiles=generated_profiles)
    if mode not in {CLOSURE_MODE_ROOT, CLOSURE_MODE_COMPOSITES,
                    CLOSURE_MODE_INCLUDE_ALL}:
        raise ValueError(f"unsupported closure export mode {mode!r}")
    documents = _validate_document_mapping(root_name, documents)
    inventory = scan_source_inventory(source_root)
    _active_material_export_session().bind_inventory(inventory)
    output = _resolved_output(inventory.root, output_dir)
    resolved = {root_name: documents[root_name]}
    composite_rows = {}

    def resolve_composite(name):
        if name in resolved:
            return resolved[name]
        key = ResourceKey("composite", name)
        owners = [ResourceKey("composite", owner)
                  for owner, document in resolved.items()
                  if name in iter_resource_references(document, kind="composite")]
        if mode != CLOSURE_MODE_ROOT and name in documents:
            resource = documents[name]
        else:
            candidate = (_resolve_excluded_source(inventory, key, owners)
                         if mode == CLOSURE_MODE_ROOT else inventory.resolve(key))
            resource, composite_rows[name] = _source_composite(candidate)
        resolved[name] = resource
        return resource

    closure = build_composite_source_closure(root_name, resolve_composite)
    inputs = dict(mesh_inputs)
    for key in inputs:
        if (not isinstance(key, tuple) or len(key) != 2 or key[0] != "mesh"
                or not isinstance(key[1], str)):
            _fail("MH_E_INVALID_RESOURCE_SOURCE", [repr(key)],
                  "mesh input key must be ('mesh', canonical_name)")
        resource_key = ResourceKey("static_mesh", key[1])
        if (mode == CLOSURE_MODE_INCLUDE_ALL
                and resource_key not in closure.static_meshes):
            _fail("MH_E_INVALID_RESOURCE_SOURCE", [resource_key],
                  "mesh input is outside the root source closure")

    generated = dict(generated_profiles or {})
    generated_keys = {
        ResourceKey("placement_profile", name) for name in generated}
    unknown_generated = generated_keys.difference(closure.placement_profiles)
    if unknown_generated:
        _fail(
            "MH_E_INVALID_RESOURCE_SOURCE", sorted(unknown_generated),
            "generated placement profile is outside the composite closure")

    # Typed profiles reuse their exact sources. Inline dag4blend p2 profiles
    # are immutable canonical bytes and enter this same dependency-first batch.
    profiles = []
    for key in sorted(closure.placement_profiles):
        payload = generated.get(key.name)
        if payload is not None:
            profiles.append(_generated_profile_row(
                inventory, output, key, payload))
        else:
            candidate = (_resolve_excluded_source(
                inventory, key, closure.referrers_for(key))
                if mode != CLOSURE_MODE_INCLUDE_ALL else inventory.resolve(key))
            profiles.append(_source_profile(candidate))

    meshes = []
    material_inputs = {}
    material_names = set()
    material_owners = {}
    for key in sorted(closure.static_meshes):
        owners = closure.referrers_for(key)
        collection = inputs.get(("mesh", key.name))
        if mode != CLOSURE_MODE_INCLUDE_ALL:
            candidate = _resolve_excluded_source(inventory, key, owners)
        else:
            candidate = inventory.resolve(key, allow_missing=True)
        if mode != CLOSURE_MODE_INCLUDE_ALL or collection is None:
            if candidate is None:
                _fail("MH_E_RESOURCE_NOT_FOUND", [key, *owners],
                      "mesh dependency has no supplied input or source payload")
            source_plan = parse_mesh_fbx(candidate.path)
            names = source_plan.material_names
            meshes.append(_reuse_row(key, candidate, candidate.read_bytes()))
        else:
            _validate_mesh_input(key, collection)
            prepared = prepare_fbx_collection(
                collection, output, source_root=inventory.root,
                export_materials=False)
            if prepared.resource_name != key.name:
                _fail("MH_E_RESOURCE_KIND_MISMATCH", [key, collection.name],
                      "FBX writer identity differs from the converted token; "
                      "automatic renaming is forbidden")
            # Owner decision 2026-08-29 (closes OPEN-V5-22): a loaded mesh
            # always republishes over the existing payload through the staged
            # replace path; content-equality proof is not required.
            target, existing = _target_for(inventory, output, key, ".mesh.fbx")
            prepared = replace(prepared, target=target)
            names = tuple(material.name for material in prepared.materials)
            for material in prepared.materials:
                previous = material_inputs.get(material.name)
                if previous is not None and previous is not material:
                    _fail("MH_E_AMBIGUOUS_RESOURCE_NAME", [material.name],
                          "different Blender materials claim one material token")
                material_inputs[material.name] = material
            meshes.append(PlannedClosurePayload(
                key, target, "publish", None,
                existing.snapshot() if existing is not None else None, prepared))
        material_names.update(names)
        for name in names:
            material_owners.setdefault(name, set()).update(owners)

    materials = []
    texture_candidates = {}
    for name in sorted(material_names):
        key = ResourceKey("material", name)
        owners = sorted(material_owners[name])
        material = material_inputs.get(name)
        if mode != CLOSURE_MODE_INCLUDE_ALL:
            candidate = _resolve_excluded_source(inventory, key, owners)
            resource, row = _source_material(candidate)
        elif material is None:
            candidate = inventory.resolve(key, allow_missing=True)
            if candidate is None:
                _fail("MH_E_RESOURCE_NOT_FOUND", [key, *owners],
                      "material dependency has no supplied input or source payload")
            resource, row = _source_material(candidate)
        else:
            if material.library is not None:
                _fail("MH_E_INVALID_RESOURCE_SOURCE", [key, material.name],
                      "linked read-only material cannot be publication authority")
            prepared = prepare_blender_material_export(
                material, output, source_root=inventory.root)
            resource = prepared.resource
            row = _json_row(
                inventory, output, key, ".material", prepared.payload, prepared)
        materials.append(row)
        for token in resource.textures.values():
            texture_key = ResourceKey("texture", token)
            if texture_key in texture_candidates:
                continue
            candidate = _resolve_texture_source(inventory, texture_key, owners)
            texture_candidates[texture_key] = candidate

    textures = _snapshot_texture_candidates(texture_candidates)

    for key in closure.composites_postorder:
        if key.name not in composite_rows:
            composite_rows[key.name] = _json_row(
                inventory, output, key, ".composite",
                composite_json_bytes(resolved[key.name]))
    return ClosureExportPlan(
        mode=mode,
        source_root=inventory.root,
        output_dir=output,
        closure=closure,
        payloads=tuple(profiles + materials + meshes + [
            composite_rows[key.name] for key in closure.composites_postorder]),
        validated_only=tuple(sorted(textures.items())),
        texture_dependencies=tuple(sorted(textures)),
    )
