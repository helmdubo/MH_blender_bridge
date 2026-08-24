"""Transactional Blender import of Source Protocol v4 composites."""

from __future__ import annotations

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
from ..core.transforms import ue_to_blender_transform
from ..core.validate import MHValidationError
from .export_composite import (
    COLLECTION_KIND_KEY,
    COLLECTION_RESOURCE_KEY,
    NODE_KIND_KEY,
    NODE_NAME_KEY,
    NODE_RESOURCE_KEY,
)
from .import_fbx import (
    MeshImportTransaction,
    import_mesh_fbx,
    parse_mesh_fbx,
    preflight_mesh_import_plan,
)

__all__ = ["import_composite_file"]


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


def _resolve_source(root: Path, name: str, extension: str) -> Path:
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
        raise MHValidationError(
            "MH_E_UNRESOLVED_COMPOSITE_REFERENCE", [name],
            f"required source resource '{expected}' was not found")
    return matches[0]


def _load_closure(root_name: str, root_path: Path, source_root: Path):
    documents = {}
    paths = {}

    def load(name: str, explicit_path: Path | None = None):
        if name in documents:
            return
        path = explicit_path or _resolve_source(source_root, name, ".composite")
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
    validate_composite_cycles(root_name, documents)
    return documents, paths


def _placement_name(composite_name: str, index: int) -> str:
    return f"MH_CMP_{composite_name}_{index:04d}"


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


def _preflight(documents, source_root: Path):
    collection_names = {f"{name}.composite" for name in documents}
    placement_names = {
        _placement_name(name, index)
        for name, document in documents.items()
        for index, _node in _iter_nodes(document)
    }
    object_names = set(placement_names)
    for name in collection_names:
        _validate_blender_id_name(name, "collection")
    for name in placement_names:
        _validate_blender_id_name(name, "object")
    mesh_paths = {}
    mesh_plans = {}
    for document in documents.values():
        for name in iter_resource_references(document, kind="mesh"):
            if name in mesh_paths:
                continue
            path = _resolve_source(source_root, name, ".mesh.fbx")
            plan = parse_mesh_fbx(path)
            preflight_mesh_import_plan(plan, source_root)
            mesh_paths[name] = path
            mesh_plans[name] = plan
            collection_names.add(plan.target_collection_name)
            object_names.update(node.name for node in plan.nodes)

    occupied_collections = sorted(
        name for name in collection_names if bpy.data.collections.get(name) is not None)
    occupied_objects = sorted(
        name for name in object_names if bpy.data.objects.get(name) is not None)

    # The closure itself must also fit Blender's global ID namespaces without
    # relying on automatic .001 repair.
    all_mesh_node_names = [
        node.name for plan in mesh_plans.values() for node in plan.nodes]
    duplicate_mesh_nodes = sorted({
        name for name in all_mesh_node_names
        if all_mesh_node_names.count(name) > 1
    })
    placement_mesh_conflicts = sorted(
        placement_names.intersection(all_mesh_node_names))
    conflicts = occupied_collections + occupied_objects + duplicate_mesh_nodes
    if placement_mesh_conflicts:
        conflicts.extend(placement_mesh_conflicts)
    if conflicts:
        raise MHValidationError(
            "MH_E_IMPORT_TARGET_OCCUPIED", conflicts,
            "composite closure cannot be materialized without Blender ID "
            "auto-renaming")
    return mesh_paths, mesh_plans


def _matrix_world(transform):
    translation, rotation_xyzw, scale = ue_to_blender_transform(transform)
    x, y, z, w = rotation_xyzw
    rotation = Quaternion((w, x, y, z)).to_matrix().to_4x4()
    scale_matrix = Matrix.Diagonal((*scale, 1.0))
    return Matrix.Translation(translation) @ rotation @ scale_matrix


def _stamp_collection(collection, kind: str, name: str) -> None:
    collection[COLLECTION_KIND_KEY] = kind
    collection[COLLECTION_RESOURCE_KEY] = name


def _build_definition(document, collection, resources) -> int:
    count = 0

    def build(nodes, parent=None):
        nonlocal count
        for node in nodes:
            index = count
            count += 1
            obj = bpy.data.objects.new(
                _placement_name(document.name, index), None)
            collection.objects.link(obj)
            obj[NODE_KIND_KEY] = node.kind
            if node.resource is not None:
                obj[NODE_RESOURCE_KEY] = node.resource
            if node.name is not None:
                obj[NODE_NAME_KEY] = node.name
            if node.kind in {"mesh", "composite"}:
                obj.instance_type = "COLLECTION"
                obj.instance_collection = resources[(node.kind, node.resource)]
                obj.empty_display_type = "PLAIN_AXES"
            elif node.kind == "actor":
                obj.empty_display_type = "ARROWS"
            else:
                obj.empty_display_type = "PLAIN_AXES"
            if parent is not None:
                obj.parent = parent
            # Source values are world transforms by contract.  Set after
            # parenting so Blender derives the local matrix without changing
            # the authored world placement.
            obj.matrix_world = _matrix_world(node.transform)
            build(node.children, obj)

    build(document.nodes)
    return count


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
    mesh_paths, _mesh_plans = _preflight(documents, root)

    with MeshImportTransaction() as transaction:
        resources = {}
        mesh_reports = []
        for name, mesh_path in mesh_paths.items():
            report = import_mesh_fbx(
                mesh_path, source_root=root, transaction=transaction)
            collection = report["collection"]
            _stamp_collection(collection, "mesh", name)
            # Definitions remain datablocks and are instanced by placements;
            # unlinking avoids an extra visible copy at the scene origin.
            if collection.name in bpy.context.scene.collection.children:
                bpy.context.scene.collection.children.unlink(collection)
            resources[("mesh", name)] = collection
            mesh_reports.append({key: value for key, value in report.items()
                                 if key != "collection"})

        composite_collections = {}
        for name in documents:
            collection = bpy.data.collections.new(f"{name}.composite")
            _stamp_collection(collection, "composite", name)
            composite_collections[name] = collection
            resources[("composite", name)] = collection

        node_count = 0
        for name, document in documents.items():
            node_count += _build_definition(
                document, composite_collections[name], resources)

        root_collection = composite_collections[path.stem]
        bpy.context.scene.collection.children.link(root_collection)

    return {
        "ok": True,
        "filepath": str(path),
        "resource_name": path.stem,
        "collection_name": root_collection.name,
        "collection": root_collection,
        "composites": list(documents),
        "meshes": mesh_reports,
        "nodes": node_count,
        "warnings": [],
    }
