"""Write-free publication planning for the explicit dag4blend bridge.

Ordinary Composite exporters remain strict. This entry point accepts converted
DTOs and explicit mesh conversion inputs, admits them through the real writers,
and returns the existing closure publisher's immutable plan. It never stamps,
relinks, renames, materializes, stages, or publishes Blender/source resources.
"""

from __future__ import annotations

from dataclasses import replace

import bpy

from ..core.canonical import validate_resource_name
from ..core.composites import composite_json_bytes, iter_resource_references
from ..core.source_closure import ResourceKey, build_composite_source_closure
from ..core.source_inventory import scan_source_inventory
from ..core.validate import MHValidationError
from .export_closure import (
    CLOSURE_MODE_INCLUDE_ALL,
    ClosureExportPlan,
    PlannedClosurePayload,
    _resolve_texture_source,
    _resolved_output,
    _reuse_row,
    _source_material,
    _source_profile,
    _target_for,
)
from .export_composite import (
    _collection_instance_identity,
    _extract_composite,
    _node_kind_and_resource,
)
from .export_fbx import prepare_fbx_collection
from .export_material import prepare_blender_material_export
from .import_composite import _preflight, _validate_document_mapping
from .import_fbx import classify_resource_definition, parse_mesh_fbx
from .resource_markers import (
    COLLECTION_KIND_KEY,
    COLLECTION_RESOURCE_KEY,
    DEFINITION_REUSE,
    INCOMPLETE_IMPORT_KEY,
    is_managed_resource_collection,
    managed_resource_collections,
)

__all__ = ["prepare_dag4blend_publication"]


def _fail(code, subjects, message):
    raise MHValidationError(code, [str(subject) for subject in subjects], message)


def _admission_for_mesh_input(key, collection):
    """Validate prospective adoption without simulating it with live stamps."""
    if (collection is None
            or bpy.data.collections.get(collection.name) is not collection):
        _fail("MH_E_INVALID_RESOURCE_SOURCE", [key],
              "mesh conversion input must be a live Blender Collection")
    has_kind = COLLECTION_KIND_KEY in collection
    has_name = COLLECTION_RESOURCE_KEY in collection
    if has_kind != has_name:
        _fail("MH_E_INVALID_RESOURCE_SOURCE", [key, collection.name],
              "partial MH identity cannot be adopted")
    if has_kind and not is_managed_resource_collection(
            collection, "mesh", key.name):
        _fail("MH_E_RESOURCE_KIND_MISMATCH", [key, collection.name],
              "mesh conversion input carries a conflicting MH identity")
    if not has_kind and not ("type" in collection and "name" in collection):
        _fail("MH_E_INVALID_RESOURCE_SOURCE", [key, collection.name],
              "mesh adoption requires explicit dag4blend type/name identity")
    if _collection_instance_identity(collection) != ("mesh", key.name):
        _fail("MH_E_RESOURCE_KIND_MISMATCH", [key, collection.name],
              "mesh conversion input disagrees with its ResourceKey")
    if bool(collection.get(INCOMPLETE_IMPORT_KEY, False)):
        _fail("MH_E_INVALID_RESOURCE_SOURCE", [key, collection.name],
              "incomplete mesh definitions cannot be adopted or published")

    # The ordinary classifier cannot admit an unstamped occupant of its own
    # future ID. Check competing/malformed claims without temporarily stamping
    # that occupant; actual adoption belongs after successful publication.
    for other in bpy.data.collections:
        other_kind = COLLECTION_KIND_KEY in other
        other_name = COLLECTION_RESOURCE_KEY in other
        touches = (other.name == collection.name
                   or other.get(COLLECTION_RESOURCE_KEY) == key.name
                   or (other.get(COLLECTION_KIND_KEY) == "mesh" and not other_name))
        malformed = other_kind != other_name
        if other_kind and other_name:
            claimed_kind = other.get(COLLECTION_KIND_KEY)
            claimed_name = other.get(COLLECTION_RESOURCE_KEY)
            malformed = (not isinstance(claimed_kind, str)
                         or claimed_kind not in {"mesh", "actor", "composite"})
            try:
                validate_resource_name(claimed_name)
            except (TypeError, ValueError):
                malformed = True
        if malformed and touches:
            _fail("MH_E_IMPORT_TARGET_OCCUPIED", [key, other.name],
                  "malformed or partial MH claim blocks mesh adoption")
    twins = [candidate for candidate in managed_resource_collections(
        "mesh", key.name) if candidate is not collection]
    if twins:
        _fail("MH_E_AMBIGUOUS_RESOURCE_NAME",
              [key, collection.name, *(candidate.name for candidate in twins)],
              "mesh adoption would create a second managed ResourceKey claim")
    if has_kind:
        classify_resource_definition(
            "mesh", key.name, collection.name, definition_policy=DEFINITION_REUSE)


def _validate_reused_composite_bindings(collection, mesh_inputs):
    """A reused definition must remain strict after the planned mesh adoption."""
    for obj in collection.objects:
        settings = getattr(obj, "mh4blend", None)
        parent_settings = getattr(getattr(obj, "parent", None), "mh4blend", None)
        option = (settings is not None and parent_settings is not None
                  and parent_settings.kind == "random"
                  and settings.is_property_set("option_index"))
        kind, name = _node_kind_and_resource(obj, option=option)
        instance = obj.instance_collection
        if kind not in {"mesh", "composite"} or name is None or instance is None:
            continue
        if kind == "mesh" and mesh_inputs.get((kind, name)) is instance:
            continue  # This exact input is admitted by the real mesh writer.
        if not is_managed_resource_collection(instance, kind, name):
            _fail("MH_E_INVALID_RESOURCE_SOURCE", [f"{kind}:{name}", obj.name],
                  "reused composite still binds an unmanaged definition; "
                  "publication does not rewrite internal legacy placements")


def prepare_dag4blend_publication(
        documents, mesh_inputs, *, root_name, source_root, output_dir,
        definition_policy=DEFINITION_REUSE) -> ClosureExportPlan:
    """Preflight every payload and future Blender ID before the first write.

    The caller stages/publishes this plan through export_closure, stamps only
    mesh identities actually published, then materializes with those explicit
    managed overrides. Composite rows intentionally have no Blender authority:
    legacy definitions must never be stamped by the publisher's finalizer.
    """
    if definition_policy != DEFINITION_REUSE:
        _fail("MH_E_INVALID_RESOURCE_SOURCE", [definition_policy],
              "dag4blend publication refresh is implementation-pending; "
              "this entry point currently supports reuse only")
    documents = _validate_document_mapping(root_name, documents)
    payloads = {name: composite_json_bytes(document)
                for name, document in documents.items()}
    for name, document in documents.items():
        actors = tuple(iter_resource_references(document, kind="actor"))
        if actors:
            _fail("MH_E_INVALID_RESOURCE_SOURCE",
                  [f"composite:{name}", *(f"actor:{actor}" for actor in actors)],
                  "OPEN-V5-19: gameObj carrier is unresolved; actor-containing "
                  "closures cannot use the new publication entry point")
    closure = build_composite_source_closure(root_name, documents)
    reachable = {key.name for key in closure.composites_postorder}
    if set(documents) != reachable:
        _fail("MH_E_INVALID_RESOURCE_SOURCE", sorted(set(documents) - reachable),
              "publication documents must exactly match the root source closure")
    inventory = scan_source_inventory(source_root)
    output = _resolved_output(inventory.root, output_dir)
    inputs = dict(mesh_inputs)
    for key, collection in inputs.items():
        if (not isinstance(key, tuple) or len(key) != 2 or key[0] != "mesh"
                or not isinstance(key[1], str)):
            _fail("MH_E_INVALID_RESOURCE_SOURCE", [repr(key)],
                  "mesh input key must be ('mesh', canonical_name)")
        resource_key = ResourceKey("static_mesh", key[1])
        if resource_key not in closure.static_meshes:
            _fail("MH_E_INVALID_RESOURCE_SOURCE", [resource_key],
                  "mesh conversion input is outside the root source closure")
        _admission_for_mesh_input(resource_key, collection)

    profiles = [_source_profile(inventory.resolve(key))
                for key in sorted(closure.placement_profiles)]
    meshes = []
    material_inputs = {}
    material_names = set()
    material_owners = {}
    for key in sorted(closure.static_meshes):
        collection = inputs.get(("mesh", key.name))
        target, existing = _target_for(inventory, output, key, ".mesh.fbx")
        if collection is None:
            candidate = inventory.resolve(key)
            source_plan = parse_mesh_fbx(candidate.path)
            names = source_plan.material_names
            meshes.append(_reuse_row(key, candidate, candidate.read_bytes()))
        else:
            prepared = prepare_fbx_collection(
                collection, output, source_root=inventory.root,
                export_materials=False)
            if prepared.resource_name != key.name:
                _fail("MH_E_RESOURCE_KIND_MISMATCH", [key, collection.name],
                      "FBX writer identity differs from the converted token; "
                      "automatic renaming is forbidden")
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
            material_owners.setdefault(name, set()).update(closure.referrers_for(key))

    materials = []
    textures = {}
    for name in sorted(material_names):
        key = ResourceKey("material", name)
        material = material_inputs.get(name)
        if material is None:
            resource, row = _source_material(inventory.resolve(key))
        else:
            if material.library is not None:
                _fail("MH_E_INVALID_RESOURCE_SOURCE", [key, material.name],
                      "linked read-only material cannot be publication authority")
            prepared = prepare_blender_material_export(
                material, output, source_root=inventory.root)
            target, existing = _target_for(inventory, output, key, ".material")
            prepared = replace(prepared, target=target)
            resource = prepared.resource
            row = PlannedClosurePayload(
                key, target, "publish", prepared.payload,
                existing.snapshot() if existing is not None else None, prepared)
        materials.append(row)
        for token in resource.textures.values():
            texture_key = ResourceKey("texture", token)
            candidate = _resolve_texture_source(
                inventory, texture_key, sorted(material_owners[name]))
            textures[texture_key] = candidate.snapshot()

    # Mesh inputs are merely preloaded for ID planning, not falsely stamped.
    # The ordinary materializer runs its full managed-override check after the
    # caller's successful publication/adoption and uses these same datablocks.
    _paths, _mesh_decisions, composite_decisions, _actors = _preflight(
        documents, inventory.root, preloaded_resources=frozenset(inputs),
        definition_policy=DEFINITION_REUSE)
    for name, decision in composite_decisions.items():
        if decision.action == "reuse":
            actual = composite_json_bytes(_extract_composite(decision.collection))
            if actual != payloads[name]:
                _fail("MH_E_INVALID_RESOURCE_SOURCE",
                      [f"composite:{name}", decision.collection.name],
                      "reused managed composite differs from the converted DTO; "
                      "reuse cannot silently refresh its contents")
            _validate_reused_composite_bindings(decision.collection, inputs)

    composites = []
    for key in closure.composites_postorder:
        target, existing = _target_for(inventory, output, key, ".composite")
        composites.append(PlannedClosurePayload(
            key, target, "publish", payloads[key.name],
            existing.snapshot() if existing is not None else None, None))
    return ClosureExportPlan(
        mode=CLOSURE_MODE_INCLUDE_ALL,
        source_root=inventory.root,
        output_dir=output,
        closure=closure,
        payloads=tuple(profiles + materials + meshes + composites),
        validated_only=tuple(sorted(textures.items())),
        texture_dependencies=tuple(sorted(textures)),
    )
