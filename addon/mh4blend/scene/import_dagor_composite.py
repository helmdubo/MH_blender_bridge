"""Dagor BLK conversion and a read-only adapter of dag4blend scene data.

Two sources are admitted by the V5-S3 contract: authoritative
``*.composit.blk`` files and an already imported dag4blend collection.  Both
produce the same Source Protocol v5 DTOs. The scene route has partial
compatibility: it cannot reconstruct data dag4blend already discarded.
Direct export never materializes, adopts or relinks Blender datablocks.
The optional explicit scene conversion still uses the shared materializer.
"""

from __future__ import annotations

import math
import os
from dataclasses import dataclass
from pathlib import Path
from pathlib import PurePosixPath
import tempfile

import bpy
from mathutils import Matrix, Quaternion

from ..core.canonical import hash_hex, validate_resource_name
from ..core.composites import read_composite_file
from ..core.dagor_composites import (
    DagorComposite,
    DagorInclude,
    DagorNode,
    iter_resource_tokens,
    parse_dagor_placement_include,
    read_dagor_composite,
)
from ..core.model import (
    Composite,
    Node,
    PlacementProfile,
    PlacementRange,
    RandomOption,
)
from ..core.placement_publication import (
    PlacementPublicationRequest,
    plan_placement_publications,
    publish_placement_publications,
    stage_placement_publications,
)
from ..core.placements import parse_placement_profile, placement_json_bytes
from ..core.source_closure import ResourceKey
from ..core.source_inventory import scan_source_inventory
from ..core.transforms import (
    blender_to_ue_transform,
    matrix_reconstructs_as_float32_trs,
    ue_to_blender_transform,
)
from ..core.validate import MHValidationError
from .import_fbx import LOAD_MODE_FULL_LOD
from .resource_markers import DEFINITION_REUSE
from .import_composite import materialize_composite_documents
from .readonly_properties import existing_property_group as _existing_settings

__all__ = [
    "convert_dag4blend_collection",
    "convert_dag4blend_collection_closure",
    "convert_dagor_composite",
    "DagorConversionBundle",
    "import_dag4blend_composite_collection",
    "import_dagor_composite_file",
    "load_dagor_composite_bundle",
    "load_dagor_composite_documents",
    "publish_dag4blend_composite_collection",
]


_DAGOR_TYPE_TO_KIND = {
    "composit": "composite",
    "rendinst": "mesh",
    "prefab": "mesh",
    "gameobj": "actor",
}
_DAG4BLEND_TYPE_TO_KIND = {**_DAGOR_TYPE_TO_KIND, "gameobj": "gameobj"}
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


def _preflight_profile_publication(
        bundle: DagorConversionBundle, root: Path, output: Path):
    return plan_placement_publications((
        PlacementPublicationRequest(
            name=name,
            canonical_bytes=source.canonical_bytes,
            provenance=source.include_path,
        )
        for name, source in bundle.profiles.items()
    ), source_root=root, output_dir=output)


def _publish_profiles(plans, root: Path):
    # Stage the complete profile set before revalidating or replacing the
    # first authoritative file.  The Blender transaction owns this finalizer,
    # so any failure still rolls its materialized delta back as one unit.
    with tempfile.TemporaryDirectory(prefix="mh-placement-stage-") as directory:
        staged = stage_placement_publications(
            plans, staging_dir=directory, source_root=root)
        results = publish_placement_publications(staged, source_root=root)
    reports = []
    for result in results:
        row = {
            "name": result.name,
            "filepath": str(result.target),
            "include_path": str(result.provenance),
            "written": result.written,
            "reused": result.reused,
        }
        if result.byte_count is not None:
            row["bytes"] = result.byte_count
        reports.append(row)
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


def _dagor_settings(owner):
    # dagorprops is a dynamic key/value bag, not the MH typed carrier. Read
    # that same saved bag even when a .blend is opened without dag4blend RNA;
    # absence of its Python descriptor must not erase weights or p2 controls.
    if isinstance(owner, bpy.types.ID):
        value = owner.get("dagorprops")
        if value is not None and not hasattr(value, "keys"):
            _raise("MH_E_INVALID_RESOURCE_SOURCE", [owner.name, "dagorprops"],
                   "saved dagorprops must be a key/value mapping")
        return value
    return getattr(owner, "dagorprops", None)


def _dagor_property(owner, key):
    value = _mapping_value(_dagor_settings(owner), key)
    if value is not _MISSING:
        return value
    return _mapping_value(owner, key)


_DAG4BLEND_PLACEMENT_KEYS = frozenset({
    "offset_x:p2", "offset_y:p2", "offset_z:p2",
    "rot_x:p2", "rot_y:p2", "rot_z:p2",
    "scale:p2", "yscale:p2", "include", "include:t",
})

# dag4blend keeps these four declarations; owner doc13 §7 maps them onto the
# two ratified node carriers. Absence stays absence: it is never evidence that
# the authoritative BLK said zero.
_PLACE_TYPE_KEY = "place_type:i"
_PLACE_ON_COLLISION_KEY = "placeoncollision:b"
_IGNORE_PARENT_APPEARANCE_KEY = "ignoreparentinstseed:b"
_USE_PARENT_APPEARANCE_KEY = "useparentinstseed:b"
_DAG4BLEND_METADATA_KEYS = frozenset({
    _PLACE_TYPE_KEY, _PLACE_ON_COLLISION_KEY,
    _IGNORE_PARENT_APPEARANCE_KEY, _USE_PARENT_APPEARANCE_KEY,
})


def _mapping_keys(mapping):
    if mapping is None:
        return ()
    try:
        return tuple(key for key in mapping.keys() if isinstance(key, str))
    except (AttributeError, TypeError):
        return ()


def _dag4blend_metadata_claims(obj):
    """Collect the declarations dag4blend preserved, dagorprops winning."""

    claims = {}
    for mapping in (obj, _dagor_settings(obj)):
        for key in _mapping_keys(mapping):
            folded = key.casefold()
            if folded not in _DAG4BLEND_METADATA_KEYS:
                continue
            value = _mapping_value(mapping, key)
            if value is not _MISSING:
                claims[folded] = (key, value)
    return claims


def _dag4blend_placement_claims(obj):
    """Collect p2/include declarations with saved dagorprops taking priority."""

    claims = {}
    for mapping in (obj, _dagor_settings(obj)):
        for key in _mapping_keys(mapping):
            folded = key.casefold()
            if folded not in _DAG4BLEND_PLACEMENT_KEYS:
                continue
            value = _mapping_value(mapping, key)
            if value is not _MISSING:
                claims[folded] = (key, value)
    return claims


def _dagor_flag(key, value, subjects):
    if isinstance(value, bool):
        return value
    if isinstance(value, int) and value in (0, 1):
        return bool(value)
    _raise(
        "MH_E_COMPOSITE_GRAMMAR", [*subjects, key],
        f"dag4blend {key} must be a boolean declaration")


def _dag4blend_source_metadata(obj, provenance, node_path, *, option=False):
    """Map preserved dag4blend declarations onto the two node carriers."""

    claims = _dag4blend_metadata_claims(obj)
    if option and claims:
        # Node metadata has no random-option wire position; dropping it here
        # would be exactly the silent loss this carrier exists to prevent.
        _raise(
            "MH_E_COMPOSITE_GRAMMAR",
            [provenance, *sorted(key for key, _value in claims.values())],
            "dag4blend random options cannot carry node place_type or "
            f"appearance_seed_boundary metadata at NodePath {node_path}")

    place_type = None
    declared = claims.get(_PLACE_TYPE_KEY)
    if declared is not None:
        key, value = declared
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            _raise(
                "MH_E_COMPOSITE_GRAMMAR", [provenance, key],
                f"dag4blend {key} must be a non-negative integer at NodePath "
                f"{node_path}; unknown non-negative values pass as provenance")
        place_type = int(value)
    collision = claims.get(_PLACE_ON_COLLISION_KEY)
    if collision is not None:
        key, value = collision
        # An explicit place_type always outranks the legacy boolean shorthand.
        if _dagor_flag(key, value, [provenance]) and place_type is None:
            place_type = 1

    boundaries = {}
    ignored = claims.get(_IGNORE_PARENT_APPEARANCE_KEY)
    if ignored is not None:
        key, value = ignored
        boundaries[_dagor_flag(key, value, [provenance])] = key
    inherited = claims.get(_USE_PARENT_APPEARANCE_KEY)
    if inherited is not None:
        key, value = inherited
        if not _dagor_flag(key, value, [provenance]):
            # Owner ratified this declaration only as an explicit false. A
            # negated form has no ratified meaning; guessing one would invent
            # semantics, so it stays fail-closed.
            _raise(
                "MH_E_COMPOSITE_GRAMMAR", [provenance, key],
                f"dag4blend {key} is admitted only as an explicit false "
                f"appearance_seed_boundary at NodePath {node_path}")
        boundaries[False] = key
    if len(boundaries) > 1:
        _raise(
            "MH_E_COMPOSITE_GRAMMAR", [provenance, *sorted(boundaries.values())],
            "dag4blend appearance_seed_boundary declarations contradict each "
            f"other at NodePath {node_path}")
    return place_type, next(iter(boundaries), False)


def _dag4blend_p2_range(key, value, provenance):
    try:
        raw = value.to_list() if hasattr(value, "to_list") else tuple(value)
    except (TypeError, ValueError) as exc:
        _raise(
            "MH_E_PLACEMENT_PROFILE_GRAMMAR", [provenance, key],
            f"dag4blend {key} must be exactly [base, deviation]: {exc}")
    if len(raw) != 2:
        _raise(
            "MH_E_PLACEMENT_PROFILE_GRAMMAR", [provenance, key],
            f"dag4blend {key} must be exactly [base, deviation]")
    if any(isinstance(item, bool) or not isinstance(item, (int, float))
           for item in raw):
        _raise(
            "MH_E_PLACEMENT_PROFILE_GRAMMAR", [provenance, key],
            f"dag4blend {key} base and deviation must be numbers")
    return PlacementRange(float(raw[0]), float(raw[1]))


def _dag4blend_generated_profile(claims, provenance):
    """Build one canonical content-addressed profile from inline Dagor p2."""

    zero = PlacementRange(0.0, 0.0)

    def triple(prefix):
        keys = tuple(f"{prefix}_{axis}:p2" for axis in "xyz")
        if not any(key in claims for key in keys):
            return None
        return tuple(
            (_dag4blend_p2_range(*claims[key], provenance)
             if key in claims else zero)
            for key in keys
        )

    def scalar(key):
        return (_dag4blend_p2_range(*claims[key], provenance)
                if key in claims else None)

    provisional = PlacementProfile(
        "dagor_inline_p2",
        offset_cm=triple("offset"),
        rotation_deg=triple("rot"),
        uniform_scale=scalar("scale:p2"),
        vertical_scale=scalar("yscale:p2"),
    )
    try:
        canonical = placement_json_bytes(provisional)
        suffix = hash_hex(canonical).removeprefix("xxh3:")
        name = f"dagor_p2_{suffix}"
        parse_placement_profile(canonical, name=name)
    except (RuntimeError, TypeError, ValueError) as exc:
        code = getattr(exc, "code", None) or "MH_E_PLACEMENT_PROFILE_GRAMMAR"
        _raise(
            code, [provenance, *(key for key, _value in claims.values())],
            f"inline dag4blend p2 cannot become a placement profile: {exc}")
    return name, canonical


def _dag4blend_profile(
        obj, provenance, *, option=False, generated_profiles=None):
    """Resolve typed authority or derive a profile for direct publication."""

    claims = _dag4blend_placement_claims(obj)
    carrier_keys = sorted(key for key, _value in claims.values())
    settings = _existing_settings(obj, "mh4blend")
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
    if profile:
        return profile, False

    include_keys = [
        key for folded, (key, _value) in claims.items()
        if folded in {"include", "include:t"}
    ]
    if include_keys:
        _raise(
            "MH_E_COMPOSITE_GRAMMAR",
            [provenance, *include_keys],
            "dag4blend scene include placement data requires the exact typed "
            "mh4blend.profile authority; resolving arbitrary include paths "
            "from a saved scene is forbidden")

    p2_claims = {
        folded: claim for folded, claim in claims.items()
        if folded.endswith(":p2")
    }
    if not p2_claims:
        return None, False
    if generated_profiles is None:
        _raise(
            "MH_E_COMPOSITE_GRAMMAR",
            [provenance, *(key for key, _value in p2_claims.values())],
            "dag4blend scene adapter refuses inline p2 outside direct "
            "composite publication; the derived .placement must be committed "
            "in the same batch; parameters: "
            + ", ".join(sorted(
                key for key, _value in p2_claims.values())))

    name, canonical = _dag4blend_generated_profile(
        p2_claims, provenance)
    existing = generated_profiles.get(name)
    if existing is not None and existing != canonical:
        _raise(
            "MH_E_AMBIGUOUS_RESOURCE_NAME", [provenance, name],
            "content-addressed dag4blend p2 profile collision has different "
            "canonical bytes")
    generated_profiles[name] = canonical
    return name, True


def _resource_identity(collection, owner, provenance):
    mh_markers = [key for key in ("mh_resource_kind", "mh_resource_name")
                  if key in collection]
    if mh_markers:
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE", [provenance, collection.name, *mh_markers],
            "dag4blend resource cannot carry partial or mixed MH identity")
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
    kind = _DAG4BLEND_TYPE_TO_KIND.get(type_name.casefold())
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
    value = _mapping_value(_dagor_settings(option), "weight:r")
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
    """Read the AUTHORED parent-local matrix, never the depsgraph mirror.

    ``Object.matrix_local`` is derived from evaluated world matrices, so a
    definition Collection that is not linked into an evaluated view layer
    reports identity forever and the adapter would silently publish identity
    transforms. ``matrix_parent_inverse @ matrix_basis`` is Blender's own
    definition of object parent-local, is what the .blend stores, and equals
    ``matrix_local`` exactly once the scene is evaluated. Using it makes this
    read-only adapter independent of evaluation state, which is what the
    save/reopen byte-identity gate (doc 15 2.6) requires.
    """
    try:
        matrix = obj.matrix_basis.copy()
        if obj.parent is not None:
            matrix = obj.matrix_parent_inverse @ matrix
    except (AttributeError, TypeError, ValueError) as exc:
        _raise(
            "MH_E_UNREPRESENTABLE_TRANSFORM", subjects,
            f"cannot read dag4blend parent-local matrix: {exc}")
    return _canonical_local_transform(matrix, subjects)


def convert_dag4blend_collection(
        collection, *, allow_prefab_as_mesh_lossy=False, warnings=None,
        generated_profiles=None):
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

    def warn(node_path, subject, message):
        warning = {
            "code": "MH_W_DAGOR_CONSTRUCT_DROPPED",
            "node_path": node_path,
            "subjects": [subject],
            "message": message,
        }
        if warnings is not None:
            warnings.append(warning)
        else:
            # Standalone DTO callers must not silently lose reached constructs
            # merely because they did not request the structured export report.
            import warnings as python_warnings
            python_warnings.warn(
                f"{warning['code']}: {subject}: {message}", stacklevel=2)

    def inspect_properties(obj, subject, node_path, *, option=False):
        reached_keys = {
            key for mapping in (_dagor_settings(obj), obj)
            for key in _mapping_keys(mapping)}
        metadata = _dag4blend_source_metadata(
            obj, subject, node_path, option=option)
        keys = sorted(key for key in reached_keys if key.casefold() in {
            "label", "label:t", "require", "colors"})
        for key in keys:
            warn(node_path, subject, f"Dropped Dagor {key}; it is never executed")
        return metadata

    def resource_identity(source, obj, subject, node_path):
        kind, name = _resource_identity(source, obj, subject)
        source_type = _mapping_value(source, "type")
        if source_type is _MISSING:
            source_type = _dagor_property(obj, "type:t")
        if isinstance(source_type, str) and source_type.casefold() == "prefab":
            if not allow_prefab_as_mesh_lossy:
                _raise(
                    "MH_E_INVALID_RESOURCE_SOURCE", [subject, name],
                    "Dagor prefab requires explicit Allow Prefab as Mesh (Lossy); "
                    "collision/gameplay semantics cannot be preserved")
            warn(node_path, subject,
                 f"Prefab {name} exported as mesh by explicit policy; "
                 "collision/gameplay semantics are not preserved")
        return kind, name

    def convert_object(obj, parent_world, node_path, ancestor_subjects=()):
        resource_label = ""
        instance = obj.instance_collection
        if instance is not None:
            candidate = _mapping_value(instance, "name")
            if isinstance(candidate, str) and candidate:
                resource_label = f"/resource:{candidate}"
        subject = (
            f"collection:{collection.name}/object:{obj.name}"
            f"{resource_label}/NodePath:{node_path}")
        subjects = [*ancestor_subjects, subject]
        place_type, appearance_seed_boundary = inspect_properties(
            obj, subject, node_path)
        profile, inline_profile = _dag4blend_profile(
            obj, subject, generated_profiles=generated_profiles)
        if inline_profile:
            # Dagor p2 and tm are mutually exclusive. dag4blend materializes
            # p2 base values into matrix_local only as a viewport preview; the
            # generated profile is the complete authority, so admitting that
            # matrix here would apply every base twice.
            local = Matrix.Identity(4)
            transform, local = _canonical_local_transform(local, subjects)
        else:
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
                option_subject = (
                    f"{subject}/ent[{option_index}]:{option_obj.name}"
                    f"/NodePath:{node_path}/options[{option_index}]")
                option_path = f"{node_path}/options[{option_index}]"
                inspect_properties(
                    option_obj, option_subject, option_path, option=True)
                _dag4blend_profile(
                    option_obj, option_subject, option=True,
                    generated_profiles=generated_profiles)
                if option_obj.type != "EMPTY":
                    _raise(
                        "MH_E_UNREPRESENTABLE_SCENE_OBJECT", [option_subject],
                        "dag4blend random options must be Empty objects")
                option_collection = option_obj.instance_collection
                if option_collection is None:
                    # An unbound ent is the explicit Dagor "nothing" option,
                    # not an unresolved resource guessed from its display name.
                    option_kind, option_name = "empty", None
                else:
                    option_kind, option_name = resource_identity(
                        option_collection, option_obj, option_subject, option_path)
                weight = _option_weight(option_obj, option_subject)
                try:
                    total = math.fsum((total, weight))
                except OverflowError:
                    _raise(
                        "MH_E_COMPOSITE_GRAMMAR", [subject],
                        "dag4blend random option total must be finite and positive")
                options.append(RandomOption(option_kind, weight, option_name))
                if option_collection is not None:
                    _register_override(
                        overrides, (option_kind, option_name),
                        option_collection, option_subject)
            if not options or not math.isfinite(total):
                _raise(
                    "MH_E_COMPOSITE_GRAMMAR", [subject],
                    "dag4blend random node requires a finite positive option total")
            if all(option.kind == "empty" for option in options):
                # Dagor clears an all-empty entList. Keep the structural frame
                # and children, but there is no entity and no selection draw.
                kind, options = "group", []
            elif total <= 0.0:
                _raise(
                    "MH_E_COMPOSITE_GRAMMAR", [subject],
                    "dag4blend random node requires a finite positive option total")
        elif obj.instance_collection is None:
            kind = "group"
        else:
            kind, resource = resource_identity(
                obj.instance_collection, obj, subject, node_path)
            _register_override(
                overrides, (kind, resource), obj.instance_collection, subject)

        converted_children = [
            convert_object(
                child, world, f"{node_path}/children[{index}]", tuple(subjects))
            for index, child in enumerate(children[obj.as_pointer()])
        ]
        return Node(
            kind,
            transform=transform,
            resource=resource,
            profile=profile,
            options=options,
            children=converted_children,
            place_type=place_type,
            appearance_seed_boundary=appearance_seed_boundary,
        )

    document = Composite(root_name, [
        convert_object(obj, Matrix.Identity(4), f"{root_name}:nodes[{index}]")
        for index, obj in enumerate(roots)
    ])
    return document, overrides


_STRUCTURAL_COMPLETENESS_MODES = frozenset({"composite_closure", "include_all"})


def _structurally_empty(collection) -> bool:
    """dag4blend's own not-imported shape: zero objects and zero children.

    This is a STRUCTURAL predicate, never a replay of import flags. It matches
    ``dag4blend cmp_import.get_node_collection``, which allocates a stub
    Collection carrying full Dagor identity and leaves it empty whenever the
    artist imported without Recursive. An authored composite whose random
    variants are all empty is NOT empty: it still owns its node Empties.
    """

    return not collection.objects and not collection.children


def _nonempty_source_composite(inventory, name) -> bool:
    """Does Source Root already hold a real, non-empty payload for ``name``?"""

    if inventory is None:
        return False
    candidate = inventory.resolve(
        ResourceKey("composite", name), allow_missing=True)
    if candidate is None:
        return False
    return bool(read_composite_file(candidate.path).nodes)


def _dag4blend_collection_bundle(
        collection, *, allow_prefab_as_mesh_lossy=False, warnings=None,
        mode=None, source_root=None, generated_profiles=None):
    """Recursively convert all explicit composite definitions and options.

    Composite resource collections become documents, never overrides.  Only
    Mesh definition collections cross into the writer as explicit read-only
    inputs. Named gameobj collections carry tokens only: they have no resource
    payload, actor class binding or materialization requirement.

    ``mode``/``source_root`` enable the doc 15 1.5/2.4 completeness admission.
    ``root_only`` publishes nothing but the root, so it makes no demand on
    scene geometry; the closure modes refuse to turn a structurally empty
    nested definition into a valid empty document that would be published over
    a real ``.composite``.
    """

    documents = {}
    composite_sources = {}
    resource_overrides = {}
    inventory = None
    if mode in _STRUCTURAL_COMPLETENESS_MODES and source_root is not None:
        inventory = scan_source_inventory(source_root)

    if mode is not None and _structurally_empty(collection):
        # The root is published in EVERY export mode, so an empty root can only
        # ever overwrite a real source with "no nodes". ``mode is None`` is the
        # in-Blender migration path (Convert), which writes no source file and
        # therefore cannot destroy one.
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE", [f"collection:{collection.name}"],
            f"dag4blend composite root {collection.name!r} collection is "
            "empty (zero objects and zero child collections); likely imported "
            "without Recursive. A root composite definition is never "
            "published as an empty document")

    def admit_nested(owner, name, resource_collection):
        """Return True when the nested definition must be read from the scene."""

        if (mode not in _STRUCTURAL_COMPLETENESS_MODES
                or not _structurally_empty(resource_collection)):
            return True
        if _nonempty_source_composite(inventory, name):
            # Reuse the real Source Root payload; never republish emptiness.
            # Leaving the name out of ``documents`` is what routes the planner
            # to the source file, and the publication report names it as
            # reused rather than published.
            return False
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE",
            [f"composite:{owner}", resource_collection.name, name],
            f"nested dag4blend composite {name!r} (collection "
            f"{resource_collection.name!r}) collection is empty; likely "
            "imported without Recursive, and Source Root holds no non-empty "
            f"{name}.composite to reuse instead. Re-import that composite "
            "with Recursive enabled, or publish it on its own first; an empty "
            "definition must never overwrite a real source")

    def load(source_collection):
        document, discovered = convert_dag4blend_collection(
            source_collection, allow_prefab_as_mesh_lossy=allow_prefab_as_mesh_lossy,
            warnings=warnings, generated_profiles=generated_profiles)
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
                if admit_nested(document.name, name, resource_collection):
                    load(resource_collection)
            elif kind == "mesh":
                _register_override(
                    resource_overrides, key, resource_collection,
                    f"composite:{document.name}/{kind}:{name}")
            else:
                # GameObj identity belongs to the node/option, not a separately
                # published resource. A registry collision must never turn it
                # into an executable actor.
                if kind not in {"actor", "gameobj"}:
                    _raise(
                        "MH_E_UNSUPPORTED_NODE_KIND", [kind, name],
                        "unsupported dag4blend closure resource kind")

    load(collection)
    return documents, resource_overrides, composite_sources


def convert_dag4blend_collection_closure(collection):
    """Return DTOs and mesh inputs without changing the legacy scene."""
    documents, overrides, _sources = _dag4blend_collection_bundle(collection)
    return documents, overrides


def _dag4blend_external_relinks(composite_sources, working_scene):
    """Freeze only working-scene placements outside every source definition.

    Membership, not the current view layer or selection, defines ownership.
    A multiply linked object is protected if ANY service scene/definition owns
    it; changing the shared Object would otherwise mutate the migration source.
    """
    from .resource_markers import COLLECTION_KIND_KEY, COLLECTION_RESOURCE_KEY
    from .service_scenes import SERVICE_SCENE_NAMES

    service_names = set(SERVICE_SCENE_NAMES) | {
        "COMPOSITS", "GEOMETRY", "GAMEOBJ", "TECH_STUFF"}
    if working_scene is None or working_scene.name in service_names:
        return ()
    protected = set()
    protected_collections = set()
    source_names = {
        collection.as_pointer(): name
        for name, collection in composite_sources.items()}

    def protect(definition):
        identity = definition.as_pointer()
        if identity not in protected_collections:
            protected_collections.add(identity)
            protected.update(obj.as_pointer() for obj in definition.all_objects)

    for scene in bpy.data.scenes:
        if scene.name in service_names:
            protected.update(obj.as_pointer() for obj in scene.objects)
    for definition in bpy.data.collections:
        if (definition.name.startswith("random.")
                or definition.get("type") is not None
                or COLLECTION_KIND_KEY in definition
                or COLLECTION_RESOURCE_KEY in definition
                or definition.as_pointer() in source_names):
            protect(definition)
    for obj in bpy.data.objects:
        instance = obj.instance_collection
        owner_type = _dagor_property(obj, "type:t")
        typed = getattr(obj, "mh4blend", None)
        if instance is not None and (
                _is_random_helper(obj)
                or (isinstance(owner_type, str)
                    and owner_type.casefold() in _DAGOR_TYPE_TO_KIND)
                or (typed is not None and typed.kind in {
                    "mesh", "actor", "composite"})):
            protect(instance)
    planned = []
    for obj in working_scene.objects:
        source = obj.instance_collection
        if (obj.as_pointer() in protected or source is None
                or source.as_pointer() not in source_names):
            continue
        if obj.library is not None:
            _raise(
                "MH_E_INVALID_RESOURCE_SOURCE", [obj.name, source.name],
                "external linked placement cannot be relinked in place")
        planned.append((obj, source, source_names[source.as_pointer()]))
    return tuple(planned)


def _schedule_dag4blend_relinks(
        context, planned, *, composite_sources, working_scene):
    transaction = context["transaction"]
    resources = context["resources"]

    def finalize():
        # Validate the entire set before the first pointer changes.
        eligible = {
            obj.as_pointer(): (source, name)
            for obj, source, name in _dag4blend_external_relinks(
                composite_sources, working_scene)}
        for obj, source, name in planned:
            if (bpy.data.objects.get(obj.name) is not obj
                    or obj.instance_collection is not source
                    or eligible.get(obj.as_pointer()) != (source, name)):
                _raise(
                    "MH_E_INVALID_RESOURCE_SOURCE", [obj.name, name],
                    "external placement or its ownership changed during conversion")
        for obj, source, name in planned:
            transaction.add_rollback(
                lambda obj=obj, source=source: setattr(
                    obj, "instance_collection", source))
            obj.instance_collection = resources[("composite", name)]

    transaction.add_finalize(finalize)


def import_dag4blend_composite_collection(
        collection, *, source_root=None, load_mode=LOAD_MODE_FULL_LOD,
        definition_policy=DEFINITION_REUSE, relink_external=False) -> dict:
    """Convert an existing dag4blend definition through the shared materializer."""

    documents, overrides, composite_sources = _dag4blend_collection_bundle(
        collection)
    working_scene = bpy.context.scene
    relinks = (_dag4blend_external_relinks(composite_sources, working_scene)
               if relink_external else ())
    root_kind, root_name = _resource_identity(
        collection, None, f"collection:{collection.name}")
    if root_kind != "composite":
        _raise(
            "MH_E_RESOURCE_KIND_MISMATCH", [collection.name, root_kind],
            "dag4blend source collection must have explicit type 'composit'")
    report = materialize_composite_documents(
        documents,
        root_name=root_name,
        source_root=source_root,
        filepath=None,
        resource_overrides=overrides,
        load_mode=load_mode,
        definition_policy=definition_policy,
        before_commit=lambda context: _schedule_dag4blend_relinks(
            context, relinks, composite_sources=composite_sources,
            working_scene=working_scene),
    )
    report["relinked_placements"] = [obj.name for obj, _source, _name in relinks]
    return report


def _dag4blend_compatibility_report():
    return {
        "route": "dag4blend_scene_partial_compatibility",
        "preserved": ["hierarchy", "parent-local transforms", "random options and weights",
                      "nested composites", "mesh LODs", "materials", "gameObj tokens",
                      "node place_type", "ignoreParentInstSeed",
                      "inline p2 as generated placement profiles"],
        "unrecoverable": ["document-root properties", "aboveHt", "useCollisionNormal",
                          "quantizeTm", "label", "require", "colors", "xScale",
                          "snake_case aliases"],
        "blocked": ["prefab without explicit lossy opt-in",
                    "scene include without typed profile"],
    }


def publish_dag4blend_composite_collection(
        collection, output_dir, *, source_root,
        mode="include_all", lock_root=None, allow_prefab_as_mesh_lossy=False,
        _boundary_hook=None) -> dict:
    """Publish source files from the scene DTOs without changing any datablock."""
    from .dag4blend_publication import prepare_dag4blend_publication
    from .export_closure import (
        publish_composite_closure_export,
        stage_composite_closure_export,
    )

    warnings = []
    generated_profiles = {}
    documents, overrides, _sources = _dag4blend_collection_bundle(
        collection, allow_prefab_as_mesh_lossy=allow_prefab_as_mesh_lossy,
        warnings=warnings, mode=mode, source_root=source_root,
        generated_profiles=generated_profiles)
    _kind, root_name = _resource_identity(
        collection, None, f"collection:{collection.name}")
    plan = prepare_dag4blend_publication(
        documents, overrides, root_name=root_name, source_root=source_root,
        output_dir=output_dir, mode=mode,
        generated_profiles=generated_profiles)
    # Writer-side drops (doc 15 §2.2: excluded Dagor collision nodes and
    # technical materials) are recorded on the prepared FBX rows; the artist
    # must see them in the same report/exception channel as adapter warnings.
    for row in plan.payloads:
        prepared_warnings = getattr(row.prepared, "warnings", None)
        if prepared_warnings:
            warnings.extend(
                warning for warning in prepared_warnings
                if warning not in warnings)
    compatibility = _dag4blend_compatibility_report()
    try:
        with tempfile.TemporaryDirectory(prefix="mh-dag4blend-stage-") as staging:
            staged = stage_composite_closure_export(plan, staging_dir=staging)
            report = publish_composite_closure_export(
                plan, staged, lock_root=lock_root, _boundary_hook=_boundary_hook)
    except (OSError, RuntimeError, ValueError) as exc:
        # A published prefix may already contain intentionally dropped data.
        # Preserve its warnings alongside the exact partial-publication sets.
        exc.warnings = warnings
        exc.compatibility = compatibility
        raise
    root_row = plan.row_for(plan.closure.root)
    root_staged = next(row for row in staged if row.planned.key == plan.closure.root)
    report.update({
        "mode": mode,
        "root": str(plan.closure.root),
        "closure": [str(key) for key in plan.full_closure_keys],
        "staged": [str(row.key) for row in plan.payloads],
        "filepath": str(root_row.target),
        "resource_name": root_name,
        "nodes": len(documents[root_name].nodes),
        "bytes": len(root_staged.payload),
        "written": str(plan.closure.root) in report["published"],
        "warnings": warnings,
        "compatibility": compatibility,
    })
    return report
