"""Transactional Blender import of Source Protocol v5 composites."""

from __future__ import annotations

from collections import Counter
import contextlib
import os
from pathlib import Path

import bpy
from mathutils import Matrix, Quaternion

from ..core.canonical import validate_resource_name
from ..core.composites import (
    iter_resource_references,
    read_composite_file,
    validate_composite_cycles,
)
from ..core.model import Composite
from ..core.transforms import ue_to_blender_transform
from ..core.validate import MHValidationError
from .export_composite import (
    COLLECTION_KIND_KEY,
    COLLECTION_RESOURCE_KEY,
    NODE_KIND_KEY,
    NODE_NAME_KEY,
    NODE_RESOURCE_KEY,
    UNRESOLVED_PLACEMENT_KEY,
    _collection_instance_identity,
    _stamp_imported_transform,
)
from .resource_markers import stamp_resource_collection
from .service_scenes import ensure_service_scenes
from ..ui.composite_authoring import sync_typed_mirror
from .import_fbx import (
    MeshImportTransaction,
    import_mesh_fbx,
    mesh_import_id_names,
    parse_mesh_fbx,
    preflight_mesh_import_plan,
)

__all__ = [
    "UNRESOLVED_PLACEMENT_KEY",
    "import_composite_file",
    "materialize_composite_documents",
]


_BLENDER_ID_NAME_MAX_BYTES = 63


def _validate_blender_id_name(name: str, subject: str) -> None:
    if (not isinstance(name, str)
            or len(name.encode("utf-8")) > _BLENDER_ID_NAME_MAX_BYTES):
        raise MHValidationError(
            "MH_E_IMPORT_TARGET_OCCUPIED", [f"{subject}:{name}"],
            "cannot preserve exact Blender ID name (maximum is 63 UTF-8 "
            "bytes); import repair or truncation is forbidden")


def _resolved_root(source_root) -> Path:
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


def _resolve_source(
        root: Path, name: str, extension: str, *, allow_missing=False
) -> Path | None:
    validate_resource_name(name)
    expected = f"{name}{extension}"
    matches = []
    for path in root.rglob("*"):
        if not path.is_file() or path.name.casefold() != expected.casefold():
            continue
        if path.name != expected:
            raise MHValidationError(
                "MH_E_NONCANONICAL_RESOURCE_NAME", [str(path)],
                f"source filename must be exactly '{expected}'")
        matches.append(path.resolve(strict=False))
    matches.sort(key=lambda row: str(row).replace("\\", "/"))
    if len(matches) > 1:
        raise MHValidationError(
            "MH_E_AMBIGUOUS_RESOURCE_NAME", [name, *(str(row) for row in matches)],
            f"multiple resources resolve as '{expected}'")
    if not matches:
        if allow_missing:
            return None
        raise MHValidationError(
            "MH_E_UNRESOLVED_COMPOSITE_REFERENCE", [name],
            f"required source resource '{expected}' was not found")
    return matches[0]


def _load_closure(root_name: str, root_path: Path, source_root: Path):
    documents = {}
    paths = {}
    missing_composites = set()

    def load(name: str, explicit_path: Path | None = None):
        if name in documents or name in missing_composites:
            return
        path = explicit_path or _resolve_source(
            source_root, name, ".composite", allow_missing=True)
        if path is None:
            missing_composites.add(name)
            return
        document = read_composite_file(path)
        if document.name != name:
            raise MHValidationError(
                "MH_E_NONCANONICAL_RESOURCE_NAME", [str(path)],
                "composite filename identity differs from requested resource")
        documents[name] = document
        paths[name] = path
        for dependency in iter_resource_references(document, kind="composite"):
            load(dependency)

    load(root_name, root_path)
    cycle_documents = dict(documents)
    cycle_documents.update(
        (name, Composite(name)) for name in missing_composites)
    validate_composite_cycles(root_name, cycle_documents)
    return documents, paths


def _placement_name(composite_name: str, index: int) -> str:
    return f"MH_CMP_{composite_name}_{index:04d}"


def _actor_collection_name(resource_name: str) -> str:
    return f"{resource_name}.actor"


def _iter_nodes(document):
    index = 0

    def walk(nodes):
        nonlocal index
        for node in nodes:
            current = index
            index += 1
            yield current, node
            yield from walk(node.children)

    yield from walk(document.nodes)


def _authoring_object_count(document) -> int:
    return sum(
        1 + (len(node.options) if node.kind == "random" else 0)
        for _index, node in _iter_nodes(document)
    )


def _preflight(documents, source_root: Path | None, *, preloaded_resources=frozenset()):
    collection_names = [f"{name}.composite" for name in documents]
    placement_names = {
        _placement_name(name, index)
        for name, document in documents.items()
        for index in range(_authoring_object_count(document))
    }
    object_names = list(placement_names)
    mesh_names = []
    for name in collection_names:
        _validate_blender_id_name(name, "collection")
    for name in placement_names:
        _validate_blender_id_name(name, "object")
    mesh_paths = {}
    mesh_plans = {}
    missing_meshes = set()
    actor_collection_names = set()
    for document in documents.values():
        for _index, node in _iter_nodes(document):
            if node.profile is not None:
                raise MHValidationError(
                    "MH_E_COMPOSITE_GRAMMAR", [document.name],
                    "STOP OPEN-V5-9: Blender profile authority and Dagor "
                    "include identity require an owner decision")
        for name in iter_resource_references(document, kind="mesh"):
            if ("mesh", name) in preloaded_resources:
                continue
            if name in mesh_paths or name in missing_meshes:
                continue
            if source_root is None:
                raise MHValidationError(
                    "MH_E_INVALID_RESOURCE_SOURCE", [name],
                    "materialization without Project Source Root requires "
                    "every mesh resource to be supplied explicitly")
            path = _resolve_source(
                source_root, name, ".mesh.fbx", allow_missing=True)
            if path is None:
                missing_meshes.add(name)
                continue
            plan = parse_mesh_fbx(path)
            preflight_mesh_import_plan(plan, source_root)
            mesh_paths[name] = path
            mesh_plans[name] = plan
            id_names = mesh_import_id_names(plan)
            collection_names.extend(id_names["collections"])
            object_names.extend(id_names["objects"])
            mesh_names.extend(id_names["meshes"])

        for name in iter_resource_references(document, kind="actor"):
            if ("actor", name) not in preloaded_resources:
                actor_collection_names.add(_actor_collection_name(name))

    for name in sorted(actor_collection_names):
        _validate_blender_id_name(name, "collection")
        collection_names.append(name)

    occupied_collections = sorted(
        f"collection:{name}" for name in set(collection_names)
        if bpy.data.collections.get(name) is not None)
    occupied_objects = sorted(
        f"object:{name}" for name in set(object_names)
        if bpy.data.objects.get(name) is not None)
    occupied_meshes = sorted(
        f"mesh:{name}" for name in set(mesh_names)
        if bpy.data.meshes.get(name) is not None)

    # The closure itself must also fit Blender's global ID namespaces without
    # relying on automatic .001 repair.
    duplicate_names = []
    for namespace, names in (
            ("collection", collection_names),
            ("object", object_names),
            ("mesh", mesh_names)):
        duplicate_names.extend(
            f"{namespace}:{name}"
            for name, count in Counter(names).items()
            if count > 1)
    conflicts = (
        occupied_collections + occupied_objects + occupied_meshes
        + sorted(duplicate_names))
    if conflicts:
        raise MHValidationError(
            "MH_E_IMPORT_TARGET_OCCUPIED", conflicts,
            "composite closure cannot be materialized without Blender ID "
            "auto-renaming")
    return mesh_paths, mesh_plans


def _matrix_local(transform):
    translation, rotation_xyzw, scale = ue_to_blender_transform(transform)
    x, y, z, w = rotation_xyzw
    rotation = Quaternion((w, x, y, z)).to_matrix().to_4x4()
    scale_matrix = Matrix.Diagonal((*scale, 1.0))
    return Matrix.Translation(translation) @ rotation @ scale_matrix


def _build_definition(document, collection, resources, warnings) -> int:
    count = 0

    def make_object(parent):
        nonlocal count
        obj = bpy.data.objects.new(
            _placement_name(document.name, count), None)
        count += 1
        collection.objects.link(obj)
        if parent is not None:
            obj.parent = parent
        obj.matrix_parent_inverse = Matrix.Identity(4)
        return obj

    def bind_resource(obj, kind, resource_name):
        if resource_name is not None:
            obj[NODE_RESOURCE_KEY] = resource_name
        if kind not in {"mesh", "actor", "composite"}:
            obj.empty_display_type = "PLAIN_AXES"
            return
        resource = resources.get((kind, resource_name))
        if resource is None:
            obj[UNRESOLVED_PLACEMENT_KEY] = True
            obj.empty_display_type = "CUBE"
            obj.color = (1.0, 0.0, 0.0, 1.0)
            warnings.append({
                "code": "MH_W_UNRESOLVED_PLACEMENT",
                "subjects": sorted([f"{kind}:{resource_name}", obj.name]),
                "message": (
                    f"{kind} resource '{resource_name}' is missing; "
                    "placement was imported as a placeholder"),
            })
            return
        obj.instance_type = "COLLECTION"
        obj.instance_collection = resource
        obj.empty_display_type = "ARROWS" if kind == "actor" else "PLAIN_AXES"

    def stamp_kind(obj, kind):
        obj.mh4blend.kind = kind
        sync_typed_mirror(obj)

    def build(nodes, parent=None):
        for node in nodes:
            obj = make_object(parent)
            stamp_kind(obj, node.kind)
            if node.name is not None:
                obj[NODE_NAME_KEY] = node.name
            bind_resource(obj, node.kind, node.resource)
            obj.matrix_basis = _matrix_local(node.transform)
            _stamp_imported_transform(obj, node.transform)

            if node.kind == "random":
                for option_index, option in enumerate(node.options):
                    option_obj = make_object(obj)
                    option_obj.mh4blend.kind = option.kind
                    option_obj.mh4blend.weight = option.weight
                    option_obj.mh4blend.option_index = option_index
                    bind_resource(
                        option_obj, option.kind, option.resource)
                    option_obj.matrix_basis = Matrix.Identity(4)
                    sync_typed_mirror(option_obj)
            build(node.children, obj)

    build(document.nodes)
    return count


def _validate_document_mapping(root_name, documents) -> dict:
    validate_resource_name(root_name)
    normalized = dict(documents)
    if root_name not in normalized:
        raise MHValidationError(
            "MH_E_UNRESOLVED_COMPOSITE_REFERENCE", [root_name],
            "root composite document was not supplied for materialization")
    for name, document in normalized.items():
        if not isinstance(document, Composite):
            raise MHValidationError(
                "MH_E_COMPOSITE_GRAMMAR", [name, type(document).__name__],
                "materialization document values must be Composite DTOs")
        if name != document.name:
            raise MHValidationError(
                "MH_E_NONCANONICAL_RESOURCE_NAME", [name, document.name],
                "composite mapping key must equal document.name")
        validate_resource_name(name)
    missing = {
        dependency
        for document in normalized.values()
        for dependency in iter_resource_references(document, kind="composite")
        if dependency not in normalized
    }
    cycle_documents = dict(normalized)
    cycle_documents.update((name, Composite(name)) for name in missing)
    validate_composite_cycles(root_name, cycle_documents)
    return normalized


def materialize_composite_documents(
        documents, *, root_name, source_root, filepath=None,
        resource_overrides=None) -> dict:
    """Materialize validated DTOs behind one exact rollback boundary.

    ``resource_overrides`` is the explicit seam used by the dag4blend scene
    converter.  Existing collections are never stamped or otherwise mutated;
    they are linked into service scenes only after the transaction commits.
    """
    documents = _validate_document_mapping(root_name, documents)
    root = None if source_root is None else _resolved_root(source_root)
    overrides = dict(resource_overrides or {})
    referenced_override_keys = {
        ("mesh", dependency)
        for document in documents.values()
        for dependency in iter_resource_references(document, kind="mesh")
    }
    override_collections = {}
    for key, collection in overrides.items():
        if (not isinstance(key, tuple) or len(key) != 2
                or key[0] != "mesh"
                or not isinstance(key[1], str)):
            raise MHValidationError(
                "MH_E_COMPOSITE_GRAMMAR", [repr(key)],
                "resource override key must be ('mesh', canonical_name); "
                "actor tokens use canonical placeholders and composite "
                "definitions must be supplied as converted DTOs")
        validate_resource_name(key[1])
        if key not in referenced_override_keys:
            raise MHValidationError(
                "MH_E_COMPOSITE_GRAMMAR", [repr(key)],
                "resource override is not referenced by the supplied closure")
        if collection is None or bpy.data.collections.get(collection.name) is not collection:
            raise MHValidationError(
                "MH_E_INVALID_RESOURCE_SOURCE", [repr(key)],
                "resource override must name a live Blender collection")
        has_mh_identity = (
            COLLECTION_KIND_KEY in collection
            and COLLECTION_RESOURCE_KEY in collection)
        has_dagor_identity = "type" in collection and "name" in collection
        if not has_mh_identity and not has_dagor_identity:
            raise MHValidationError(
                "MH_E_INVALID_RESOURCE_SOURCE", [repr(key), collection.name],
                "resource override requires explicit MH markers or exact "
                "dag4blend type/name identity")
        actual_key = _collection_instance_identity(collection)
        if actual_key != key:
            raise MHValidationError(
                "MH_E_RESOURCE_KIND_MISMATCH",
                [repr(key), repr(actual_key), collection.name],
                "resource override key disagrees with explicit collection identity")
        identity = collection.as_pointer()
        previous_key = override_collections.get(identity)
        if previous_key is not None and previous_key != key:
            raise MHValidationError(
                "MH_E_AMBIGUOUS_RESOURCE_NAME",
                [repr(previous_key), repr(key), collection.name],
                "one resource collection cannot represent multiple logical keys")
        override_collections[identity] = key

    mesh_paths, _mesh_plans = _preflight(
        documents, root, preloaded_resources=frozenset(overrides))

    override_links = []
    try:
        with MeshImportTransaction() as transaction:
            service_scenes = ensure_service_scenes()

            def route(scene_name, collection):
                target = service_scenes[scene_name].collection
                if target.children.get(collection.name) is None:
                    target.children.link(collection)

            resources = dict(overrides)
            mesh_reports = []
            for name, mesh_path in mesh_paths.items():
                report = import_mesh_fbx(
                    mesh_path, source_root=root, transaction=transaction)
                collection = report["collection"]
                stamp_resource_collection(collection, "mesh", name)
                if bpy.context.scene.collection.children.get(
                        collection.name) is not None:
                    bpy.context.scene.collection.children.unlink(collection)
                route("MESH", collection)
                resources[("mesh", name)] = collection
                mesh_reports.append({key: value for key, value in report.items()
                                     if key != "collection"})

            composite_collections = {}
            for name in documents:
                collection = bpy.data.collections.new(f"{name}.composite")
                stamp_resource_collection(collection, "composite", name)
                route("COMPOSITE", collection)
                composite_collections[name] = collection
                resources[("composite", name)] = collection

            actor_names = tuple(dict.fromkeys(
                actor_name
                for document in documents.values()
                for actor_name in iter_resource_references(document, kind="actor")
            ))
            for name in actor_names:
                if ("actor", name) in resources:
                    continue
                collection = bpy.data.collections.new(_actor_collection_name(name))
                stamp_resource_collection(collection, "actor", name)
                route("ACTOR_PLACEHOLDERS", collection)
                resources[("actor", name)] = collection

            node_count = 0
            warnings = []
            for name, document in documents.items():
                node_count += _build_definition(
                    document, composite_collections[name], resources, warnings)

            root_collection = composite_collections[root_name]

            # Existing dag4blend definitions are external to the transaction,
            # so every new link needs an explicit rollback journal.
            scene_for_kind = {"mesh": "MESH"}
            for (kind, _name), collection in overrides.items():
                target = service_scenes[scene_for_kind[kind]].collection
                if target.children.get(collection.name) is None:
                    target.children.link(collection)
                    override_links.append((target, collection))
    except Exception:
        for target, collection in reversed(override_links):
            with contextlib.suppress(RuntimeError, ReferenceError):
                if target.children.get(collection.name) is collection:
                    target.children.unlink(collection)
        raise

    # An explicit Composite import is an authoring action: reveal the
    # authoritative COMPOSITE scene only after the transaction has committed.
    # Keeping a non-owning scene active makes Blender report identity
    # matrix_local/world values for datablocks that live exclusively elsewhere.
    composite_scene = service_scenes["COMPOSITE"]
    with contextlib.suppress(RuntimeError, ReferenceError):
        if composite_scene.view_layers:
            composite_scene.view_layers[0].update()
        window = getattr(bpy.context, "window", None)
        if window is not None:
            window.scene = composite_scene
            bpy.context.view_layer.update()

    return {
        "ok": True,
        "filepath": None if filepath is None else str(filepath),
        "resource_name": root_name,
        "collection_name": root_collection.name,
        "collection": root_collection,
        "composites": list(documents),
        "meshes": mesh_reports,
        "nodes": node_count,
        "service_scenes": list(service_scenes),
        "warnings": warnings,
    }


def import_composite_file(filepath, *, source_root) -> dict:
    """Import one complete Composite closure with a single rollback boundary."""
    path = Path(bpy.path.abspath(os.fspath(filepath))).resolve(strict=True)
    root = _resolved_root(source_root)
    if not _inside(root, path):
        raise ValueError("Composite source must be inside Project Source Root")
    if path.suffix != ".composite":
        raise ValueError(
            "MH_E_NONCANONICAL_RESOURCE_NAME: composite filename must end "
            "in lowercase .composite")
    validate_resource_name(path.stem)

    documents, _paths = _load_closure(path.stem, path, root)
    return materialize_composite_documents(
        documents,
        root_name=path.stem,
        source_root=root,
        filepath=path,
    )
