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
from dataclasses import dataclass
from pathlib import Path
from pathlib import PurePosixPath

import bpy
from mathutils import Matrix, Quaternion

from ..core.canonical import validate_resource_name
from ..core.dagor_composites import (
    DagorComposite,
    DagorInclude,
    DagorNode,
    iter_resource_tokens,
    parse_dagor_placement_include,
    read_dagor_composite,
)
from ..core.model import Composite, Node, PlacementProfile, RandomOption
from ..core.payload_publish_v2 import atomic_publish_bytes
from ..core.placements import (
    parse_placement_profile,
    placement_json_bytes,
    read_placement_file,
)
from ..core.transforms import (
    blender_to_ue_transform,
    matrix_reconstructs_as_float32_trs,
    ue_to_blender_transform,
)
from ..core.validate import MHValidationError
from .import_fbx import LOAD_MODE_FULL_LOD
from .resource_markers import DEFINITION_REUSE
from .import_composite import materialize_composite_documents

__all__ = [
    "convert_dag4blend_collection",
    "convert_dag4blend_collection_closure",
    "convert_dagor_composite",
    "DagorConversionBundle",
    "import_dag4blend_composite_collection",
    "import_dagor_composite_file",
    "load_dagor_composite_bundle",
    "load_dagor_composite_documents",
]


_DAGOR_TYPE_TO_KIND = {
    "composit": "composite",
    "rendinst": "mesh",
    "prefab": "mesh",
    "gameobj": "actor",
}
_MISSING = object()


@dataclass(frozen=True)
class DagorProfileSource:
    profile: PlacementProfile
    include_path: Path
    canonical_bytes: bytes


@dataclass(frozen=True)
class DagorConversionBundle:
    documents: dict[str, Composite]
    profiles: dict[str, DagorProfileSource]
    root_name: str

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


def _include_profile_name(include: DagorInclude) -> str:
    normalized_separators = include.path.replace("\\", "/")
    filename = PurePosixPath(normalized_separators).name
    if not filename or filename in {".", ".."}:
        _raise(
            "MH_E_NONCANONICAL_RESOURCE_NAME",
            [include.path, include.provenance.render()],
            "Dagor include requires a file whose stem is the placement "
            "profile identity",
        )
    name = PurePosixPath(filename).stem
    try:
        validate_resource_name(name)
    except (TypeError, ValueError) as exc:
        _raise(
            "MH_E_NONCANONICAL_RESOURCE_NAME",
            [include.path, include.provenance.render()],
            "Dagor include stem must already match [a-z0-9_]+ exactly; "
            "normalization is forbidden",
        )
    return name


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
        profile=(
            _include_profile_name(node.include)
            if node.include is not None else None),
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


def _iter_dagor_nodes(nodes):
    for node in nodes:
        yield node
        yield from _iter_dagor_nodes(node.children)


def _resolve_dagor_include(
        root: Path, composite_path: Path, include: DagorInclude
) -> tuple[str, Path, PlacementProfile, bytes]:
    raw_path = Path(include.path.replace("/", os.sep))
    candidate = (
        raw_path if raw_path.is_absolute()
        else composite_path.parent / raw_path)
    try:
        physical = candidate.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE",
            [include.path, include.provenance.render(), candidate],
            f"Dagor placement include cannot be resolved: {exc}",
        )
    if not physical.is_file() or not _inside(root, physical):
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE",
            [include.path, include.provenance.render(), physical, root],
            "Dagor placement include must be a file inside Project Source Root",
        )
    try:
        name = _include_profile_name(include)
    except MHValidationError as exc:
        _raise(
            "MH_E_NONCANONICAL_RESOURCE_NAME",
            [include.path, physical, include.provenance.render()],
            f"Dagor include stem is noncanonical: {exc}")
    try:
        profile = parse_dagor_placement_include(
            physical.read_bytes(), source=str(physical), name=name)
        canonical = placement_json_bytes(profile)
    except OSError as exc:
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE", [physical],
            f"Dagor placement include cannot be read: {exc}")
    return name, physical, profile, canonical


def load_dagor_composite_bundle(
        filepath, *, source_root) -> DagorConversionBundle:
    """Load direct closure plus every losslessly converted node include."""

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
    profiles: dict[str, DagorProfileSource] = {}

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
        for node in _iter_dagor_nodes(graph.nodes):
            if node.include is None:
                continue
            profile_name, include_path, profile, canonical = (
                _resolve_dagor_include(
                    root, dependency_path, node.include))
            existing = profiles.get(profile_name)
            if existing is not None:
                if existing.canonical_bytes != canonical:
                    _raise(
                        "MH_E_AMBIGUOUS_RESOURCE_NAME",
                        [profile_name, existing.include_path, include_path],
                        "Dagor includes with the same placement-profile "
                        "identity have different canonical bytes",
                    )
            else:
                profiles[profile_name] = DagorProfileSource(
                    profile, include_path, canonical)
        document = convert_dagor_composite(graph)
        documents[name] = document
        for token in iter_resource_tokens(graph):
            if token.kind == "composite":
                load(token.name)

    load(root_name, path)
    return DagorConversionBundle(documents, profiles, root_name)


def load_dagor_composite_documents(filepath, *, source_root) -> dict[str, Composite]:
    """Load every existing composite dependency through every random option."""

    return load_dagor_composite_bundle(
        filepath, source_root=source_root).documents


def _scan_placement_candidates(root: Path, name: str) -> list[Path]:
    expected = f"{name}.placement"
    matches: dict[str, Path] = {}
    for candidate in root.rglob("*"):
        if not candidate.is_file() or candidate.name.casefold() != expected.casefold():
            continue
        if candidate.name != expected:
            _raise(
                "MH_E_NONCANONICAL_RESOURCE_NAME", [candidate],
                f"placement filename must be exactly {expected!r}")
        physical = candidate.resolve(strict=True)
        if not _inside(root, physical):
            _raise(
                "MH_E_INVALID_RESOURCE_SOURCE", [candidate, physical],
                "placement source resolves outside Project Source Root")
        matches[os.path.normcase(str(physical))] = physical
    return sorted(matches.values(), key=lambda value: str(value).replace("\\", "/"))


def _preflight_profile_publication(
        bundle: DagorConversionBundle, root: Path, output: Path):
    plans = []
    for name, source in bundle.profiles.items():
        matches = _scan_placement_candidates(root, name)
        if len(matches) > 1:
            _raise(
                "MH_E_AMBIGUOUS_RESOURCE_NAME",
                [name, *(str(path) for path in matches), source.include_path],
                "multiple physical placement profiles share one logical name")
        if matches:
            existing_path = matches[0]
            raw = existing_path.read_bytes()
            try:
                canonical = placement_json_bytes(
                    read_placement_file(existing_path))
            except ValueError as exc:
                _raise(
                    getattr(exc, "code", None)
                    or "MH_E_PLACEMENT_PROFILE_GRAMMAR",
                    [existing_path, source.include_path],
                    f"existing placement profile is invalid: {exc}")
            if raw != canonical:
                _raise(
                    "MH_E_PLACEMENT_PROFILE_GRAMMAR",
                    [existing_path, source.include_path],
                    "existing placement profile is not exact canonical bytes; "
                    "Dagor conversion never normalizes it")
            if raw != source.canonical_bytes:
                _raise(
                    "MH_E_AMBIGUOUS_RESOURCE_NAME",
                    [name, existing_path, source.include_path],
                    "existing placement profile and Dagor include differ; "
                    "overwrite/winner selection is forbidden")
            plans.append((name, source, existing_path, False))
            continue
        target = output / f"{name}.placement"
        if target.exists():
            _raise(
                "MH_E_INVALID_RESOURCE_SOURCE", [target, source.include_path],
                "placement target exists but is not a regular candidate file")
        plans.append((name, source, target, True))
    return plans


def _publish_profiles(plans, root: Path):
    # Revalidate the whole identity set at the final transaction edge before
    # the first replace.  This covers reuse races as well as new-target races;
    # per-target guards below repeat the absence check under the payload lock.
    for name, source, target, should_write in plans:
        matches = _scan_placement_candidates(root, name)
        if should_write:
            if matches:
                _raise(
                    "MH_E_AMBIGUOUS_RESOURCE_NAME",
                    [name, *(str(path) for path in matches),
                     source.include_path],
                    "placement identity changed after preflight; refusing "
                    "race publication")
            continue
        if len(matches) != 1 or matches[0] != target:
            _raise(
                "MH_E_AMBIGUOUS_RESOURCE_NAME",
                [name, target, *(str(path) for path in matches),
                 source.include_path],
                "reused placement identity changed after preflight")
        raw = target.read_bytes()
        try:
            canonical = placement_json_bytes(read_placement_file(target))
        except ValueError as exc:
            _raise(
                getattr(exc, "code", None)
                or "MH_E_PLACEMENT_PROFILE_GRAMMAR",
                [target, source.include_path],
                f"reused placement profile became invalid: {exc}")
        if raw != canonical:
            _raise(
                "MH_E_PLACEMENT_PROFILE_GRAMMAR",
                [target, source.include_path],
                "reused placement profile ceased to be exact canonical bytes")
        if raw != source.canonical_bytes:
            _raise(
                "MH_E_AMBIGUOUS_RESOURCE_NAME",
                [name, target, source.include_path],
                "reused placement profile diverged after preflight")

    reports = []
    for name, source, target, should_write in plans:
        if not should_write:
            reports.append({
                "name": name,
                "filepath": str(target),
                "include_path": str(source.include_path),
                "written": False,
                "reused": True,
            })
            continue

        def guard(profile_name=name):
            raced = _scan_placement_candidates(root, profile_name)
            if raced:
                _raise(
                    "MH_E_AMBIGUOUS_RESOURCE_NAME",
                    [profile_name, *(str(path) for path in raced),
                     source.include_path],
                    "placement identity changed after preflight; refusing "
                    "race overwrite")

        def validate_read_back(payload, profile_name=name):
            decoded = parse_placement_profile(payload, name=profile_name)
            if placement_json_bytes(decoded) != source.canonical_bytes:
                _raise(
                    "MH_E_PLACEMENT_PROFILE_GRAMMAR",
                    [profile_name, source.include_path],
                    "staged placement profile failed canonical read-back")

        receipt = atomic_publish_bytes(
            target,
            source.canonical_bytes,
            source_root=root,
            read_back_validator=validate_read_back,
            pre_replace_guard=guard,
        )
        reports.append({
            "name": name,
            "filepath": str(target),
            "include_path": str(source.include_path),
            "bytes": receipt["bytes"],
            "written": True,
            "reused": False,
        })
    return reports


def import_dagor_composite_file(
        filepath, *, source_root, output_dir=None, resource_overrides=None,
        load_mode=LOAD_MODE_FULL_LOD,
        definition_policy=DEFINITION_REUSE) -> dict:
    """Convert and transactionally materialize one direct Dagor closure."""

    path = Path(bpy.path.abspath(os.fspath(filepath))).resolve(strict=True)
    root = _resolved_root(source_root)
    bundle = load_dagor_composite_bundle(path, source_root=root)
    plans = []
    if bundle.profiles:
        if output_dir is None:
            _raise(
                "MH_E_INVALID_RESOURCE_SOURCE",
                [path, *(source.include_path
                         for source in bundle.profiles.values())],
                "Dagor composites with include-derived placement profiles "
                "require an explicit output_dir; implicit source-root writes "
                "and hidden Blender authority are forbidden",
            )
        output = Path(bpy.path.abspath(os.fspath(output_dir))).resolve(strict=False)
        if not output.is_dir() or not _inside(root, output):
            _raise(
                "MH_E_INVALID_RESOURCE_SOURCE", [output, root],
                "Dagor placement output_dir must be an existing directory "
                "inside Project Source Root")
        plans = _preflight_profile_publication(bundle, root, output)

    def publish_before_commit(context):
        reports = []
        context["transaction"].add_finalize(
            lambda: reports.extend(_publish_profiles(plans, root)))
        return reports

    report = materialize_composite_documents(
        bundle.documents,
        root_name=bundle.root_name,
        source_root=source_root,
        filepath=path,
        resource_overrides=resource_overrides,
        load_mode=load_mode,
        definition_policy=definition_policy,
        before_commit=publish_before_commit if plans else None,
        preloaded_profiles=frozenset(bundle.profiles),
    )
    report["placement_profiles"] = report["before_commit_result"] or []
    return report


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


_DAG4BLEND_PLACEMENT_KEYS = frozenset({
    "offset_x:p2", "offset_y:p2", "offset_z:p2",
    "rot_x:p2", "rot_y:p2", "rot_z:p2",
    "scale:p2", "yscale:p2", "include", "include:t",
})


def _mapping_keys(mapping):
    if mapping is None:
        return ()
    try:
        return tuple(key for key in mapping.keys() if isinstance(key, str))
    except (AttributeError, TypeError):
        return ()


def _dag4blend_profile(obj, provenance, *, option=False):
    """Admit only the normative typed carrier; never approximate p2 as tm."""

    carrier_keys = sorted({
        key
        for mapping in (getattr(obj, "dagorprops", None), obj)
        for key in _mapping_keys(mapping)
        if key.casefold() in _DAG4BLEND_PLACEMENT_KEYS
    })
    settings = getattr(obj, "mh4blend", None)
    profile = "" if settings is None else settings.profile
    if profile:
        try:
            validate_resource_name(profile)
        except (TypeError, ValueError) as exc:
            _raise(
                "MH_E_NONCANONICAL_RESOURCE_NAME",
                [provenance, repr(profile), *carrier_keys],
                "typed mh4blend.profile must match [a-z0-9_]+ exactly")
    if option and (profile or carrier_keys):
        _raise(
            "MH_E_COMPOSITE_GRAMMAR",
            [provenance, profile, *carrier_keys],
            "dag4blend random options cannot carry placement profiles")
    if carrier_keys and not profile:
        _raise(
            "MH_E_COMPOSITE_GRAMMAR",
            [provenance, *carrier_keys],
            "lossless dag4blend conversion refuses p2/include placement data "
            "without the exact typed mh4blend.profile authority; matrix_local "
            "is only a preview/base and cannot replace deviation semantics")
    return profile or None


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
        profile = _dag4blend_profile(obj, subject)
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
                _dag4blend_profile(option_obj, option_subject, option=True)
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
            profile=profile,
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


def import_dag4blend_composite_collection(
        collection, *, source_root=None, load_mode=LOAD_MODE_FULL_LOD,
        definition_policy=DEFINITION_REUSE) -> dict:
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
        source_root=source_root,
        filepath=None,
        resource_overrides=overrides,
        load_mode=load_mode,
        definition_policy=definition_policy,
    )
