"""Blender writer for Source Protocol v5 composite resources."""

from __future__ import annotations

import os
from pathlib import Path

import bpy
from mathutils import Matrix

from ..core.canonical import validate_resource_name
from ..core.canonical_json import canonical_json_bytes, parse_json
from ..core.composites import (
    composite_json_bytes,
    iter_resource_references,
    parse_composite,
    read_composite_file,
    validate_composite_cycles,
)
from ..core.model import Composite, CompositeTransform, Node
from ..core.payload_publish_v2 import atomic_publish_bytes
from ..core.transforms import (
    blender_to_ue_transform,
    matrix_reconstructs_as_float32_trs,
)
from ..core.validate import MHValidationError
from .export_fbx import _dagor_lod_structure
from .resource_markers import (
    COLLECTION_KIND_KEY,
    COLLECTION_RESOURCE_KEY,
    stamp_resource_collection,
)

__all__ = [
    "COLLECTION_KIND_KEY",
    "COLLECTION_RESOURCE_KEY",
    "NODE_KIND_KEY",
    "NODE_NAME_KEY",
    "NODE_RESOURCE_KEY",
    "UNRESOLVED_PLACEMENT_KEY",
    "export_composite_collection",
]


NODE_KIND_KEY = "mh_composite_kind"
NODE_RESOURCE_KEY = "mh_composite_resource"
NODE_NAME_KEY = "mh_composite_name"
UNRESOLVED_PLACEMENT_KEY = "mh_unresolved_placement"
_IMPORTED_TRANSFORM_KEY = "mh_imported_source_transform"
_IMPORTED_MATRIX_KEY = "mh_imported_local_matrix"


def _matrix_signature(matrix) -> str:
    return "|".join(
        float(matrix[row][column]).hex()
        for row in range(4)
        for column in range(4)
    )


def _stamp_imported_transform(obj, transform: CompositeTransform) -> None:
    obj[_IMPORTED_TRANSFORM_KEY] = canonical_json_bytes(
        transform.disk_dict()).decode("utf-8")
    obj[_IMPORTED_MATRIX_KEY] = _matrix_signature(obj.matrix_local)


def _stored_imported_transform(obj) -> CompositeTransform | None:
    encoded = obj.get(_IMPORTED_TRANSFORM_KEY)
    snapshot = obj.get(_IMPORTED_MATRIX_KEY)
    if (not isinstance(encoded, str) or not isinstance(snapshot, str)
            or snapshot != _matrix_signature(obj.matrix_local)):
        return None
    try:
        document = parse_json(encoded)
        if (not isinstance(document, dict)
                or set(document) != {
                    "translation_cm", "rotation_quat", "scale"}):
            return None
        return CompositeTransform(
            tuple(document["translation_cm"]),
            tuple(document["rotation_quat"]),
            tuple(document["scale"]),
        )
    except (KeyError, TypeError, ValueError):
        return None


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


def _collection_resource_name(collection) -> str:
    marker = collection.get(COLLECTION_RESOURCE_KEY)
    if isinstance(marker, str) and marker:
        name = marker
    elif collection.name.endswith(".composite"):
        name = collection.name[:-len(".composite")]
    else:
        name = collection.name
    validate_resource_name(name)
    return name


def _resolve_target(root: Path, output: Path, name: str) -> Path:
    matches = []
    for path in root.rglob("*"):
        if not path.is_file() or path.suffix.lower() != ".composite":
            continue
        if path.stem.casefold() != name:
            continue
        if path.suffix != ".composite" or path.stem != name:
            raise MHValidationError(
                "MH_E_NONCANONICAL_RESOURCE_NAME", [str(path)],
                "composite filename must use the exact lowercase logical "
                "name and .composite suffix")
        matches.append(path.resolve(strict=False))
    matches.sort(key=lambda row: str(row).replace("\\", "/"))
    if len(matches) > 1:
        raise MHValidationError(
            "MH_E_AMBIGUOUS_RESOURCE_NAME", [name, *(str(row) for row in matches)],
            "multiple composite resources share this logical name")
    return matches[0] if matches else output / f"{name}.composite"


def _resolve_dependency(
        root: Path, name: str, extension: str, *, allow_missing=False
) -> Path | None:
    """Resolve one source ResourceKey by exact filename identity."""
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
            "MH_E_AMBIGUOUS_RESOURCE_NAME",
            [name, *(str(row) for row in matches)],
            f"multiple resources resolve as '{expected}'")
    if not matches:
        if allow_missing:
            return None
        raise MHValidationError(
            "MH_E_UNRESOLVED_COMPOSITE_REFERENCE", [name],
            f"required source resource '{expected}' was not found")
    return matches[0]


def _preflight_dependencies(root: Path, candidate: Composite) -> None:
    """Resolve mesh/composite closure and reject candidate graph cycles."""
    documents = {candidate.name: candidate}

    def load_composite(name: str) -> None:
        if name in documents:
            return
        source = _resolve_dependency(
            root, name, ".composite", allow_missing=True)
        if source is None:
            documents[name] = Composite(name)
            return
        document = read_composite_file(source)
        documents[name] = document
        for dependency in iter_resource_references(
                document, kind="composite"):
            load_composite(dependency)

    for dependency in iter_resource_references(candidate, kind="composite"):
        load_composite(dependency)
    validate_composite_cycles(candidate.name, documents)

    # Actor tokens are intentionally lossless in Blender and resolve only in
    # UE.  Missing mesh/composite placements are also valid authoring state;
    # ambiguous same-kind identities remain fail-closed above.
    for document in documents.values():
        for dependency in iter_resource_references(document, kind="mesh"):
            _resolve_dependency(
                root, dependency, ".mesh.fbx", allow_missing=True)


def _collection_instance_identity(instance) -> tuple[str, str]:
    """Infer one resource placement without asking the artist to restate it."""
    marker_kind = instance.get(COLLECTION_KIND_KEY)
    marker_resource = instance.get(COLLECTION_RESOURCE_KEY)
    if marker_kind is not None or marker_resource is not None:
        if marker_kind not in {"mesh", "composite"}:
            raise MHValidationError(
                "MH_E_COMPOSITE_GRAMMAR", [instance.name],
                f"instance collection has invalid {COLLECTION_KIND_KEY}="
                f"{marker_kind!r}")
        if not isinstance(marker_resource, str) or not marker_resource:
            raise MHValidationError(
                "MH_E_COMPOSITE_GRAMMAR", [instance.name],
                f"instance collection is missing {COLLECTION_RESOURCE_KEY}")
        validate_resource_name(marker_resource)
        return marker_kind, marker_resource

    if instance.name.endswith(".composite"):
        resource = instance.name[:-len(".composite")]
        validate_resource_name(resource)
        return "composite", resource

    lod_structure = _dagor_lod_structure(instance)
    if lod_structure is not None:
        return "mesh", lod_structure["resource_name"]

    if any(obj.type == "MESH" for obj in instance.all_objects):
        validate_resource_name(instance.name)
        return "mesh", instance.name

    raise MHValidationError(
        "MH_E_COMPOSITE_GRAMMAR", [instance.name],
        "collection instance is not a known mesh/composite definition: "
        "export or import that resource once so its collection identity is "
        "established automatically")


def _node_kind_and_resource(obj) -> tuple[str, str | None]:
    explicit_kind = obj.get(NODE_KIND_KEY)
    explicit_resource = obj.get(NODE_RESOURCE_KEY)
    instance = getattr(obj, "instance_collection", None)
    instance_kind = None
    instance_resource = None
    if instance is not None:
        instance_kind, instance_resource = _collection_instance_identity(instance)

    kind = explicit_kind or instance_kind
    if kind is None and obj.type == "EMPTY" and instance is None:
        kind = "group"
    if kind not in {"mesh", "actor", "composite", "group"}:
        instance_name = getattr(instance, "name", None)
        raise MHValidationError(
            "MH_E_COMPOSITE_GRAMMAR", [obj.name],
            f"placement object {obj.name!r} (type={obj.type!r}, "
            f"instance_collection={instance_name!r}) has "
            f"{NODE_KIND_KEY}={explicit_kind!r} and inherited "
            f"kind={instance_kind!r}; Composite collections accept collection "
            "instances as resource placements and plain Empty objects as "
            "groups")

    resource = explicit_resource or instance_resource
    if kind == "group":
        if resource not in {None, ""}:
            raise MHValidationError(
                "MH_E_COMPOSITE_GRAMMAR", [obj.name],
                "group placement cannot carry a resource")
        return kind, None
    if not isinstance(resource, str) or not resource:
        raise MHValidationError(
            "MH_E_COMPOSITE_GRAMMAR", [obj.name],
            f"{kind} placement object {obj.name!r} requires a logical "
            f"resource token in {NODE_RESOURCE_KEY}; current value is "
            f"{resource!r}")
    return kind, resource


def _object_transform(obj):
    world_translation, world_rotation, world_scale = obj.matrix_world.decompose()
    reconstructed_world = Matrix.LocRotScale(
        world_translation, world_rotation, world_scale)
    if not matrix_reconstructs_as_float32_trs(
            obj.matrix_world, reconstructed_world):
        subjects = [obj.name]
        if obj.parent is not None:
            subjects.insert(0, obj.parent.name)
        raise MHValidationError(
            "MH_E_UNREPRESENTABLE_TRANSFORM", subjects,
            f"composite node {obj.name!r} world matrix contains shear or "
            "cannot round-trip as float32 T/R/S")

    stored = _stored_imported_transform(obj)
    if stored is not None:
        return stored
    translation, rotation, scale = obj.matrix_local.decompose()
    recomposed = Matrix.LocRotScale(translation, rotation, scale)
    if not matrix_reconstructs_as_float32_trs(obj.matrix_local, recomposed):
        subjects = [obj.name]
        if obj.parent is not None:
            subjects.insert(0, obj.parent.name)
        raise MHValidationError(
            "MH_E_UNREPRESENTABLE_TRANSFORM", subjects,
            f"composite node {obj.name!r} parent-local matrix contains shear "
            "or cannot round-trip as float32 T/R/S")
    return blender_to_ue_transform(
        tuple(float(value) for value in translation),
        (float(rotation.x), float(rotation.y), float(rotation.z),
         float(rotation.w)),
        tuple(float(value) for value in scale),
    )


def _extract_composite(collection) -> Composite:
    name = _collection_resource_name(collection)
    ordered = list(collection.objects)
    identities = {obj.as_pointer() for obj in ordered}
    children = {obj.as_pointer(): [] for obj in ordered}
    roots = []
    for obj in ordered:
        parent = obj.parent
        if parent is None:
            roots.append(obj)
            continue
        if parent.as_pointer() not in identities:
            raise MHValidationError(
                "MH_E_PARENT_OUTSIDE_RESOURCE", [obj.name, parent.name],
                "composite placement parent is outside the selected collection")
        children[parent.as_pointer()].append(obj)

    def build(obj) -> Node:
        kind, resource = _node_kind_and_resource(obj)
        display_name = obj.get(NODE_NAME_KEY)
        if display_name == "":
            display_name = None
        if display_name is not None and not isinstance(display_name, str):
            raise MHValidationError(
                "MH_E_COMPOSITE_GRAMMAR", [obj.name],
                "display-only composite node name must be a string")
        return Node(
            kind=kind,
            resource=resource,
            name=display_name,
            transform=_object_transform(obj),
            children=[build(child) for child in children[obj.as_pointer()]],
        )

    return Composite(name=name, nodes=[build(obj) for obj in roots])


def export_composite_collection(collection, output_dir, *, source_root) -> dict:
    """Publish one complete composite through sibling tmp/read-back/replace."""
    if collection is None:
        raise ValueError("collection is required")
    root = _resolved_root(source_root)
    output = Path(bpy.path.abspath(os.fspath(output_dir))).resolve(strict=False)
    if not _inside(root, output):
        raise ValueError("Composite output folder must be inside Project Source Root")

    resource = _extract_composite(collection)
    payload = composite_json_bytes(resource)
    target = _resolve_target(root, output, resource.name)
    if target.exists() and target.is_dir():
        raise ValueError(f"Composite target exists as a directory: {target}")
    _preflight_dependencies(root, resource)

    def validate_read_back(read_back: bytes) -> None:
        decoded = parse_composite(read_back, name=resource.name)
        if composite_json_bytes(decoded) != payload:
            raise RuntimeError(
                "MH_E_COMPOSITE_GRAMMAR: staged composite failed canonical "
                "read-back")

    receipt = atomic_publish_bytes(
        target,
        payload,
        source_root=root,
        read_back_validator=validate_read_back,
    )
    stamp_resource_collection(collection, "composite", resource.name)
    return {
        "ok": True,
        "filepath": str(target),
        "resource_name": resource.name,
        "nodes": len(list(collection.objects)),
        "bytes": receipt["bytes"],
        "written": True,
    }
