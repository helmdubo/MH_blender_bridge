"""Lossless Dagor-to-MH composite conversion inside Blender.

Two sources are admitted by the V5-S3 contract: authoritative
``*.composit.blk`` files and an already imported dag4blend collection.  Both
are converted to the same Source Protocol v5 DTOs before the shared
transactional materializer is called.  Dagor helper collections are inspected
only to lift options; they never become MH authority.
"""

from __future__ import annotations

import math
import os
from pathlib import Path

import bpy
from mathutils import Matrix, Quaternion

from ..core.canonical import validate_resource_name
from ..core.dagor_composites import (
    DagorComposite,
    DagorNode,
    iter_resource_tokens,
    read_dagor_composite,
)
from ..core.model import Composite, Node, RandomOption
from ..core.transforms import (
    blender_to_ue_transform,
    matrix_reconstructs_as_float32_trs,
    ue_to_blender_transform,
)
from ..core.validate import MHValidationError
from .import_composite import materialize_composite_documents

__all__ = [
    "convert_dag4blend_collection",
    "convert_dag4blend_collection_closure",
    "convert_dagor_composite",
    "import_dag4blend_composite_collection",
    "import_dagor_composite_file",
    "load_dagor_composite_documents",
]


_DAGOR_TYPE_TO_KIND = {
    "composit": "composite",
    "rendinst": "mesh",
    "prefab": "mesh",
    "gameobj": "actor",
}
_MISSING = object()

# Dagor is Y-up and stores four columns of three values.  Conjugating by this
# axis swap produces Blender's Z-up local matrix while preserving reflections,
# non-uniform scale, and any shear for the admission predicate to inspect.
_DAGOR_TO_BLENDER = Matrix((
    (1.0, 0.0, 0.0, 0.0),
    (0.0, 0.0, 1.0, 0.0),
    (0.0, 1.0, 0.0, 0.0),
    (0.0, 0.0, 0.0, 1.0),
))


def _raise(code, subjects, message):
    raise MHValidationError(code, subjects, message)


def _canonical_resource_name(name, provenance) -> str:
    try:
        validate_resource_name(name)
    except (TypeError, ValueError) as exc:
        _raise(
            "MH_E_NONCANONICAL_RESOURCE_NAME",
            [name, provenance],
            "Dagor resource names must already be canonical; rename inference "
            "or repair is forbidden",
        )
    return name


def _validate_trs(matrix, subjects, boundary):
    try:
        translation, rotation, scale = matrix.decompose()
        reconstructed = Matrix.LocRotScale(translation, rotation, scale)
    except (ArithmeticError, TypeError, ValueError) as exc:
        _raise(
            "MH_E_UNREPRESENTABLE_TRANSFORM", subjects,
            f"{boundary} matrix cannot be decomposed as parent-local T/R/S: {exc}")
    if not matrix_reconstructs_as_float32_trs(matrix, reconstructed):
        _raise(
            "MH_E_UNREPRESENTABLE_TRANSFORM", subjects,
            f"{boundary} matrix contains shear or cannot round-trip within "
            "the owner-frozen 8-ULP float32 predicate",
        )
    return translation, rotation, scale


def _canonical_local_transform(matrix, subjects):
    translation, rotation, scale = _validate_trs(matrix, subjects, "local")
    if any(float(value) == 0.0 for value in scale):
        _raise(
            "MH_E_INVALID_SCALE", subjects,
            "Dagor parent-local scale components must be non-zero")
    try:
        transform = blender_to_ue_transform(
            tuple(float(value) for value in translation),
            (float(rotation.x), float(rotation.y), float(rotation.z),
             float(rotation.w)),
            tuple(float(value) for value in scale),
        )
    except (ArithmeticError, TypeError, ValueError) as exc:
        _raise(
            "MH_E_UNREPRESENTABLE_TRANSFORM", subjects,
            f"Dagor parent-local T/R/S cannot be represented as float32: {exc}")
    canonical_translation, canonical_rotation, canonical_scale = (
        ue_to_blender_transform(transform))
    x, y, z, w = canonical_rotation
    canonical_matrix = (
        Matrix.Translation(canonical_translation)
        @ Quaternion((w, x, y, z)).to_matrix().to_4x4()
        @ Matrix.Diagonal((*canonical_scale, 1.0))
    )
    if not matrix_reconstructs_as_float32_trs(matrix, canonical_matrix):
        _raise(
            "MH_E_UNREPRESENTABLE_TRANSFORM", subjects,
            "local Dagor transform changes beyond 8 ULP when narrowed to "
            "Source Protocol float32 T/R/S",
        )
    return transform, canonical_matrix


def _dagor_local_matrix(node: DagorNode) -> Matrix:
    if node.transform is None:
        return Matrix.Identity(4)
    first, second, third, translation = node.transform.columns
    dagor = Matrix((
        (first[0], second[0], third[0], translation[0]),
        (first[1], second[1], third[1], translation[1]),
        (first[2], second[2], third[2], translation[2]),
        (0.0, 0.0, 0.0, 1.0),
    ))
    return _DAGOR_TO_BLENDER @ dagor @ _DAGOR_TO_BLENDER


def _dagor_node_subject(node):
    if node.resource is not None:
        label = f"resource:{node.resource.name}"
    elif node.options:
        label = "random"
    else:
        label = "group"
    return f"{label} {node.provenance.render()}"


def _convert_dagor_node(node, parent_world, ancestor_subjects=()):
    subject = _dagor_node_subject(node)
    subjects = [*ancestor_subjects, subject]
    transform, local = _canonical_local_transform(
        _dagor_local_matrix(node), subjects)
    world = parent_world @ local
    _validate_trs(world, subjects, "composed world")

    resource = None
    if node.resource is not None:
        resource = _canonical_resource_name(
            node.resource.name, node.resource.provenance.render())
    options = [
        RandomOption(
            option.resource.kind,
            float(option.weight),
            _canonical_resource_name(
                option.resource.name, option.resource.provenance.render()),
        )
        for option in node.options
    ]
    children = [
        _convert_dagor_node(child, world, tuple(subjects))
        for child in node.children
    ]
    return Node(
        node.kind,
        transform=transform,
        resource=resource,
        options=options,
        children=children,
    )


def convert_dagor_composite(source: DagorComposite) -> Composite:
    """Convert a parsed Dagor graph to one v5 parent-local Composite DTO."""

    name = _canonical_resource_name(source.name, source.provenance.render())
    return Composite(name, [
        _convert_dagor_node(node, Matrix.Identity(4))
        for node in source.nodes
    ])


def _resolved_root(source_root) -> Path:
    if not isinstance(source_root, (str, os.PathLike)) or not str(source_root).strip():
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE", [repr(source_root)],
            "Dagor import requires a Project Source Root")
    root = Path(bpy.path.abspath(os.fspath(source_root))).resolve(strict=False)
    if not root.is_dir():
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE", [root],
            "Project Source Root does not exist")
    return root


def _inside(root: Path, path: Path) -> bool:
    try:
        return os.path.commonpath([
            os.path.normcase(str(root)), os.path.normcase(str(path)),
        ]) == os.path.normcase(str(root))
    except ValueError:
        return False


def _resolve_dagor_composite(root: Path, name: str) -> Path | None:
    name = _canonical_resource_name(name, root)
    expected = f"{name}.composit.blk"
    matches = {}
    for candidate in root.rglob("*"):
        if not candidate.is_file() or candidate.name.casefold() != expected.casefold():
            continue
        if candidate.name != expected:
            _raise(
                "MH_E_NONCANONICAL_RESOURCE_NAME", [candidate],
                f"Dagor source filename must be exactly {expected!r}")
        physical = candidate.resolve(strict=True)
        if not _inside(root, physical):
            _raise(
                "MH_E_INVALID_RESOURCE_SOURCE", [candidate, physical],
                "Dagor source resolves outside Project Source Root")
        matches[os.path.normcase(str(physical))] = physical
    if len(matches) > 1:
        _raise(
            "MH_E_AMBIGUOUS_RESOURCE_NAME", [name, *matches.values()],
            "multiple physical Dagor composites have the same logical name")
    return next(iter(matches.values()), None)


def load_dagor_composite_documents(filepath, *, source_root) -> dict[str, Composite]:
    """Load every existing composite dependency through every random option."""

    root = _resolved_root(source_root)
    path = Path(bpy.path.abspath(os.fspath(filepath))).resolve(strict=True)
    if not _inside(root, path):
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE", [path, root],
            "Dagor composite source must be inside Project Source Root")
    if not path.name.endswith(".composit.blk"):
        _raise(
            "MH_E_NONCANONICAL_RESOURCE_NAME", [path],
            "Dagor composite filename must end with exact .composit.blk")
    root_name = path.name.removesuffix(".composit.blk")
    _canonical_resource_name(root_name, path)
    resolved_root = _resolve_dagor_composite(root, root_name)
    if resolved_root is None or resolved_root != path:
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE", [path, resolved_root],
            "selected Dagor root does not match its unique physical source")

    documents: dict[str, Composite] = {}

    def load(name: str, explicit_path: Path | None = None):
        if name in documents:
            return
        dependency_path = explicit_path or _resolve_dagor_composite(root, name)
        if dependency_path is None:
            return
        graph = read_dagor_composite(dependency_path)
        if graph.name != name:
            _raise(
                "MH_E_NONCANONICAL_RESOURCE_NAME", [dependency_path, name],
                "Dagor filename identity differs from requested resource")
        document = convert_dagor_composite(graph)
        documents[name] = document
        for token in iter_resource_tokens(graph):
            if token.kind == "composite":
                load(token.name)

    load(root_name, path)
    return documents


def import_dagor_composite_file(
        filepath, *, source_root, resource_overrides=None) -> dict:
    """Convert and transactionally materialize one direct Dagor closure."""

    path = Path(bpy.path.abspath(os.fspath(filepath))).resolve(strict=True)
    documents = load_dagor_composite_documents(path, source_root=source_root)
    root_name = path.name.removesuffix(".composit.blk")
    return materialize_composite_documents(
        documents,
        root_name=root_name,
        source_root=source_root,
        filepath=path,
        resource_overrides=resource_overrides,
    )


def _mapping_value(mapping, key):
    if mapping is None:
        return _MISSING
    try:
        if key in mapping.keys():
            return mapping[key]
    except (AttributeError, KeyError, TypeError):
        pass
    try:
        if key in mapping:
            return mapping[key]
    except (KeyError, TypeError):
        pass
    return _MISSING


def _dagor_property(owner, key):
    value = _mapping_value(getattr(owner, "dagorprops", None), key)
    if value is not _MISSING:
        return value
    return _mapping_value(owner, key)


def _resource_identity(collection, owner, provenance):
    name = _mapping_value(collection, "name")
    type_name = _mapping_value(collection, "type")
    if type_name is _MISSING and owner is not None:
        type_name = _dagor_property(owner, "type:t")
    if name is _MISSING or not isinstance(name, str) or not name:
        _raise(
            "MH_E_COMPOSITE_GRAMMAR", [provenance, collection.name],
            "dag4blend resource collection requires explicit string 'name'")
    if type_name is _MISSING or not isinstance(type_name, str):
        _raise(
            "MH_E_COMPOSITE_GRAMMAR", [provenance, collection.name],
            "dag4blend resource requires explicit collection 'type' or type:t")
    kind = _DAGOR_TYPE_TO_KIND.get(type_name.casefold())
    if kind is None:
        _raise(
            "MH_E_UNSUPPORTED_NODE_KIND", [provenance, type_name],
            "unsupported explicit dag4blend resource type")
    return kind, _canonical_resource_name(name, provenance)


def _is_random_helper(obj) -> bool:
    helper = obj.instance_collection
    helper_shape = (
        helper is not None
        and helper.name.casefold().startswith("random.")
        and any(option.type == "EMPTY" for option in helper.objects)
    )
    marker = _dagor_property(obj, "type:t")
    marker_shape = isinstance(marker, str) and marker.casefold() == "random"
    return helper_shape or marker_shape


def _option_weight(option, provenance) -> float:
    value = _mapping_value(getattr(option, "dagorprops", None), "weight:r")
    if value is _MISSING:
        value = _mapping_value(option, "weight:r")
    if value is _MISSING:
        return 1.0
    if (isinstance(value, bool) or not isinstance(value, (int, float))
            or not math.isfinite(float(value)) or float(value) < 0.0):
        _raise(
            "MH_E_COMPOSITE_GRAMMAR", [provenance],
            "dag4blend random weight must be a finite number >= 0")
    return float(value)


def _register_override(overrides, key, collection, provenance):
    previous = overrides.get(key)
    if previous is not None and previous is not collection:
        _raise(
            "MH_E_AMBIGUOUS_RESOURCE_NAME",
            [provenance, previous.name, collection.name],
            f"dag4blend scene maps {key!r} to multiple collections",
        )
    overrides[key] = collection


def _dag4blend_local(obj, subjects):
    try:
        matrix = obj.matrix_local.copy()
    except (AttributeError, TypeError, ValueError) as exc:
        _raise(
            "MH_E_UNREPRESENTABLE_TRANSFORM", subjects,
            f"cannot read dag4blend parent-local matrix: {exc}")
    return _canonical_local_transform(matrix, subjects)


def convert_dag4blend_collection(collection):
    """Return ``(Composite, resource_overrides)`` from imported dag4blend data."""

    if collection is None or bpy.data.collections.get(collection.name) is not collection:
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE", [repr(collection)],
            "dag4blend source must be a live Blender collection")
    root_kind, root_name = _resource_identity(
        collection, None, f"collection:{collection.name}")
    if root_kind != "composite":
        _raise(
            "MH_E_RESOURCE_KIND_MISMATCH", [collection.name, root_kind],
            "dag4blend source collection must have explicit type 'composit'")

    ordered = list(collection.objects)
    identities = {obj.as_pointer() for obj in ordered}
    children = {obj.as_pointer(): [] for obj in ordered}
    roots = []
    for obj in ordered:
        if obj.type != "EMPTY":
            _raise(
                "MH_E_UNREPRESENTABLE_SCENE_OBJECT", [obj.name],
                "dag4blend composite nodes must be Empty objects")
        parent = obj.parent
        if parent is None:
            roots.append(obj)
        elif parent.as_pointer() not in identities:
            _raise(
                "MH_E_PARENT_OUTSIDE_RESOURCE", [obj.name, parent.name],
                "dag4blend node parent is outside the source collection")
        else:
            children[parent.as_pointer()].append(obj)

    overrides = {}

    def convert_object(obj, parent_world, ancestor_subjects=()):
        resource_label = ""
        instance = obj.instance_collection
        if instance is not None:
            candidate = _mapping_value(instance, "name")
            if isinstance(candidate, str) and candidate:
                resource_label = f"/resource:{candidate}"
        subject = (
            f"collection:{collection.name}/object:{obj.name}"
            f"{resource_label}")
        subjects = [*ancestor_subjects, subject]
        transform, local = _dag4blend_local(obj, subjects)
        world = parent_world @ local
        _validate_trs(world, subjects, "composed world")

        resource = None
        options = []
        if _is_random_helper(obj):
            kind = "random"
            helper = obj.instance_collection
            if helper is None:
                _raise(
                    "MH_E_COMPOSITE_GRAMMAR", [subject],
                    "random type:t marker requires an option helper collection")
            if helper.children:
                _raise(
                    "MH_E_COMPOSITE_GRAMMAR", [subject, helper.name],
                    "dag4blend random helper child collections are unsupported")
            total = 0.0
            for option_index, option_obj in enumerate(helper.objects):
                option_subject = f"{subject}/ent[{option_index}]:{option_obj.name}"
                if option_obj.type != "EMPTY":
                    _raise(
                        "MH_E_UNREPRESENTABLE_SCENE_OBJECT", [option_subject],
                        "dag4blend random options must be Empty objects")
                option_collection = option_obj.instance_collection
                if option_collection is None:
                    _raise(
                        "MH_E_COMPOSITE_GRAMMAR", [option_subject],
                        "dag4blend random option requires explicit resource collection")
                option_kind, option_name = _resource_identity(
                    option_collection, option_obj, option_subject)
                weight = _option_weight(option_obj, option_subject)
                try:
                    total = math.fsum((total, weight))
                except OverflowError:
                    _raise(
                        "MH_E_COMPOSITE_GRAMMAR", [subject],
                        "dag4blend random option total must be finite and positive")
                options.append(RandomOption(option_kind, weight, option_name))
                _register_override(
                    overrides, (option_kind, option_name),
                    option_collection, option_subject)
            if not options or not math.isfinite(total) or total <= 0.0:
                _raise(
                    "MH_E_COMPOSITE_GRAMMAR", [subject],
                    "dag4blend random node requires a finite positive option total")
        elif obj.instance_collection is None:
            kind = "group"
        else:
            kind, resource = _resource_identity(
                obj.instance_collection, obj, subject)
            _register_override(
                overrides, (kind, resource), obj.instance_collection, subject)

        converted_children = [
            convert_object(child, world, tuple(subjects))
            for child in children[obj.as_pointer()]
        ]
        return Node(
            kind,
            transform=transform,
            resource=resource,
            options=options,
            children=converted_children,
        )

    document = Composite(root_name, [
        convert_object(obj, Matrix.Identity(4)) for obj in roots
    ])
    return document, overrides


def convert_dag4blend_collection_closure(collection):
    """Recursively convert all explicit composite definitions and options.

    Composite resource collections become documents, never overrides.  Only
    Mesh definition collections cross into the shared materializer as explicit
    preloaded resources. Actor collections remain transport hints only; their
    lossless tokens receive canonical MH placeholder collections.
    """

    documents = {}
    composite_sources = {}
    resource_overrides = {}

    def load(source_collection):
        document, discovered = convert_dag4blend_collection(source_collection)
        previous = composite_sources.get(document.name)
        if previous is not None:
            if previous is not source_collection:
                _raise(
                    "MH_E_AMBIGUOUS_RESOURCE_NAME",
                    [document.name, previous.name, source_collection.name],
                    "multiple dag4blend composite definitions claim one "
                    "canonical resource name",
                )
            return
        # Publish visited state before descending so cycles terminate here and
        # are diagnosed by the shared Source Protocol cycle validator.
        composite_sources[document.name] = source_collection
        documents[document.name] = document
        for key, resource_collection in discovered.items():
            kind, name = key
            if kind == "composite":
                load(resource_collection)
            elif kind == "mesh":
                _register_override(
                    resource_overrides, key, resource_collection,
                    f"composite:{document.name}/{kind}:{name}")
            else:
                # Actor collections in a dag4blend working scene are transport
                # hints, not MH definition authority.  The explicit token is
                # retained in the DTO and the materializer creates the
                # canonical empty ACTOR_PLACEHOLDERS definition.
                if kind != "actor":
                    _raise(
                        "MH_E_UNSUPPORTED_NODE_KIND", [kind, name],
                        "unsupported dag4blend closure resource kind")

    load(collection)
    return documents, resource_overrides


def import_dag4blend_composite_collection(collection) -> dict:
    """Convert an existing dag4blend definition through the shared materializer."""

    documents, overrides = convert_dag4blend_collection_closure(collection)
    root_kind, root_name = _resource_identity(
        collection, None, f"collection:{collection.name}")
    if root_kind != "composite":
        _raise(
            "MH_E_RESOURCE_KIND_MISMATCH", [collection.name, root_kind],
            "dag4blend source collection must have explicit type 'composit'")
    return materialize_composite_documents(
        documents,
        root_name=root_name,
        source_root=None,
        filepath=None,
        resource_overrides=overrides,
    )
