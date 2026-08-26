"""Transactional Blender import for Source Protocol v4 ``*.mesh.fbx``.

Stages 0/1/3/4 deliberately operate on the parsed transport model rather than
on guesses about names produced by Blender's importer.  Stage 2 is the pinned
``io_scene_fbx`` seam replaced by S7's native decoder only after its own gate.
"""

from __future__ import annotations

import contextlib
from dataclasses import dataclass, replace
import os
from pathlib import Path
import re
from typing import Iterable

import bpy

from ..core.canonical import validate_resource_name
from ..core.materials import resolve_texture_reference
from ..core.mesh_nodes import MeshImportPlan, MeshNode, build_mesh_import_plan
from ..core.validate import MHValidationError
from .export_material import apply_material_resource, read_material_file
from .resource_markers import (
    COLLECTION_KIND_KEY,
    COLLECTION_RESOURCE_KEY,
    DEFINITION_POLICIES,
    DEFINITION_REFRESH,
    DEFINITION_REUSE,
    INCOMPLETE_IMPORT_KEY,
    managed_resource_collections,
    stamp_resource_collection,
)

__all__ = [
    "FBX_IMPORT_KWARGS",
    "LOAD_MODE_FULL_LOD",
    "LOAD_MODE_LOD0",
    "LOAD_MODE_STRUCTURE_ONLY",
    "MeshImportTransaction",
    "classify_resource_definition",
    "import_mesh_fbx",
    "mesh_import_id_names",
    "mesh_import_id_names_for_mode",
    "preflight_mesh_definition",
    "parse_mesh_fbx",
    "preflight_mesh_import_plan",
]


LOAD_MODE_FULL_LOD = "full-LOD"
LOAD_MODE_LOD0 = "LOD0"
LOAD_MODE_STRUCTURE_ONLY = "structure-only"
LOAD_MODES = frozenset({
    LOAD_MODE_FULL_LOD, LOAD_MODE_LOD0, LOAD_MODE_STRUCTURE_ONLY,
})


@dataclass(frozen=True)
class ResourceDefinitionDecision:
    action: str
    collection: object | None


def _validate_load_mode(load_mode: str) -> str:
    if load_mode not in LOAD_MODES:
        raise ValueError(
            f"load_mode must be one of {sorted(LOAD_MODES)!r}, got {load_mode!r}")
    return load_mode


def _validate_definition_policy(definition_policy: str) -> str:
    if definition_policy not in DEFINITION_POLICIES:
        raise ValueError(
            "definition_policy must be 'reuse' or 'refresh', got "
            f"{definition_policy!r}")
    return definition_policy


def _collection_has_mesh_geometry(collection) -> bool:
    if any(obj.type == "MESH" and obj.data is not None
           for obj in collection.objects):
        return True
    return any(
        _collection_has_mesh_geometry(child) for child in collection.children)


def classify_resource_definition(
        kind: str, resource_name: str, target_name: str, *,
        definition_policy=DEFINITION_REUSE) -> ResourceDefinitionDecision:
    """Resolve create/reuse/refresh before the first scene mutation.

    Identity comes only from the exact managed stamps.  A datablock name is
    used solely as the collision boundary which prevents Blender's implicit
    ``.001`` repair.
    """

    _validate_definition_policy(definition_policy)
    malformed = []
    for collection in bpy.data.collections:
        has_kind = COLLECTION_KIND_KEY in collection
        has_name = COLLECTION_RESOURCE_KEY in collection
        touches_key = (
            collection.name == target_name
            or collection.get(COLLECTION_RESOURCE_KEY) == resource_name
            or (collection.get(COLLECTION_KIND_KEY) == kind and not has_name)
        )
        if has_kind != has_name and touches_key:
            malformed.append(collection.name)
        elif has_kind and has_name:
            claimed_kind = collection.get(COLLECTION_KIND_KEY)
            claimed_name = collection.get(COLLECTION_RESOURCE_KEY)
            if (not isinstance(claimed_kind, str)
                    or claimed_kind not in {"mesh", "actor", "composite"}
                    or not isinstance(claimed_name, str)):
                if touches_key:
                    malformed.append(collection.name)
            else:
                try:
                    validate_resource_name(claimed_name)
                except (TypeError, ValueError):
                    if touches_key:
                        malformed.append(collection.name)
    if malformed:
        raise MHValidationError(
            "MH_E_IMPORT_TARGET_OCCUPIED",
            [f"collection:{name}" for name in sorted(set(malformed))],
            "malformed or partial MH resource marker claim blocks definition "
            "reuse/refresh")
    candidates = managed_resource_collections(kind, resource_name)
    if len(candidates) > 1:
        raise MHValidationError(
            "MH_E_AMBIGUOUS_RESOURCE_NAME",
            [f"{kind}:{resource_name}", *(row.name for row in candidates)],
            "multiple managed Collections claim one ResourceKey")
    existing = candidates[0] if candidates else None
    occupant = bpy.data.collections.get(target_name)
    if occupant is not None and occupant is not existing:
        raise MHValidationError(
            "MH_E_IMPORT_TARGET_OCCUPIED", [f"collection:{target_name}"],
            "an unmanaged or differently stamped Collection occupies the "
            "canonical resource definition name")
    if existing is None:
        return ResourceDefinitionDecision("create", None)
    if definition_policy == DEFINITION_REFRESH:
        return ResourceDefinitionDecision("refresh", existing)
    if bool(existing.get(INCOMPLETE_IMPORT_KEY, False)):
        raise MHValidationError(
            "MH_E_IMPORT_TARGET_OCCUPIED", [f"collection:{existing.name}"],
            "reuse requires a complete managed definition; choose Refresh")
    if kind == "mesh" and not _collection_has_mesh_geometry(existing):
        raise MHValidationError(
            "MH_E_IMPORT_TARGET_OCCUPIED", [f"collection:{existing.name}"],
            "reuse rejects a mesh placeholder without geometry; choose Refresh")
    return ResourceDefinitionDecision("reuse", existing)


def mesh_plan_for_load_mode(plan: MeshImportPlan, load_mode: str) -> MeshImportPlan:
    """Return the authored subset materialized for one approved load mode."""

    _validate_load_mode(load_mode)
    if load_mode == LOAD_MODE_FULL_LOD:
        return plan
    if load_mode == LOAD_MODE_STRUCTURE_ONLY:
        return replace(plan, nodes=(), lod_levels=(), material_names=())
    by_name = {node.name: node for node in plan.nodes}
    selected = {
        node.name for node in plan.nodes
        if node.kind != "render" or node.lod_level == 0
    }
    pending = list(selected)
    while pending:
        parent = by_name[pending.pop()].parent
        if parent is not None and parent not in selected:
            selected.add(parent)
            pending.append(parent)
    nodes = []
    for node in plan.nodes:
        if node.name not in selected:
            continue
        if node.kind == "render" and node.lod_level != 0:
            node = replace(
                node, node_type="NULL", kind="group", lod_level=None,
                material_slots=(), geometry_name=None)
        nodes.append(node)
    nodes = tuple(nodes)
    materials = tuple(dict.fromkeys(
        material
        for node in nodes
        for material in node.material_slots
    ))
    return replace(plan, nodes=nodes, lod_levels=(0,), material_names=materials)


def _collection_tree(root) -> tuple:
    rows = []

    def visit(collection):
        if collection in rows:
            return
        rows.append(collection)
        for child in collection.children:
            visit(child)

    visit(root)
    return tuple(rows)


def _owned_definition_ids(root) -> tuple[tuple, tuple, tuple]:
    collections = _collection_tree(root)
    objects = tuple(dict.fromkeys(
        obj for collection in collections for obj in collection.objects))
    data = tuple(dict.fromkeys(
        obj.data for obj in objects if obj.data is not None))
    return collections, objects, data


def validate_refresh_ownership(root) -> None:
    """Reject a refresh which would delete content shared outside the root."""

    collections, objects, data = _owned_definition_ids(root)
    collection_ids = {_identity(row) for row in collections}
    object_ids = {_identity(row) for row in objects}
    conflicts = []
    for obj in objects:
        external = [
            row.name for row in obj.users_collection
            if _identity(row) not in collection_ids
        ]
        if external:
            conflicts.append(f"object:{obj.name}:external={','.join(external)}")
    for datum in data:
        internal_users = sum(
            1 for obj in objects
            if obj.data is datum and _identity(obj) in object_ids)
        if datum.users > internal_users:
            conflicts.append(f"data:{datum.name}:shared-users={datum.users}")
    for child in collections[1:]:
        external_parents = [
            parent.name for parent in bpy.data.collections
            if _identity(parent) not in collection_ids
            and parent.children.get(child.name) is child
        ]
        external_scenes = [
            scene.name for scene in bpy.data.scenes
            if scene.collection.children.get(child.name) is child
        ]
        external_instances = [
            obj.name for obj in bpy.data.objects
            if obj.instance_collection is child
        ]
        if external_parents or external_scenes or external_instances:
            conflicts.append(
                f"collection:{child.name}:external="
                f"{','.join(external_parents + external_scenes + external_instances)}")
    if conflicts:
        raise MHValidationError(
            "MH_E_IMPORT_TARGET_OCCUPIED", sorted(conflicts),
            "refresh would destroy definition content shared outside its "
            "managed root Collection")


def _temporary_id_name(data_block, phase: str) -> str:
    return f".__mh_{phase}_{_identity(data_block):x}"


def schedule_collection_refresh(
        transaction: "MeshImportTransaction", existing, replacement, *,
        kind: str, resource_name: str, target_name: str, incomplete: bool,
        renames=()) -> None:
    """Schedule the only mutation of an existing definition at commit time."""

    validate_refresh_ownership(existing)
    old_collections, old_objects, old_data = _owned_definition_ids(existing)
    old_direct_objects = tuple(existing.objects)
    old_direct_children = tuple(existing.children)
    new_direct_objects = tuple(replacement.objects)
    new_direct_children = tuple(replacement.children)
    old_names = tuple(
        (row, row.name)
        for row in (*old_collections, *old_objects, *old_data)
    )
    old_properties = dict(existing.items())

    def finalize():
        def undo_swap():
            for obj in tuple(existing.objects):
                if obj in new_direct_objects:
                    existing.objects.unlink(obj)
                    if replacement.objects.get(obj.name) is None:
                        replacement.objects.link(obj)
            for child in tuple(existing.children):
                if child in new_direct_children:
                    existing.children.unlink(child)
                    if replacement.children.get(child.name) is None:
                        replacement.children.link(child)
            for obj in old_direct_objects:
                if existing.objects.get(obj.name) is None:
                    existing.objects.link(obj)
            for child in old_direct_children:
                if existing.children.get(child.name) is None:
                    existing.children.link(child)
            for key in tuple(existing.keys()):
                del existing[key]
            for key, value in old_properties.items():
                existing[key] = value
            for data_block, _desired_name in renames:
                data_block.name = _temporary_id_name(
                    data_block, "rollback_stage")
            for data_block, old_name in old_names:
                data_block.name = old_name

        # Register before the first mutation so even an injected failure in a
        # rename/link/stamp step restores the exact old snapshot.
        transaction.add_rollback(undo_swap)
        for data_block, _name in old_names:
            data_block.name = _temporary_id_name(data_block, "old")
        for obj in old_direct_objects:
            existing.objects.unlink(obj)
        for child in old_direct_children:
            existing.children.unlink(child)
        for obj in new_direct_objects:
            replacement.objects.unlink(obj)
            existing.objects.link(obj)
        for child in new_direct_children:
            replacement.children.unlink(child)
            existing.children.link(child)
        for data_block, desired_name in renames:
            data_block.name = desired_name
            if data_block.name != desired_name:
                raise MHValidationError(
                    "MH_E_IMPORT_TARGET_OCCUPIED",
                    [f"{type(data_block).__name__}:{desired_name}"],
                    "refresh could not restore an exact canonical ID name")
        existing.name = target_name
        if existing.name != target_name:
            raise MHValidationError(
                "MH_E_IMPORT_TARGET_OCCUPIED", [f"collection:{target_name}"],
                "refresh could not preserve the canonical Collection name")
        stamp_resource_collection(
            existing, kind, resource_name, incomplete=incomplete)
        transaction.retire_on_commit(
            replacement, *old_collections[1:], *old_objects, *old_data)

    transaction.add_finalize(finalize)


FBX_IMPORT_KWARGS = dict(
    use_manual_orientation=True,
    axis_forward="X",
    axis_up="Z",
    global_scale=1.0,
    bake_space_transform=False,
    use_custom_normals=True,
    use_image_search=False,
    use_alpha_decals=False,
    decal_offset=0.0,
    use_anim=False,
    anim_offset=1.0,
    use_subsurf=False,
    use_custom_props=False,
    use_custom_props_enum_as_string=True,
    ignore_leaf_bones=False,
    force_connect_children=False,
    automatic_bone_orientation=False,
    primary_bone_axis="Y",
    secondary_bone_axis="X",
    use_prepost_rot=True,
    colors_type="SRGB",
)

_TRACKED_DATA = (
    "objects", "collections", "scenes", "meshes", "materials", "images",
    "node_groups", "actions", "curves", "armatures", "cameras", "lights",
)
_BLENDER_AUTO_SUFFIX_RE = re.compile(r"\.\d{3}$")
_BLENDER_ID_NAME_MAX_BYTES = 63


def _identity(data_block) -> int:
    return data_block.as_pointer()


def _data_snapshot() -> dict[str, frozenset[int]]:
    return {
        attr: frozenset(_identity(item) for item in getattr(bpy.data, attr))
        for attr in _TRACKED_DATA
    }


def _data_delta(snapshot, attr: str) -> list:
    before = snapshot[attr]
    return [item for item in getattr(bpy.data, attr)
            if _identity(item) not in before]


class MeshImportTransaction:
    """Own the Blender datablock delta for one mesh or Composite closure.

    A Composite importer opens one transaction around all dependency imports
    and placements.  A direct mesh import creates this transaction internally.
    No Blender undo operator is invoked here; the UI operator remains the one
    ``REGISTER, UNDO`` boundary.
    """

    def __init__(self):
        self._snapshot = None
        self._active = False
        self._rolled_back = False
        self._interaction = None
        self._rollback_actions = []
        self._finalize_actions = []
        self._retired_ids = []

    @property
    def active(self) -> bool:
        return self._active

    @property
    def snapshot(self):
        if not self._active or self._snapshot is None:
            raise RuntimeError("MeshImportTransaction is not active")
        return self._snapshot

    def __enter__(self):
        if self._active:
            raise RuntimeError("MeshImportTransaction cannot be nested")
        view_layer = bpy.context.view_layer
        active = view_layer.objects.active if view_layer is not None else None
        selected = tuple(
            obj for obj in view_layer.objects if obj.select_get()
        ) if view_layer is not None else ()
        self._interaction = (
            view_layer,
            view_layer.active_layer_collection if view_layer is not None else None,
            active,
            selected,
            getattr(active, "mode", "OBJECT"),
        )
        self._snapshot = _data_snapshot()
        self._active = True
        return self

    def rollback(self) -> None:
        if not self._active or self._rolled_back:
            return
        for action in reversed(self._rollback_actions):
            with contextlib.suppress(RuntimeError, ReferenceError):
                action()
        new_ids = []
        # Objects/collections first makes ownership explicit even though
        # batch_remove resolves dependencies for the combined set.
        for attr in _TRACKED_DATA:
            new_ids.extend(_data_delta(self._snapshot, attr))
        if new_ids:
            bpy.data.batch_remove(new_ids)
        self._restore_interaction()
        self._rolled_back = True

    def add_rollback(self, action) -> None:
        if not self._active:
            raise RuntimeError("MeshImportTransaction is not active")
        self._rollback_actions.append(action)

    def add_finalize(self, action) -> None:
        """Defer an in-place swap until every failure-prone stage succeeds."""

        if not self._active:
            raise RuntimeError("MeshImportTransaction is not active")
        self._finalize_actions.append(action)

    def retire_on_commit(self, *data_blocks) -> None:
        if not self._active:
            raise RuntimeError("MeshImportTransaction is not active")
        self._retired_ids.extend(data_blocks)

    def _commit(self) -> None:
        seen = set()
        live = []
        for data_block in self._retired_ids:
            with contextlib.suppress(ReferenceError):
                identity = _identity(data_block)
                if identity not in seen:
                    seen.add(identity)
                    live.append(data_block)
        if live:
            bpy.data.batch_remove(live)

    def _restore_interaction(self) -> None:
        if self._interaction is None:
            return
        view_layer, layer_collection, active, selected, mode = self._interaction
        if view_layer is None:
            return
        with contextlib.suppress(RuntimeError, ReferenceError):
            if layer_collection is not None:
                view_layer.active_layer_collection = layer_collection
        for obj in view_layer.objects:
            with contextlib.suppress(RuntimeError, ReferenceError):
                obj.select_set(False)
        for obj in selected:
            with contextlib.suppress(RuntimeError, ReferenceError):
                if obj.name in view_layer.objects:
                    obj.select_set(True)
        with contextlib.suppress(RuntimeError, ReferenceError):
            view_layer.objects.active = (
                active if active is not None and active.name in view_layer.objects
                else None)
        if mode != "OBJECT" and active is not None:
            with contextlib.suppress(RuntimeError, ReferenceError):
                if active.name in view_layer.objects:
                    bpy.ops.object.mode_set(mode=mode)

    def __exit__(self, exc_type, _exc, _tb):
        try:
            if exc_type is not None:
                self.rollback()
            else:
                try:
                    for action in self._finalize_actions:
                        action()
                    self._commit()
                except Exception:
                    self.rollback()
                    raise
        finally:
            self._active = False
        return False


def _element(parent, elem_id: bytes):
    return next((item for item in parent.elems if item.id == elem_id), None)


def _fbx_name(value) -> str:
    if not isinstance(value, (bytes, bytearray)):
        raise ValueError("FBX object name must be a byte string")
    return bytes(value).split(b"\x00", 1)[0].decode("utf-8")


def _fbx_type(value) -> str:
    if not isinstance(value, (bytes, bytearray)):
        return str(value)
    return bytes(value).decode("utf-8")


def _unsupported(subjects: Iterable[str], message: str):
    raise MHValidationError("MH_E_UNSUPPORTED_NODE_KIND", subjects, message)


def _resource_name_from_path(path: Path) -> str:
    suffix = ".mesh.fbx"
    if not path.name.endswith(suffix):
        raise MHValidationError(
            "MH_E_NONCANONICAL_RESOURCE_NAME", [path.name],
            "mesh resource filename must end in exact lowercase .mesh.fbx")
    name = path.name[:-len(suffix)]
    try:
        validate_resource_name(name)
    except (TypeError, ValueError) as exc:
        raise MHValidationError(
            "MH_E_NONCANONICAL_RESOURCE_NAME", [name], str(exc)) from exc
    return name


def parse_mesh_fbx(filepath) -> MeshImportPlan:
    """Stage 0 parse using Blender's FBX tree reader, without scene mutation."""

    path = Path(bpy.path.abspath(os.fspath(filepath))).resolve(strict=True)
    resource_name = _resource_name_from_path(path)
    from io_scene_fbx import parse_fbx

    root, version = parse_fbx.parse(str(path))
    if version < 7100:
        _unsupported([path.name], f"FBX version {version} is older than 7100")
    objects = _element(root, b"Objects")
    connections = _element(root, b"Connections")
    if objects is None or connections is None:
        _unsupported([path.name], "FBX must contain Objects and Connections")

    table = {}
    models = []
    geometries = {}
    materials = {}
    allowed_other = {b"NodeAttribute", b"Texture", b"Video"}
    forbidden = []
    for item in objects.elems:
        if not item.props:
            _unsupported([path.name], "FBX Object has no numeric identity")
        identity = item.props[0]
        if identity in table:
            _unsupported([path.name], "FBX contains duplicate object identities")
        table[identity] = item
        if item.id == b"Model":
            if len(item.props) < 3:
                _unsupported([path.name], "FBX Model is missing name or type")
            model_type = _fbx_type(item.props[2])
            if model_type not in {"Mesh", "Null"}:
                forbidden.append(f"Model:{model_type}")
            models.append(item)
        elif item.id == b"Geometry":
            geometry_type = _fbx_type(item.props[-1]) if item.props else ""
            if geometry_type != "Mesh":
                forbidden.append(f"Geometry:{geometry_type}")
            geometries[identity] = item
        elif item.id == b"Material":
            materials[identity] = item
        elif item.id in allowed_other:
            if item.id == b"NodeAttribute" and len(item.props) >= 3:
                attribute_type = _fbx_type(item.props[2])
                if attribute_type not in {"Null"}:
                    forbidden.append(f"NodeAttribute:{attribute_type}")
        else:
            forbidden.append(_fbx_type(item.id))
    if forbidden:
        _unsupported(
            sorted(set(forbidden)),
            "mesh FBX contains constructs outside the static-mesh dialect: "
            + ", ".join(sorted(set(forbidden))))

    model_ids = {item.props[0] for item in models}
    model_names = {item.props[0]: _fbx_name(item.props[1]) for item in models}
    parent_ids: dict[int, int] = {}
    geometry_by_model: dict[int, list[int]] = {}
    materials_by_model: dict[int, list[int]] = {}
    for link in connections.elems:
        if (link.id != b"C" or len(link.props) < 3
                or link.props[0] != b"OO"):
            continue
        child, parent = link.props[1], link.props[2]
        if child in model_ids:
            if parent in model_ids:
                if child in parent_ids:
                    _unsupported(
                        [model_names[child]], "FBX Model has multiple parents")
                parent_ids[child] = parent
            elif parent not in {0, None}:
                _unsupported(
                    [model_names[child]],
                    "FBX Model is parented outside the Model graph")
        if child in geometries and parent in model_ids:
            geometry_by_model.setdefault(parent, []).append(child)
        if child in materials and parent in model_ids:
            materials_by_model.setdefault(parent, []).append(child)

    parsed_nodes = []
    used_geometries = set()
    for model in models:
        identity = model.props[0]
        name = model_names[identity]
        model_type = _fbx_type(model.props[2])
        geometry_ids = geometry_by_model.get(identity, [])
        if model_type == "Mesh" and len(geometry_ids) != 1:
            _unsupported(
                [name], "each FBX Mesh Model must own exactly one Mesh Geometry")
        if model_type == "Null" and geometry_ids:
            _unsupported([name], "FBX Null Model cannot own Mesh Geometry")
        if geometry_ids and geometry_ids[0] in used_geometries:
            _unsupported([name], "shared FBX Geometry is outside the v4 dialect")
        used_geometries.update(geometry_ids)

        slot_names = []
        for material_id in materials_by_model.get(identity, []):
            material = materials[material_id]
            if len(material.props) < 2:
                _unsupported([name], "FBX Material is missing its name")
            slot_names.append(_fbx_name(material.props[1]))
        geometry_name = None
        if geometry_ids:
            geometry = geometries[geometry_ids[0]]
            if len(geometry.props) < 2:
                _unsupported([name], "FBX Geometry is missing its name")
            geometry_name = _fbx_name(geometry.props[1])
        parsed_nodes.append(MeshNode(
            name=name,
            node_type="MESH" if model_type == "Mesh" else "NULL",
            parent=model_names.get(parent_ids.get(identity)),
            material_slots=tuple(slot_names),
            geometry_name=geometry_name,
        ))

    unowned = sorted(
        _fbx_name(geometry.props[1]) for identity, geometry in geometries.items()
        if identity not in used_geometries)
    if unowned:
        _unsupported(unowned, "FBX contains Mesh Geometry without a Model node")
    return build_mesh_import_plan(resource_name, parsed_nodes)


def _staging_name(resource_name: str) -> str:
    return f".__mh_mesh_import__{resource_name}"


def mesh_import_id_names(plan: MeshImportPlan) -> dict[str, frozenset[str]]:
    """Return the exact Blender ID namespaces one mesh plan will create."""

    collection_names = {
        plan.target_collection_name,
        _staging_name(plan.resource_name),
    }
    if plan.uses_lod_collections:
        collection_names.update(
            f"{plan.resource_name}.lod{level:02d}"
            for level in plan.lod_levels)
    return {
        "collections": frozenset(collection_names),
        "objects": frozenset(node.name for node in plan.nodes),
        "meshes": frozenset(
            node.geometry_name for node in plan.nodes
            if node.geometry_name is not None),
    }


def mesh_import_id_names_for_mode(
        plan: MeshImportPlan, load_mode: str) -> dict[str, frozenset[str]]:
    effective = mesh_plan_for_load_mode(plan, load_mode)
    if load_mode == LOAD_MODE_STRUCTURE_ONLY:
        return {
            "collections": frozenset({effective.target_collection_name}),
            "objects": frozenset(),
            "meshes": frozenset(),
        }
    return mesh_import_id_names(effective)


def _validate_blender_id_name(name: str, subject: str) -> None:
    """Reject names Blender would silently truncate in an ID namespace."""
    if (not isinstance(name, str)
            or len(name.encode("utf-8")) > _BLENDER_ID_NAME_MAX_BYTES):
        raise MHValidationError(
            "MH_E_IMPORT_TARGET_OCCUPIED", [f"{subject}:{name}"],
            "cannot preserve exact Blender ID name (maximum is 63 UTF-8 "
            "bytes); import repair or truncation is forbidden")


def preflight_mesh_import_plan(plan: MeshImportPlan, source_root: Path) -> None:
    """Validate every exact Blender ID before any datablock mutation."""
    id_names = mesh_import_id_names(plan)
    for name in id_names["collections"]:
        _validate_blender_id_name(name, "collection")
    for name in id_names["objects"]:
        _validate_blender_id_name(name, "object")
    for name in id_names["meshes"]:
        _validate_blender_id_name(name, "mesh")
    for name in plan.material_names:
        _validate_blender_id_name(name, "material")
        if bpy.data.materials.get(name) is not None:
            continue
        matches = _material_candidates(source_root, name)
        if not matches:
            raise MHValidationError(
                "MH_E_UNRESOLVED_MATERIAL_REFERENCE", [name],
                f"no '{name}.material' exists in Project Source Root")
        if len(matches) > 1:
            raise MHValidationError(
                "MH_E_AMBIGUOUS_RESOURCE_NAME", [name, *map(str, matches)],
                "multiple material resources share this logical name")
        resource = read_material_file(matches[0])
        for token in resource.textures.values():
            texture_path = resolve_texture_reference(source_root, token)
            _validate_blender_id_name(texture_path.name, "image")


def _preflight_scene(
        plan: MeshImportPlan, decision: ResourceDefinitionDecision, *,
        load_mode: str) -> None:
    if decision.action == "reuse":
        return
    conflicts = []
    id_names = mesh_import_id_names_for_mode(plan, load_mode)
    if load_mode != LOAD_MODE_STRUCTURE_ONLY:
        collections = set(id_names["collections"])
        if decision.action == "refresh":
            collections.discard(_staging_name(plan.resource_name))
        id_names = {
            "collections": frozenset(collections),
            # The black-box decoder creates the complete transport briefly,
            # even though LOD0 discards higher render geometry afterwards.
            "objects": frozenset(node.name for node in plan.nodes),
            "meshes": frozenset(
                node.geometry_name for node in plan.nodes
                if node.geometry_name is not None),
        }
    allowed_collections = set()
    allowed_objects = set()
    allowed_meshes = set()
    if decision.action == "refresh":
        validate_refresh_ownership(decision.collection)
        collections, objects, data = _owned_definition_ids(decision.collection)
        allowed_collections = {_identity(row) for row in collections}
        allowed_objects = {_identity(row) for row in objects}
        allowed_meshes = {_identity(row) for row in data}
    conflicts.extend(
        f"collection:{name}" for name in sorted(id_names["collections"])
        if (bpy.data.collections.get(name) is not None
            and _identity(bpy.data.collections[name]) not in allowed_collections))
    conflicts.extend(
        f"object:{name}" for name in sorted(id_names["objects"])
        if (bpy.data.objects.get(name) is not None
            and _identity(bpy.data.objects[name]) not in allowed_objects))
    conflicts.extend(
        f"mesh:{name}" for name in sorted(id_names["meshes"])
        if (bpy.data.meshes.get(name) is not None
            and _identity(bpy.data.meshes[name]) not in allowed_meshes))
    if conflicts:
        raise MHValidationError(
            "MH_E_IMPORT_TARGET_OCCUPIED", conflicts,
            "mesh import target is occupied: " + ", ".join(conflicts))


def preflight_mesh_definition(
        plan: MeshImportPlan, source_root: Path, *,
        load_mode=LOAD_MODE_FULL_LOD,
        definition_policy=DEFINITION_REUSE) -> ResourceDefinitionDecision:
    """Build the immutable action for one ResourceKey without mutation."""

    _validate_load_mode(load_mode)
    _validate_definition_policy(definition_policy)
    decision = classify_resource_definition(
        "mesh", plan.resource_name, plan.target_collection_name,
        definition_policy=definition_policy)
    if decision.action != "reuse":
        preflight_mesh_import_plan(
            mesh_plan_for_load_mode(plan, load_mode), source_root)
    _preflight_scene(plan, decision, load_mode=load_mode)
    return decision


def _material_candidates(root: Path, logical_name: str) -> list[Path]:
    result = []
    for path in root.rglob("*"):
        if not path.is_file() or path.suffix.lower() != ".material":
            continue
        if path.stem.casefold() != logical_name.casefold():
            continue
        if path.suffix != ".material" or path.stem != logical_name:
            raise MHValidationError(
                "MH_E_NONCANONICAL_RESOURCE_NAME", [str(path)],
                "material filename must match the exact lowercase logical name")
        result.append(path.resolve(strict=False))
    return sorted(result, key=lambda item: str(item).replace("\\", "/"))


def _stage_materials(plan: MeshImportPlan, source_root: Path):
    resolved = {}
    reused = []
    created = []
    for name in plan.material_names:
        material = bpy.data.materials.get(name)
        if material is not None:
            resolved[name] = material
            reused.append(name)
            continue
        matches = _material_candidates(source_root, name)
        if not matches:
            raise MHValidationError(
                "MH_E_UNRESOLVED_MATERIAL_REFERENCE", [name],
                f"no '{name}.material' exists in Project Source Root")
        if len(matches) > 1:
            raise MHValidationError(
                "MH_E_AMBIGUOUS_RESOURCE_NAME", [name, *map(str, matches)],
                "multiple material resources share this logical name")
        resource = read_material_file(matches[0])
        material = bpy.data.materials.new(name)
        apply_material_resource(material, resource, source_root=source_root)
        resolved[name] = material
        created.append(name)
    return resolved, reused, created


class _ImportReportSink:
    def __init__(self):
        self.rows = []

    def report(self, levels, message):
        self.rows.append((set(levels), str(message)))


def _find_layer_collection(layer_collection, target):
    if layer_collection.collection == target:
        return layer_collection
    for child in layer_collection.children:
        found = _find_layer_collection(child, target)
        if found is not None:
            return found
    return None


def _stage_geometry(filepath: Path, staging):
    from io_scene_fbx import import_fbx

    view_layer = bpy.context.view_layer
    previous = view_layer.active_layer_collection
    layer_collection = _find_layer_collection(view_layer.layer_collection, staging)
    if layer_collection is None:
        raise RuntimeError("staging collection is absent from the active view layer")
    sink = _ImportReportSink()
    try:
        view_layer.active_layer_collection = layer_collection
        result = import_fbx.load(
            sink, bpy.context, filepath=str(filepath), **FBX_IMPORT_KWARGS)
    finally:
        with contextlib.suppress(RuntimeError, ReferenceError):
            view_layer.active_layer_collection = previous
    if result != {"FINISHED"}:
        detail = "; ".join(message for _levels, message in sink.rows)
        raise RuntimeError(f"io_scene_fbx import failed: {detail or result}")


def _bind_parsed_nodes(
        plan: MeshImportPlan,
        imported_objects: list,
        materials: dict[str, object],
        *, allow_temporary_names=False, material_node_names=None,
) -> tuple[dict[str, object], tuple]:
    expected = {node.name for node in plan.nodes}
    if not allow_temporary_names:
        by_name = {obj.name: obj for obj in imported_objects}
        if set(by_name) != expected or len(by_name) != len(imported_objects):
            missing = sorted(expected - set(by_name))
            extra = sorted(set(by_name) - expected)
            raise RuntimeError(
                "FBX importer did not preserve parsed Model names literally; "
                f"missing={missing}, extra={extra}")
    else:
        def compatible(actual, desired):
            if actual == desired:
                return True
            match = _BLENDER_AUTO_SUFFIX_RE.search(actual)
            return match is not None and desired.startswith(actual[:-4])

        candidates = {}
        for obj in imported_objects:
            rows = []
            for node in plan.nodes:
                expected_type = "MESH" if node.node_type == "MESH" else "EMPTY"
                if obj.type != expected_type or not compatible(obj.name, node.name):
                    continue
                if node.node_type == "MESH" and (
                        obj.data is None
                        or not compatible(obj.data.name, node.geometry_name)):
                    continue
                rows.append(node.name)
            candidates[_identity(obj)] = rows
        solutions = []

        def solve(remaining, used, mapping):
            if len(solutions) > 1:
                return
            if not remaining:
                solutions.append(dict(mapping))
                return
            obj = min(
                remaining,
                key=lambda row: len([
                    name for name in candidates[_identity(row)]
                    if name not in used]),
            )
            available = [
                name for name in candidates[_identity(obj)] if name not in used]
            for name in available:
                solve(
                    [row for row in remaining if row is not obj],
                    used | {name},
                    {**mapping, name: obj},
                )

        solve(list(imported_objects), set(), {})
        if len(solutions) != 1 or set(solutions[0]) != expected:
            raise MHValidationError(
                "MH_E_IMPORT_TARGET_OCCUPIED",
                sorted(obj.name for obj in imported_objects),
                "refresh staging could not recover one unambiguous FBX Model "
                "identity after Blender assigned temporary ID suffixes")
        by_name = solutions[0]

    renames = []
    for node in plan.nodes:
        obj = by_name[node.name]
        expected_type = "MESH" if node.node_type == "MESH" else "EMPTY"
        if obj.type != expected_type:
            raise RuntimeError(
                f"parsed node '{node.name}' expected {expected_type}, got {obj.type}")
        if node.node_type != "MESH":
            continue
        geometry_name_ok = (
            obj.data is not None
            and (obj.data.name == node.geometry_name
                 or (allow_temporary_names
                     and _BLENDER_AUTO_SUFFIX_RE.search(obj.data.name)
                     and node.geometry_name.startswith(obj.data.name[:-4])))
        )
        if not geometry_name_ok:
            actual = None if obj.data is None else obj.data.name
            raise RuntimeError(
                f"FBX importer did not preserve Geometry name for '{node.name}': "
                f"expected '{node.geometry_name}', got '{actual}'")
        if material_node_names is None or node.name in material_node_names:
            obj.data.materials.clear()
            for slot_name in node.material_slots:
                obj.data.materials.append(materials[slot_name])
            if [slot.material.name for slot in obj.material_slots] != list(
                    node.material_slots):
                raise RuntimeError(
                    f"material slot binding diverged for parsed node '{node.name}'")
        if allow_temporary_names:
            renames.append((obj.data, node.geometry_name))
    if allow_temporary_names:
        for node in plan.nodes:
            obj = by_name[node.name]
            renames.append((obj, node.name))
        for data_block, _desired_name in renames:
            data_block.name = _temporary_id_name(data_block, "stage")
    return by_name, tuple(renames)


def _remove_placeholder_data(stage2_snapshot) -> None:
    placeholders = (
        _data_delta(stage2_snapshot, "materials")
        + _data_delta(stage2_snapshot, "images")
    )
    if placeholders:
        bpy.data.batch_remove(placeholders)


def _stage_restructure(
        plan: MeshImportPlan, staging, by_name, *, temporary=False):
    scene_root = bpy.context.scene.collection
    target = bpy.data.collections.new(
        ".__mh_refresh_definition" if temporary
        else plan.target_collection_name)
    scene_root.children.link(target)
    destinations = {}
    renames = []
    if plan.uses_lod_collections:
        for level in plan.lod_levels:
            desired_name = f"{plan.resource_name}.lod{level:02d}"
            child = bpy.data.collections.new(
                ".__mh_refresh_lod" if temporary else desired_name)
            target.children.link(child)
            destinations[level] = child
            if temporary:
                renames.append((child, desired_name))

    for node in plan.nodes:
        obj = by_name[node.name]
        if not plan.uses_lod_collections:
            destination = target
        elif node.kind == "render":
            destination = destinations[node.lod_level]
        elif node.kind in {"collision", "socket"}:
            destination = destinations[0]
        else:
            destination = target
        if obj.name not in destination.objects:
            destination.objects.link(obj)
        if obj.name in staging.objects:
            staging.objects.unlink(obj)
    bpy.data.collections.remove(staging)
    return target, tuple(renames)


def _resolved_source_root(source_root) -> Path:
    if not isinstance(source_root, (str, os.PathLike)) or not str(source_root).strip():
        raise ValueError("Configure Project Source Root in the MH addon preferences")
    root = Path(bpy.path.abspath(os.fspath(source_root))).resolve(strict=False)
    if not root.is_dir():
        raise ValueError(f"Project Source Root does not exist: {root}")
    return root


def _inside(root: Path, path: Path) -> bool:
    try:
        return os.path.commonpath([
            os.path.normcase(str(root)), os.path.normcase(str(path)),
        ]) == os.path.normcase(str(root))
    except ValueError:
        return False


def _execute_import(
        filepath: Path, source_root: Path, transaction, *,
        load_mode, definition_policy, prepared=None):
    # Stage 0: full transport parsing is mandatory in every load mode.
    plan = parse_mesh_fbx(filepath)
    effective = mesh_plan_for_load_mode(plan, load_mode)
    decision = prepared or preflight_mesh_definition(
        plan, source_root, load_mode=load_mode,
        definition_policy=definition_policy)

    if decision.action == "reuse":
        return {
            "ok": True,
            "filepath": str(filepath),
            "resource_name": plan.resource_name,
            "collection_name": decision.collection.name,
            "collection": decision.collection,
            "objects_imported": 0,
            "lod_levels": list(plan.lod_levels),
            "materials": [],
            "materials_reused": [],
            "materials_created": [],
            "warnings": [],
            "decoder": "reused-managed-definition",
            "load_mode": load_mode,
            "definition_action": "reuse",
        }

    incomplete = load_mode != LOAD_MODE_FULL_LOD
    temporary = decision.action == "refresh"
    if load_mode == LOAD_MODE_STRUCTURE_ONLY:
        collection = bpy.data.collections.new(
            ".__mh_refresh_definition" if temporary
            else plan.target_collection_name)
        bpy.context.scene.collection.children.link(collection)
        if temporary:
            schedule_collection_refresh(
                transaction, decision.collection, collection,
                kind="mesh", resource_name=plan.resource_name,
                target_name=plan.target_collection_name,
                incomplete=True)
            result_collection = decision.collection
        else:
            stamp_resource_collection(
                collection, "mesh", plan.resource_name, incomplete=True)
            result_collection = collection
        return {
            "ok": True,
            "filepath": str(filepath),
            "resource_name": plan.resource_name,
            "collection_name": result_collection.name,
            "collection": result_collection,
            "objects_imported": 0,
            "lod_levels": [],
            "materials": [],
            "materials_reused": [],
            "materials_created": [],
            "warnings": [],
            "decoder": "structure-only",
            "load_mode": load_mode,
            "definition_action": decision.action,
        }

    # Stage 1: canonical materials exist before the black-box decoder runs.
    images_before_materials = {
        _identity(image) for image in bpy.data.images
    }
    materials, reused, created = _stage_materials(effective, source_root)
    renamed_images = sorted(
        image.name for image in bpy.data.images
        if _identity(image) not in images_before_materials
        and _BLENDER_AUTO_SUFFIX_RE.search(image.name))
    if renamed_images:
        raise MHValidationError(
            "MH_E_IMPORT_TARGET_OCCUPIED", renamed_images,
            "material dependency images would survive with Blender .001 names")

    # Stage 2: isolate all importer output in a staging collection.
    staging = bpy.data.collections.new(
        ".__mh_refresh_fbx" if temporary
        else _staging_name(plan.resource_name))
    bpy.context.scene.collection.children.link(staging)
    stage2_snapshot = _data_snapshot()
    _stage_geometry(filepath, staging)
    imported_objects = _data_delta(stage2_snapshot, "objects")

    # Stage 3: bind names and ordered slots from parse truth, then discard
    # the black-box importer's material/image placeholders.
    effective_names = {node.name for node in effective.nodes}
    by_name, _temporary_renames = _bind_parsed_nodes(
        plan, imported_objects, materials,
        allow_temporary_names=temporary,
        material_node_names=effective_names)
    _remove_placeholder_data(stage2_snapshot)

    # LOD0 still decodes the full transport so malformed higher LOD cannot
    # hide, then discards geometry outside the approved authored subset.
    effective_by_name = {node.name: node for node in effective.nodes}
    discard = []
    for name, obj in tuple(by_name.items()):
        wanted = effective_by_name.get(name)
        if wanted is None:
            if obj.data is not None:
                discard.append(obj.data)
            discard.append(obj)
            del by_name[name]
        elif wanted.node_type == "NULL" and obj.type == "MESH":
            datum = obj.data
            obj.data = None
            if datum is not None:
                discard.append(datum)
    if discard:
        bpy.data.batch_remove(discard)

    # Stage 4: restore dag4blend-compatible authoring collections.
    collection, collection_renames = _stage_restructure(
        effective, staging, by_name, temporary=temporary)
    if temporary:
        content_renames = []
        for node in effective.nodes:
            obj = by_name[node.name]
            content_renames.append((obj, node.name))
            if node.geometry_name is not None:
                content_renames.append((obj.data, node.geometry_name))
        schedule_collection_refresh(
            transaction, decision.collection, collection,
            kind="mesh", resource_name=plan.resource_name,
            target_name=plan.target_collection_name,
            incomplete=incomplete,
            renames=(*collection_renames, *content_renames))
        result_collection = decision.collection
    else:
        stamp_resource_collection(
            collection, "mesh", plan.resource_name, incomplete=incomplete)
        result_collection = collection
    return {
        "ok": True,
        "filepath": str(filepath),
        "resource_name": plan.resource_name,
        "collection_name": result_collection.name,
        "collection": result_collection,
        "objects_imported": len(by_name),
        "lod_levels": list(effective.lod_levels),
        "materials": list(effective.material_names),
        "materials_reused": reused,
        "materials_created": created,
        "warnings": [],
        "decoder": "io_scene_fbx",
        "load_mode": load_mode,
        "definition_action": decision.action,
    }


def import_mesh_fbx(
        filepath, *, source_root, transaction=None,
        load_mode=LOAD_MODE_FULL_LOD,
        definition_policy=DEFINITION_REUSE,
        _prepared=None) -> dict:
    """Import one ``*.mesh.fbx`` as a complete transactional working copy."""

    path = Path(bpy.path.abspath(os.fspath(filepath))).resolve(strict=True)
    root = _resolved_source_root(source_root)
    _validate_load_mode(load_mode)
    _validate_definition_policy(definition_policy)
    if not path.is_file():
        raise ValueError(f"Mesh FBX source is not a file: {path}")
    if not _inside(root, path):
        raise MHValidationError(
            "MH_E_INVALID_RESOURCE_SOURCE", [str(path)],
            "mesh FBX source must be inside Project Source Root")
    if transaction is None:
        with MeshImportTransaction() as owned:
            return _execute_import(
                path, root, owned, load_mode=load_mode,
                definition_policy=definition_policy, prepared=_prepared)
    if not isinstance(transaction, MeshImportTransaction) or not transaction.active:
        raise ValueError("transaction must be an active MeshImportTransaction")
    return _execute_import(
        path, root, transaction, load_mode=load_mode,
        definition_policy=definition_policy, prepared=_prepared)
