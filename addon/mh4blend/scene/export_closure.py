"""Preflight, staging and ordered publication for v5 closure exports.

The planner is intentionally separate from filesystem publication.  It binds
the immutable all-options graph to either exact managed Blender authority or
one existing physical source candidate, validates the complete requested
closure, and returns a deterministic plan.  Staging and replacement consume
that plan only after every member has been admitted.

Blender is an external publisher and deliberately emits no UE watcher token.
Textures are preflight-only dependencies and never enter the publish order.
Resolution state never enters this API.
"""

from __future__ import annotations

from dataclasses import dataclass, replace
import hashlib
import os
from pathlib import Path
import stat
import tempfile
from typing import Any

import bpy

from ..core.composites import (
    composite_json_bytes,
    iter_resource_references,
    read_composite_file,
)
from ..core.batch_publish import (
    BatchPartialPublishError,
    BatchPublishItem,
    publish_ordered_batch,
)
from ..core.materials import (
    MaterialValueError,
    material_json_bytes,
    parse_material,
    resolve_texture_reference,
)
from ..core.placements import placement_json_bytes, read_placement_file
from ..core.source_closure import (
    CompositeSourceClosure,
    ResourceKey,
    build_composite_source_closure,
)
from ..core.source_inventory import (
    SourceCandidate,
    SourceInventory,
    SourceSnapshot,
    scan_source_inventory,
)
from ..core.validate import MHValidationError
from .export_composite import (
    _extract_composite,
    _node_kind_and_resource,
)
from .export_fbx import PreparedFBXExport, prepare_fbx_collection
from .export_material import (
    PreparedMaterialExport,
    _extract_resource as _extract_material_resource,
    prepare_blender_material_export,
)
from .import_fbx import parse_mesh_fbx
from .resource_markers import (
    COLLECTION_KIND_KEY,
    COLLECTION_RESOURCE_KEY,
    is_managed_resource_collection,
    managed_resource_collections,
    stamp_resource_collection,
)
from ..ui.composite_authoring import sync_typed_mirror

__all__ = [
    "CLOSURE_MODE_ROOT",
    "CLOSURE_MODE_COMPOSITES",
    "CLOSURE_MODE_INCLUDE_ALL",
    "ClosureExportPlan",
    "PlannedClosurePayload",
    "StagedClosurePayload",
    "export_composite_closure_collection",
    "prepare_composite_closure_export",
    "publish_composite_closure_export",
    "revalidate_composite_closure_export",
    "stage_composite_closure_export",
]


CLOSURE_MODE_ROOT = "root_only"
CLOSURE_MODE_COMPOSITES = "composite_closure"
CLOSURE_MODE_INCLUDE_ALL = "include_all"
_CLOSURE_MODES = frozenset({
    CLOSURE_MODE_ROOT,
    CLOSURE_MODE_COMPOSITES,
    CLOSURE_MODE_INCLUDE_ALL,
})


@dataclass(frozen=True)
class PlannedClosurePayload:
    """One preflighted member; ``publish`` and ``reuse`` are exhaustive."""

    key: ResourceKey
    target: Path
    action: str
    payload: bytes | None
    source_snapshot: SourceSnapshot | None
    prepared: Any = None

    def __post_init__(self) -> None:
        if self.action not in {"publish", "reuse"}:
            raise ValueError("closure payload action must be publish or reuse")
        if self.action == "reuse":
            if self.payload is None or self.source_snapshot is None:
                raise ValueError("reuse payload requires exact bytes and snapshot")
            if self.prepared is not None:
                raise ValueError("reuse payload cannot carry Blender authority")


@dataclass(frozen=True)
class StagedClosurePayload:
    """One exact read-back outside Project Source Root authority."""

    planned: PlannedClosurePayload
    staged_path: Path
    physical_path: Path
    payload: bytes


@dataclass(frozen=True)
class ClosureExportPlan:
    """Complete write-free result in protocol publication order."""

    mode: str
    source_root: Path
    output_dir: Path
    closure: CompositeSourceClosure
    payloads: tuple[PlannedClosurePayload, ...]
    validated_only: tuple[tuple[ResourceKey, SourceSnapshot], ...]
    texture_dependencies: tuple[ResourceKey, ...]

    @property
    def to_publish(self) -> tuple[PlannedClosurePayload, ...]:
        return tuple(row for row in self.payloads if row.action == "publish")

    @property
    def reused(self) -> tuple[PlannedClosurePayload, ...]:
        return tuple(row for row in self.payloads if row.action == "reuse")

    def row_for(self, key: ResourceKey) -> PlannedClosurePayload:
        for row in self.payloads:
            if row.key == key:
                return row
        raise KeyError(key)

    @property
    def full_closure_keys(self) -> tuple[ResourceKey, ...]:
        """All admitted ResourceKeys, including preflight-only textures."""

        return tuple(dict.fromkeys((
            *(row.key for row in self.payloads),
            *self.texture_dependencies,
        )))


def _raise(code: str, subjects, message: str) -> None:
    raise MHValidationError(code, subjects, message)


_INCLUDE_ALL_COMMAND = "Export Composite Include All Stuff"
_TEXTURE_FIX_COMMAND = (
    "Copy All Textures to Project, then Remap All Texture Paths")


def _owner_subjects(owners) -> list[str]:
    return [str(owner) for owner in owners]


def _resolve_excluded_source(
        inventory: SourceInventory, key: ResourceKey, owners) -> SourceCandidate:
    try:
        candidate = inventory.resolve(key)
    except MHValidationError as exc:
        _raise(
            exc.code,
            [str(key), *_owner_subjects(owners), _INCLUDE_ALL_COMMAND,
             *exc.subjects],
            f"excluded dependency is not a unique canonical managed source; "
            f"run {_INCLUDE_ALL_COMMAND}",
        )
    assert candidate is not None
    return candidate


def _resolve_texture_source(
        inventory: SourceInventory, key: ResourceKey, owners) -> SourceCandidate:
    try:
        path = resolve_texture_reference(inventory.root, key.name)
    except MaterialValueError as exc:
        _raise(
            exc.code,
            [str(key), *_owner_subjects(owners), _TEXTURE_FIX_COMMAND,
             exc.path],
            f"{exc.message}; run {_TEXTURE_FIX_COMMAND}",
        )
    candidate = inventory.resolve(key)
    assert candidate is not None
    if candidate.path != path.resolve(strict=True):
        _raise(
            "MH_E_AMBIGUOUS_RESOURCE_NAME",
            [str(key), path, candidate.path, *_owner_subjects(owners),
             _INCLUDE_ALL_COMMAND],
            "texture resolvers disagreed on physical identity",
        )
    return candidate


def _resolved_output(root: Path, value) -> Path:
    if not isinstance(value, (str, os.PathLike)) or not str(value).strip():
        raise ValueError("Choose a composite output folder")
    output = Path(bpy.path.abspath(os.fspath(value))).resolve(strict=False)
    if not output.is_dir():
        raise ValueError(
            "Closure output folder must exist before write-free preflight")
    try:
        inside = os.path.commonpath([
            os.path.normcase(str(root)), os.path.normcase(str(output)),
        ]) == os.path.normcase(str(root))
    except ValueError:
        inside = False
    if not inside:
        raise ValueError(
            "Closure output folder must be inside Project Source Root")
    return output


def _target_for(
        inventory: SourceInventory, output: Path, key: ResourceKey,
        extension: str) -> tuple[Path, SourceCandidate | None]:
    existing = inventory.resolve(key, allow_missing=True)
    if existing is not None:
        return existing.path, existing
    target = output / f"{key.name}{extension}"
    if os.path.lexists(target):
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE", [key, target],
            "new closure target exists but was not admitted as a regular "
            "canonical source candidate")
    return target, None


def _snapshot_matches(snapshot: SourceSnapshot, payload: bytes) -> bool:
    return (
        snapshot.size == len(payload)
        and snapshot.sha256 == hashlib.sha256(payload).hexdigest()
    )


def _exact_source_payload(
        candidate: SourceCandidate, canonical: bytes, *, grammar_code: str
) -> bytes:
    raw = candidate.read_bytes()
    if raw != canonical:
        _raise(
            grammar_code, [candidate.key, candidate.path],
            "existing unloaded source must already contain exact canonical "
            "bytes; closure export never normalizes or rewrites reused source")
    return raw


def _reuse_row(
        key: ResourceKey, candidate: SourceCandidate, payload: bytes
) -> PlannedClosurePayload:
    snapshot = candidate.snapshot()
    if snapshot.size != len(payload) or candidate.read_bytes() != payload:
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE", [key, candidate.path],
            "source payload changed during closure preflight")
    return PlannedClosurePayload(
        key, candidate.path, "reuse", payload, snapshot)


def _validate_collection_authority(collection, key: ResourceKey) -> None:
    if collection.library is not None:
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE", [key, collection.name],
            "linked read-only Blender Collections cannot be closure authority")
    if key.kind == "composite":
        linked_objects = [
            obj.name for obj in collection.objects if obj.library is not None]
        if linked_objects:
            _raise(
                "MH_E_INVALID_RESOURCE_SOURCE", [key, *linked_objects],
                "linked read-only placement objects cannot be closure authority")


def _validate_root_marker(collection, root_name: str) -> None:
    has_kind = COLLECTION_KIND_KEY in collection
    has_name = COLLECTION_RESOURCE_KEY in collection
    if not has_kind and not has_name:
        return
    if (not has_kind or not has_name
            or collection.get(COLLECTION_KIND_KEY) != "composite"
            or collection.get(COLLECTION_RESOURCE_KEY) != root_name):
        _raise(
            "MH_E_RESOURCE_KIND_MISMATCH", [collection.name, root_name],
            "selected root carries a partial or conflicting managed resource "
            "identity")


def _unmanaged_reason(instance, kind: str, resource: str) -> str:
    """Explain WHY a bound Collection is not an MH definition, and what fixes it."""

    detail = ""
    dagor_type = instance.get("type")
    dagor_name = instance.get("name")
    if isinstance(dagor_type, str) or isinstance(dagor_name, str):
        detail += (
            f". Collection {instance.name!r} is a dag4blend definition "
            f"(type={dagor_type!r}, name={dagor_name!r}), not an MH one: its "
            "contents are dag4blend-shaped and publishing them as a v5 source "
            "would be wrong. Convert it with 'Convert dag4blend Scene "
            "Composite' (mh.convert_dag4blend_composite), or convert the "
            "authoritative *.composit.blk with 'Import Dagor Composite' "
            "(mh.import_dagor_composite)")
    existing = managed_resource_collections(kind, resource)
    if existing:
        names = ", ".join(repr(row.name) for row in existing)
        detail += (
            f". A managed definition for {kind}:{resource} already exists in "
            f"this file as {names} — repoint this placement's "
            f"instance_collection at it instead of {instance.name!r}")
    return detail


def _validate_direct_bindings(collection) -> None:
    """Reject direct unmanaged definitions before token-only graph fallback."""

    for obj in collection.objects:
        settings = getattr(obj, "mh4blend", None)
        parent_settings = getattr(getattr(obj, "parent", None), "mh4blend", None)
        is_option = (
            parent_settings is not None
            and parent_settings.kind == "random"
            and settings is not None
            and settings.is_property_set("option_index")
        )
        explicit_resource = obj.get("mh_composite_resource")
        if (settings is not None
                and settings.kind in {"mesh", "composite"}
                and isinstance(explicit_resource, str)
                and explicit_resource
                and obj.instance_collection is not None
                and not is_managed_resource_collection(
                    obj.instance_collection,
                    settings.kind,
                    explicit_resource,
                )):
            _raise(
                "MH_E_INVALID_RESOURCE_SOURCE",
                [f"{settings.kind}:{explicit_resource}", obj.name,
                 obj.instance_collection.name],
                "loaded closure dependency is unmanaged; exact MH Collection "
                "identity stamps are required"
                + _unmanaged_reason(
                    obj.instance_collection, settings.kind, explicit_resource))
        kind, resource = _node_kind_and_resource(obj, option=is_option)
        if kind not in {"mesh", "composite"} or resource is None:
            continue
        instance = obj.instance_collection
        if instance is None:
            # A visible unresolved placement is admitted only if its source is
            # found later.  It carries no loaded authority claim.
            continue
        if not is_managed_resource_collection(instance, kind, resource):
            _raise(
                "MH_E_INVALID_RESOURCE_SOURCE",
                [f"{kind}:{resource}", obj.name, instance.name],
                "loaded closure dependency is unmanaged; exact MH Collection "
                "identity stamps are required"
                + _unmanaged_reason(instance, kind, resource))


def _managed_collection(kind: str, name: str):
    candidates = managed_resource_collections(kind, name)
    if len(candidates) > 1:
        _raise(
            "MH_E_AMBIGUOUS_RESOURCE_NAME",
            [f"{kind}:{name}", *(row.name for row in candidates)],
            "multiple managed Blender Collections claim one ResourceKey")
    return candidates[0] if candidates else None


def _source_composite(
        candidate: SourceCandidate) -> tuple[object, PlannedClosurePayload]:
    resource = read_composite_file(candidate.path)
    raw = _exact_source_payload(
        candidate, composite_json_bytes(resource),
        grammar_code="MH_E_COMPOSITE_GRAMMAR")
    return resource, _reuse_row(candidate.key, candidate, raw)


def _source_profile(candidate: SourceCandidate) -> PlannedClosurePayload:
    resource = read_placement_file(candidate.path)
    raw = _exact_source_payload(
        candidate, placement_json_bytes(resource),
        grammar_code="MH_E_PLACEMENT_PROFILE_GRAMMAR")
    return _reuse_row(candidate.key, candidate, raw)


def _source_material(
        candidate: SourceCandidate
) -> tuple[object, PlannedClosurePayload]:
    raw = candidate.read_bytes()
    resource = parse_material(raw, name=candidate.key.name)
    raw = _exact_source_payload(
        candidate, material_json_bytes(resource),
        grammar_code="MH_E_MATERIAL_GRAMMAR")
    return resource, _reuse_row(candidate.key, candidate, raw)


def prepare_composite_closure_export(
        collection, output_dir, *, source_root,
        mode=CLOSURE_MODE_COMPOSITES) -> ClosureExportPlan:
    """Build and fully validate one closure without staging or publication."""

    if collection is None:
        raise ValueError("collection is required")
    if mode not in _CLOSURE_MODES:
        raise ValueError(f"unsupported closure export mode {mode!r}")

    inventory = scan_source_inventory(source_root)
    output = _resolved_output(inventory.root, output_dir)
    root_resource = _extract_composite(collection)
    root_key = ResourceKey("composite", root_resource.name)
    _validate_root_marker(collection, root_resource.name)
    _validate_collection_authority(collection, root_key)
    _validate_direct_bindings(collection)

    composite_resources = {root_resource.name: root_resource}
    composite_rows: dict[str, PlannedClosurePayload] = {}
    loaded_composites = {root_resource.name: collection}

    def resolve_composite(name: str):
        existing = composite_resources.get(name)
        if existing is not None:
            return existing
        key = ResourceKey("composite", name)
        if mode == CLOSURE_MODE_ROOT:
            owners = []
            for owner_name, resource in composite_resources.items():
                if name in iter_resource_references(
                        resource, kind="composite"):
                    owners.append(ResourceKey("composite", owner_name))
            candidate = _resolve_excluded_source(inventory, key, owners)
            resource, row = _source_composite(candidate)
            composite_resources[name] = resource
            composite_rows[name] = row
            return resource
        loaded = _managed_collection("composite", name)
        if loaded is not None:
            _validate_collection_authority(loaded, key)
            _validate_direct_bindings(loaded)
            resource = _extract_composite(loaded)
            if resource.name != name:
                _raise(
                    "MH_E_RESOURCE_KIND_MISMATCH", [key, loaded.name],
                    "managed composite Collection extracted a different "
                    "logical identity")
            composite_resources[name] = resource
            loaded_composites[name] = loaded
            return resource
        candidate = inventory.resolve(key, allow_missing=True)
        if candidate is None:
            return None
        resource, row = _source_composite(candidate)
        composite_resources[name] = resource
        composite_rows[name] = row
        return resource

    # The immutable builder drives recursive resolution through every option.
    closure = build_composite_source_closure(
        root_resource.name, resolve_composite)

    # Profiles have no Blender datablock carrier; exact existing source is the
    # only authority in this slice and is always reuse-only.
    profile_rows = []
    for key in sorted(closure.placement_profiles):
        candidate = inventory.resolve(key)
        profile_rows.append(_source_profile(candidate))

    mesh_rows = []
    validated_only: dict[ResourceKey, SourceSnapshot] = {}
    material_names: set[str] = set()
    material_owners: dict[str, dict[ResourceKey, None]] = {}
    for key in sorted(closure.static_meshes):
        owners = closure.referrers_for(key)
        source_candidate = (
            _resolve_excluded_source(inventory, key, owners)
            if mode != CLOSURE_MODE_INCLUDE_ALL
            else inventory.resolve(key, allow_missing=True))
        loaded = _managed_collection("mesh", key.name)
        if mode != CLOSURE_MODE_INCLUDE_ALL:
            assert source_candidate is not None
            source_plan = parse_mesh_fbx(source_candidate.path)
            material_names.update(source_plan.material_names)
            for name in source_plan.material_names:
                owner_set = material_owners.setdefault(name, {})
                for owner in owners:
                    owner_set.setdefault(owner, None)
            # Excluded payloads remain full closure members: exact source bytes
            # are staged/read back, but action=reuse prevents replacement.
            mesh_rows.append(_reuse_row(
                key, source_candidate, source_candidate.read_bytes()))
            continue

        if loaded is not None:
            _validate_collection_authority(loaded, key)
            prepared = prepare_fbx_collection(
                loaded, output, source_root=inventory.root,
                export_materials=False)
            if prepared.resource_name != key.name:
                _raise(
                    "MH_E_RESOURCE_KIND_MISMATCH", [key, loaded.name],
                    "managed mesh Collection prepared a different logical "
                    "identity")
            target, _existing = _target_for(
                inventory, output, key, ".mesh.fbx")
            prepared = replace(prepared, target=target)
            material_names.update(material.name for material in prepared.materials)
            for material in prepared.materials:
                owner_set = material_owners.setdefault(material.name, {})
                for owner in owners:
                    owner_set.setdefault(owner, None)
            mesh_rows.append(PlannedClosurePayload(
                key, target, "publish", None,
                (_existing.snapshot() if _existing is not None else None),
                prepared))
        elif source_candidate is not None:
            source_plan = parse_mesh_fbx(source_candidate.path)
            material_names.update(source_plan.material_names)
            for name in source_plan.material_names:
                owner_set = material_owners.setdefault(name, {})
                for owner in owners:
                    owner_set.setdefault(owner, None)
            mesh_rows.append(_reuse_row(
                key, source_candidate, source_candidate.read_bytes()))
        else:
            _raise(
                "MH_E_RESOURCE_NOT_FOUND", [key, *_owner_subjects(owners)],
                "mesh dependency has neither managed Blender authority nor "
                "an existing source payload")

    material_rows = []
    texture_keys: dict[ResourceKey, None] = {}
    texture_owners: dict[ResourceKey, dict[ResourceKey, None]] = {}
    if mode != CLOSURE_MODE_INCLUDE_ALL:
        # Mesh slot dependencies are excluded from publication too. They still
        # become staged reuse rows, while textures remain preflight-only.
        for name in sorted(material_names):
            key = ResourceKey("material", name)
            owners = tuple(material_owners.get(name, ()))
            candidate = _resolve_excluded_source(inventory, key, owners)
            resource, row = _source_material(candidate)
            material_rows.append(row)
            for token in resource.textures.values():
                texture_key = ResourceKey("texture", token)
                texture_keys.setdefault(texture_key, None)
                owner_set = texture_owners.setdefault(texture_key, {})
                for owner in owners:
                    owner_set.setdefault(owner, None)
    else:
        for name in sorted(material_names):
            key = ResourceKey("material", name)
            owners = tuple(material_owners.get(name, ()))
            source_candidate = inventory.resolve(key, allow_missing=True)
            material = bpy.data.materials.get(name)
            if material is not None:
                if material.library is not None:
                    _raise(
                        "MH_E_INVALID_RESOURCE_SOURCE", [key, material.name],
                        "linked read-only Blender Materials cannot be closure "
                        "authority")
                try:
                    prepared = prepare_blender_material_export(
                        material, output, source_root=inventory.root)
                except MaterialValueError as exc:
                    # The standalone material preparer wraps resolver context
                    # for its own UI. Re-resolve its typed texture tokens here
                    # so closure diagnostics retain ResourceKey provenance.
                    resource = _extract_material_resource(material)
                    for token in resource.textures.values():
                        _resolve_texture_source(
                            inventory,
                            ResourceKey("texture", token),
                            tuple(material_owners.get(name, ())),
                        )
                    raise exc
                target, _existing = _target_for(
                    inventory, output, key, ".material")
                prepared = PreparedMaterialExport(
                    prepared.resource, target, prepared.payload)
                for token in prepared.resource.textures.values():
                    texture_key = ResourceKey("texture", token)
                    texture_keys.setdefault(texture_key, None)
                    owner_set = texture_owners.setdefault(texture_key, {})
                    for owner in material_owners.get(name, ()):
                        owner_set.setdefault(owner, None)
                material_rows.append(PlannedClosurePayload(
                    key, target, "publish", prepared.payload,
                    (_existing.snapshot() if _existing is not None else None),
                    prepared))
            elif source_candidate is not None:
                resource, row = _source_material(source_candidate)
                for token in resource.textures.values():
                    texture_key = ResourceKey("texture", token)
                    texture_keys.setdefault(texture_key, None)
                    owner_set = texture_owners.setdefault(texture_key, {})
                    for owner in material_owners.get(name, ()):
                        owner_set.setdefault(owner, None)
                material_rows.append(row)
            else:
                _raise(
                    "MH_E_RESOURCE_NOT_FOUND", [key, *_owner_subjects(owners)],
                    "material dependency has neither loaded Blender authority "
                    "nor an existing source payload")

    # Textures never have a batch publish phase. Every token uses the ratified
    # resolver and remains under exact snapshot revalidation until first write.
    for key in texture_keys:
        candidate = _resolve_texture_source(
            inventory, key, tuple(texture_owners.get(key, ())))
        validated_only.setdefault(key, candidate.snapshot())

    # Loaded composites are canonical payloads ready for staging.  Existing
    # unique targets are replaced in place; new resources use output_dir.
    for key in closure.composites_postorder:
        if key.name in composite_rows:
            continue
        loaded = loaded_composites[key.name]
        resource = composite_resources[key.name]
        target, existing = _target_for(
            inventory, output, key, ".composite")
        composite_rows[key.name] = PlannedClosurePayload(
            key, target, "publish", composite_json_bytes(resource),
            (existing.snapshot() if existing is not None else None), loaded)

    ordered_composites = [
        composite_rows[key.name] for key in closure.composites_postorder]
    payloads = tuple(
        profile_rows
        + sorted(material_rows, key=lambda row: row.key)
        + sorted(mesh_rows, key=lambda row: row.key)
        + ordered_composites
    )
    return ClosureExportPlan(
        mode=mode,
        source_root=inventory.root,
        output_dir=output,
        closure=closure,
        payloads=payloads,
        validated_only=tuple(validated_only.items()),
        texture_dependencies=tuple(texture_keys),
    )


def _physical_inside(root: Path, path: Path) -> bool:
    try:
        return os.path.commonpath([
            os.path.normcase(str(root.resolve(strict=True))),
            os.path.normcase(str(path.resolve(strict=False))),
        ]) == os.path.normcase(str(root.resolve(strict=True)))
    except (OSError, ValueError):
        return False


def _stage_filename(key: ResourceKey) -> str:
    extension = {
        "placement_profile": ".placement",
        "material": ".material",
        "static_mesh": ".mesh.fbx",
        "composite": ".composite",
        "texture": "",
    }[key.kind]
    if not extension:
        raise ValueError("texture dependencies are preflight-only, not staged")
    return f"{key.name}{extension}"


def _validate_staged_document(
        row: PlannedClosurePayload, path: Path, payload: bytes) -> None:
    key = row.key
    if key.kind == "composite":
        resource = read_composite_file(path)
        if composite_json_bytes(resource) != payload:
            _raise(
                "MH_E_COMPOSITE_GRAMMAR", [key, path],
                "staged composite failed canonical read-back")
    elif key.kind == "placement_profile":
        resource = read_placement_file(path)
        if placement_json_bytes(resource) != payload:
            _raise(
                "MH_E_PLACEMENT_PROFILE_GRAMMAR", [key, path],
                "staged placement profile failed canonical read-back")
    elif key.kind == "material":
        resource = parse_material(payload, name=key.name)
        if material_json_bytes(resource) != payload:
            _raise(
                "MH_E_MATERIAL_GRAMMAR", [key, path],
                "staged material failed canonical read-back")
    elif key.kind == "static_mesh" and row.action == "reuse":
        parsed = parse_mesh_fbx(path)
        if parsed.resource_name != key.name:
            _raise(
                "MH_E_RESOURCE_KIND_MISMATCH", [key, path],
                "staged reused FBX extracted a different identity")


def stage_composite_closure_export(
        plan: ClosureExportPlan, *, staging_dir
) -> tuple[StagedClosurePayload, ...]:
    """Stage and read back every member, including reuse-only sources."""

    if not isinstance(plan, ClosureExportPlan):
        raise TypeError("plan must be ClosureExportPlan")
    directory = Path(staging_dir).resolve(strict=True)
    if not directory.is_dir():
        raise ValueError("closure staging_dir must be an existing directory")
    if _physical_inside(plan.source_root, directory):
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE", [directory, plan.source_root],
            "closure staging_dir must be outside Project Source Root")

    staged_rows = []
    created_files = []
    created_directories = []
    try:
        for index, row in enumerate(plan.payloads):
            member_dir = directory / f"{index:04d}-{row.key.kind}"
            if os.path.lexists(member_dir):
                raise FileExistsError(
                    f"closure staging member already exists: {member_dir}")
            member_dir.mkdir()
            created_directories.append(member_dir)
            path = member_dir / _stage_filename(row.key)

            if row.key.kind == "static_mesh" and row.action == "publish":
                if not isinstance(row.prepared, PreparedFBXExport):
                    raise TypeError(
                        "published mesh row requires PreparedFBXExport")
                from .export_fbx import stage_prepared_fbx
                staged = stage_prepared_fbx(row.prepared, path)
                payload = staged.payload
                created_files.append(path)
            else:
                if row.payload is None:
                    raise ValueError(f"closure row has no stageable bytes: {row.key}")
                with path.open("xb") as stream:
                    stream.write(row.payload)
                    stream.flush()
                    os.fsync(stream.fileno())
                created_files.append(path)
                payload = path.read_bytes()
                if payload != row.payload:
                    raise OSError(
                        f"staged closure read-back differs for {row.key}")
                _validate_staged_document(row, path, payload)

            staged_rows.append(StagedClosurePayload(
                row, path, path.resolve(strict=True), payload))
    except Exception:
        for path in reversed(created_files):
            try:
                path.unlink()
            except FileNotFoundError:
                pass
        for path in reversed(created_directories):
            try:
                path.rmdir()
            except FileNotFoundError:
                pass
        raise
    return tuple(staged_rows)


def revalidate_composite_closure_export(
        plan: ClosureExportPlan,
        staged: tuple[StagedClosurePayload, ...], *,
        published=()) -> None:
    """Recheck all identities and bytes at the edge before first replace."""

    if not isinstance(plan, ClosureExportPlan):
        raise TypeError("plan must be ClosureExportPlan")
    rows = tuple(staged)
    if len(rows) != len(plan.payloads) or any(
            staged_row.planned != planned
            for staged_row, planned in zip(rows, plan.payloads)):
        raise ValueError("staged closure does not exactly match its plan")
    published_keys = frozenset(published)
    if any(not isinstance(key, ResourceKey) for key in published_keys):
        raise TypeError("published must contain only ResourceKey values")
    publishable_keys = {row.key for row in plan.to_publish}
    if not published_keys.issubset(publishable_keys):
        raise ValueError("published contains a non-publishable closure member")

    inventory = scan_source_inventory(plan.source_root)
    for key, snapshot in plan.validated_only:
        current = inventory.resolve(key)
        if current.path != snapshot.path:
            _raise(
                "MH_E_AMBIGUOUS_RESOURCE_NAME",
                [key, snapshot.path, current.path],
                "validated-only closure identity changed after preflight")
        if not _snapshot_matches(snapshot, current.read_bytes()):
            _raise(
                "MH_E_INVALID_RESOURCE_SOURCE", [key, current.path],
                "validated-only closure bytes changed after preflight")
    for staged_row in rows:
        row = staged_row.planned
        try:
            path_stat = os.lstat(staged_row.staged_path)
            physical = staged_row.staged_path.resolve(strict=True)
        except OSError:
            path_stat = None
            physical = None
        if (path_stat is None
                or not stat.S_ISREG(path_stat.st_mode)
                or os.path.islink(staged_row.staged_path)
                or physical != staged_row.physical_path
                or _physical_inside(plan.source_root, physical)
                or staged_row.staged_path.read_bytes() != staged_row.payload):
            _raise(
                "MH_E_INVALID_RESOURCE_SOURCE", [row.key, staged_row.staged_path],
                "staged closure payload changed before publication")
        current = inventory.resolve(row.key, allow_missing=True)
        if row.key in published_keys:
            if (current is None
                    or current.path != row.target.resolve(strict=True)
                    or current.read_bytes() != staged_row.payload):
                subjects = [row.key, row.target]
                if current is not None:
                    subjects.append(current.path)
                _raise(
                    "MH_E_INVALID_RESOURCE_SOURCE", subjects,
                    "already-published closure prefix changed during batch")
            continue
        if row.source_snapshot is None:
            if current is not None or os.path.lexists(row.target):
                subjects = [row.key, row.target]
                if current is not None:
                    subjects.append(current.path)
                _raise(
                    "MH_E_AMBIGUOUS_RESOURCE_NAME",
                    subjects,
                    "new closure target identity appeared after preflight")
            continue
        if current is None or current.path != row.source_snapshot.path:
            subjects = [row.key, row.source_snapshot.path]
            if current is not None:
                subjects.append(current.path)
            _raise(
                "MH_E_AMBIGUOUS_RESOURCE_NAME",
                subjects,
                "closure source identity changed after preflight")
        raw = current.read_bytes()
        if not _snapshot_matches(row.source_snapshot, raw):
            _raise(
                "MH_E_INVALID_RESOURCE_SOURCE", [row.key, current.path],
                "closure source bytes changed after preflight")


def publish_composite_closure_export(
        plan: ClosureExportPlan,
        staged: tuple[StagedClosurePayload, ...], *,
        lock_root=None,
        _boundary_hook=None,
        _crash_identity=None,
        _crash_at=None) -> dict:
    """Publish the irreversible dependency-closed prefix in plan order."""

    rows = tuple(staged)
    revalidate_composite_closure_export(plan, rows)
    staged_by_key = {row.planned.key: row for row in rows}
    if len(staged_by_key) != len(rows):
        raise ValueError("staged closure contains duplicate ResourceKeys")
    publish_rows = tuple(
        staged_by_key[row.key] for row in plan.to_publish)
    if not publish_rows or publish_rows[-1].planned.key != plan.closure.root:
        raise ValueError("closure publish order must end with the root composite")

    identity_to_key = {
        str(row.planned.key): row.planned.key for row in publish_rows}

    def guard(published_identities: tuple[str, ...]) -> None:
        revalidate_composite_closure_export(
            plan,
            rows,
            published=tuple(
                identity_to_key[identity]
                for identity in published_identities),
        )

    items = tuple(BatchPublishItem(
        identity=str(row.planned.key),
        target=row.planned.target,
        payload=row.payload,
    ) for row in publish_rows)
    receipts = publish_ordered_batch(
        items,
        source_root=plan.source_root,
        lock_root=lock_root,
        pre_replace_guard=guard,
        _boundary_hook=_boundary_hook,
        _crash_identity=_crash_identity,
        _crash_at=_crash_at,
    )
    return {
        "ok": True,
        "published": [row.identity for row in items],
        "unpublished": [],
        "reused": [str(row.key) for row in plan.reused],
        "receipts": list(receipts),
    }


def _finalize_published_blender_state(
        plan: ClosureExportPlan, identities) -> None:
    keys = {str(key): key for key in (row.key for row in plan.payloads)}
    for identity in identities:
        key = keys.get(identity)
        if key is None:
            continue
        row = plan.row_for(key)
        if key.kind == "composite" and row.prepared is not None:
            for obj in row.prepared.objects:
                sync_typed_mirror(obj)
            stamp_resource_collection(row.prepared, "composite", key.name)
        elif key.kind == "static_mesh" and isinstance(
                row.prepared, PreparedFBXExport):
            stamp_resource_collection(row.prepared.collection, "mesh", key.name)


def export_composite_closure_collection(
        collection, output_dir, *, source_root,
        mode=CLOSURE_MODE_COMPOSITES,
        lock_root=None,
        _boundary_hook=None) -> dict:
    """Run write-free preflight, full staging, then ordered publication."""

    plan = prepare_composite_closure_export(
        collection, output_dir, source_root=source_root, mode=mode)
    with tempfile.TemporaryDirectory(prefix="mh-v5-closure-stage-") as value:
        staged = stage_composite_closure_export(plan, staging_dir=value)
        try:
            report = publish_composite_closure_export(
                plan,
                staged,
                lock_root=lock_root,
                _boundary_hook=_boundary_hook,
            )
        except BatchPartialPublishError as exc:
            _finalize_published_blender_state(plan, exc.published)
            raise
    _finalize_published_blender_state(plan, report["published"])
    root_row = plan.row_for(plan.closure.root)
    root_staged = next(
        row for row in staged if row.planned.key == plan.closure.root)
    report.update({
        "mode": mode,
        "root": str(plan.closure.root),
        "closure": [str(key) for key in plan.full_closure_keys],
        "staged": [str(row.key) for row in plan.payloads],
        "filepath": str(root_row.target),
        "resource_name": plan.closure.root.name,
        "nodes": len(list(collection.objects)),
        "bytes": len(root_staged.payload),
        "written": str(plan.closure.root) in report["published"],
    })
    return report
