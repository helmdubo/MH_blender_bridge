"""Blender writer for Source Protocol v5 composite resources."""

from __future__ import annotations

import math

import bpy
from mathutils import Matrix

from ..core.canonical import validate_resource_name
from ..core.canonical_json import canonical_json_bytes, parse_json
from ..core.composites import (
    composite_json_bytes,
)
from ..core.model import Composite, CompositeTransform, Node, RandomOption
from ..core.transforms import (
    blender_to_ue_transform,
    matrix_reconstructs_as_float32_trs,
)
from ..core.validate import MHValidationError
from .export_fbx import _dagor_lod_structure
from .resource_markers import (
    COLLECTION_KIND_KEY,
    COLLECTION_RESOURCE_KEY,
)
from ..ui.composite_authoring import (
    OPTION_INDEX_MIRROR_KEY,
    PROFILE_MIRROR_KEY,
    WEIGHT_MIRROR_KEY,
    validate_random_options,
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
    # matrix_basis is the authored parent-local value and is available even
    # while the definition lives in a non-active service scene. matrix_local
    # is depsgraph-derived and reports identity until that scene is evaluated.
    obj[_IMPORTED_MATRIX_KEY] = _matrix_signature(obj.matrix_basis)


def _stored_imported_transform(obj) -> CompositeTransform | None:
    encoded = obj.get(_IMPORTED_TRANSFORM_KEY)
    snapshot = obj.get(_IMPORTED_MATRIX_KEY)
    if (not isinstance(encoded, str) or not isinstance(snapshot, str)
            or snapshot != _matrix_signature(obj.matrix_basis)):
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


def _collection_instance_identity(instance) -> tuple[str, str]:
    """Infer one resource placement without asking the artist to restate it."""
    leaked_option_keys = tuple(
        key for key in (WEIGHT_MIRROR_KEY, OPTION_INDEX_MIRROR_KEY)
        if key in instance)
    if leaked_option_keys:
        raise MHValidationError(
            "MH_E_COMPOSITE_GRAMMAR", [instance.name, *leaked_option_keys],
            "resource definition collections must not carry random-option "
            "weight or index properties")

    marker_kind = instance.get(COLLECTION_KIND_KEY)
    marker_resource = instance.get(COLLECTION_RESOURCE_KEY)
    if marker_kind is not None or marker_resource is not None:
        if marker_kind not in {"mesh", "actor", "composite"}:
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

    # An already imported dag4blend definition carries explicit, lossless
    # collection identity.  Never infer a Dagor resource type from its name.
    dagor_type = instance.get("type")
    dagor_name = instance.get("name")
    if dagor_type is not None or dagor_name is not None:
        if not isinstance(dagor_type, str) or not isinstance(dagor_name, str):
            raise MHValidationError(
                "MH_E_COMPOSITE_GRAMMAR", [instance.name],
                "dag4blend resource collection requires string type/name")
        kind = {
            "composit": "composite",
            "composite": "composite",
            "rendinst": "mesh",
            "prefab": "mesh",
            "gameobj": "actor",
        }.get(dagor_type.casefold())
        if kind is None:
            raise MHValidationError(
                "MH_E_COMPOSITE_GRAMMAR", [instance.name, dagor_type],
                "dag4blend resource collection has unsupported explicit type")
        validate_resource_name(dagor_name)
        return kind, dagor_name

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


_DAG4BLEND_REMEDY = (
    "This object carries dag4blend markers ({markers}); a dag4blend scene is "
    "not MH authority. Run 'Convert dag4blend Scene Composite' "
    "(mh.convert_dag4blend_composite) on the definition Collection first, or "
    "convert the authoritative *.composit.blk directly with 'Import Dagor "
    "Composite' (mh.import_dagor_composite). Export never converts a scene "
    "implicitly")


def _dag4blend_markers(obj) -> list[str]:
    """Name the dag4blend traces that explain a missing typed authority."""

    markers = []
    dagorprops = getattr(obj, "dagorprops", None)
    if dagorprops is not None:
        try:
            if len(dagorprops.keys()) > 0:
                markers.append("dagorprops")
        except (AttributeError, TypeError):
            markers.append("dagorprops")
    for key in ("type:t", "weight:r"):
        try:
            if key in obj.keys():
                markers.append(f"ID {key!r}")
        except (AttributeError, TypeError):
            pass
    instance = getattr(obj, "instance_collection", None)
    if instance is not None:
        try:
            if "type" in instance.keys() or "name" in instance.keys():
                markers.append(f"dag4blend collection {instance.name!r}")
            elif instance.name.split(".")[0] == "random":
                markers.append(f"random helper {instance.name!r}")
        except (AttributeError, TypeError):
            pass
    return markers


def _typed_kind(obj):
    """Return the typed authority, or None when the author left it unset."""

    settings = getattr(obj, "mh4blend", None)
    kind = None if settings is None else settings.kind
    return None if kind in {None, "unset"} else kind


def _placement_kind(obj, instance, instance_kind, *, option: bool) -> str:
    """Typed authority first, then the scene graph; never the ID mirror.

    Typed `mh4blend.kind` stays the single authority whenever it is set.  When
    it is unset the kind is DERIVED from what the object actually is - the
    resource collection it instances, or a plain Empty standing for a group.
    That derivation reads the scene graph, which already decides the resource
    identity, and never the non-authoritative `mh_composite_kind` mirror, so
    hand-authored scenes export without stamping every placement by hand.
    Random is deliberately underivable: it exists only as typed intent.
    """

    kind = _typed_kind(obj)
    if kind is not None:
        return kind
    if instance_kind is not None:
        return instance_kind
    if instance is None and obj.type == "EMPTY":
        return "empty" if option else "group"

    message = (
        f"placement object {obj.name!r} (type={obj.type!r}) has no typed "
        f"mh4blend.kind and no derivable identity: it instances no resource "
        f"collection and is not a plain Empty. The ID property "
        f"{NODE_KIND_KEY!r} is only a diagnostic mirror")
    markers = _dag4blend_markers(obj)
    if markers:
        message += ". " + _DAG4BLEND_REMEDY.format(markers=", ".join(markers))
    raise MHValidationError("MH_E_COMPOSITE_GRAMMAR", [obj.name], message)


def _node_kind_and_resource(
        obj, *, option=False) -> tuple[str, str | None]:
    explicit_resource = obj.get(NODE_RESOURCE_KEY)
    instance = getattr(obj, "instance_collection", None)
    instance_kind = None
    instance_resource = None
    if instance is not None:
        try:
            instance_kind, instance_resource = _collection_instance_identity(
                instance)
        except MHValidationError as exc:
            # The instanced collection is unrecognisable.  If the object still
            # carries dag4blend traces, that is why - say so here too, because
            # derivation reaches this failure before the typed-kind message.
            markers = _dag4blend_markers(obj)
            if not markers or _typed_kind(obj) is not None:
                raise
            raise MHValidationError(
                exc.code, [obj.name, *exc.subjects],
                f"{exc.message}. "
                + _DAG4BLEND_REMEDY.format(markers=", ".join(markers))
            ) from exc

    explicit_kind = _typed_kind(obj)
    kind = _placement_kind(obj, instance, instance_kind, option=option)
    allowed = (
        {"mesh", "actor", "composite", "empty", "marker"}
        if option else
        {"mesh", "actor", "composite", "group", "random", "marker"}
    )
    if kind not in allowed:
        instance_name = getattr(instance, "name", None)
        raise MHValidationError(
            "MH_E_COMPOSITE_GRAMMAR", [obj.name],
            f"placement object {obj.name!r} (type={obj.type!r}, "
            f"instance_collection={instance_name!r}) has "
            f"typed mh4blend.kind={explicit_kind!r} and collection "
            f"kind={instance_kind!r}")

    if instance_kind is not None and instance_kind != kind:
        raise MHValidationError(
            "MH_E_RESOURCE_KIND_MISMATCH", [obj.name, instance.name],
            f"typed kind {kind!r} disagrees with resource collection kind "
            f"{instance_kind!r}")

    if (explicit_resource is not None and instance_resource is not None
            and explicit_resource != instance_resource):
        raise MHValidationError(
            "MH_E_RESOURCE_KIND_MISMATCH", [obj.name, instance.name],
            f"{NODE_RESOURCE_KEY}={explicit_resource!r} disagrees with "
            f"resource collection identity {instance_resource!r}")
    resource = explicit_resource or instance_resource
    if kind in {"group", "random", "empty"}:
        if resource not in {None, ""}:
            raise MHValidationError(
                "MH_E_COMPOSITE_GRAMMAR", [obj.name],
                f"{kind} cannot carry a resource")
        if instance is not None:
            raise MHValidationError(
                "MH_E_COMPOSITE_GRAMMAR", [obj.name],
                f"{kind} cannot instance a resource collection")
        return kind, None
    if not isinstance(resource, str) or not resource:
        raise MHValidationError(
            "MH_E_COMPOSITE_GRAMMAR", [obj.name],
            f"{kind} placement object {obj.name!r} requires a logical "
            f"resource token in {NODE_RESOURCE_KEY}; current value is "
            f"{resource!r}")
    validate_resource_name(resource)
    return kind, resource


def _random_option(option, child_objects) -> RandomOption:
    if child_objects:
        raise MHValidationError(
            "MH_E_COMPOSITE_GRAMMAR", [option.name],
            "random option Empty cannot have children")
    kind, resource = _node_kind_and_resource(option, option=True)
    if (kind not in {"empty", "marker"} and option.instance_collection is None
            and not bool(option.get(UNRESOLVED_PLACEMENT_KEY, False))):
        raise MHValidationError(
            "MH_E_COMPOSITE_GRAMMAR", [option.name],
            "resolved random option requires instance_collection")
    settings = option.mh4blend
    if settings.profile:
        raise MHValidationError(
            "MH_E_COMPOSITE_GRAMMAR", [option.name, settings.profile],
            "random options cannot carry placement profiles")
    weight = settings.weight
    if (isinstance(weight, bool) or not isinstance(weight, (int, float))
            or not math.isfinite(float(weight)) or weight < 0.0):
        raise MHValidationError(
            "MH_E_COMPOSITE_GRAMMAR", [option.name],
            "random option weight must be a finite number >= 0")
    return RandomOption(kind=kind, resource=resource, weight=float(weight))


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

    def has_option_index(obj) -> bool:
        settings = getattr(obj, "mh4blend", None)
        if settings is None:
            return False
        checker = getattr(settings, "is_property_set", None)
        return checker is None or checker("option_index")

    for obj in ordered:
        if not has_option_index(obj):
            continue
        parent = obj.parent
        if (parent is None or parent.as_pointer() not in identities
                or _typed_kind(parent) != "random"):
            raise MHValidationError(
                "MH_E_COMPOSITE_GRAMMAR", [obj.name],
                "typed option_index is valid only on a direct child of a "
                "typed random node in the selected collection")

    def build(obj) -> Node:
        kind, resource = _node_kind_and_resource(obj)
        profile = obj.mh4blend.profile or None
        if profile is not None:
            try:
                validate_resource_name(profile)
            except (TypeError, ValueError) as exc:
                raise MHValidationError(
                    "MH_E_NONCANONICAL_RESOURCE_NAME",
                    [obj.name, repr(profile)],
                    "typed mh4blend.profile must match [a-z0-9_]+ exactly",
                ) from exc
        display_name = obj.get(NODE_NAME_KEY)
        if display_name == "":
            display_name = None
        if display_name is not None and not isinstance(display_name, str):
            raise MHValidationError(
                "MH_E_COMPOSITE_GRAMMAR", [obj.name],
                "display-only composite node name must be a string")
        authored_children = children[obj.as_pointer()]
        options = []
        if kind == "random":
            option_objects = validate_random_options(obj)
            external_options = [
                option.name for option in option_objects
                if option.as_pointer() not in identities
            ]
            if external_options:
                raise MHValidationError(
                    "MH_E_PARENT_OUTSIDE_RESOURCE",
                    [obj.name, *external_options],
                    "random options must belong to the selected composite "
                    "collection")
            option_ids = {option.as_pointer() for option in option_objects}
            options = [
                _random_option(option, children[option.as_pointer()])
                for option in option_objects
            ]
            authored_children = [
                child for child in authored_children
                if child.as_pointer() not in option_ids
            ]
        return Node(
            kind=kind,
            resource=resource,
            name=display_name,
            transform=_object_transform(obj),
            profile=profile,
            options=options,
            children=[build(child) for child in authored_children],
        )

    return Composite(name=name, nodes=[build(obj) for obj in roots])


def export_composite_collection(
        collection, output_dir, *, source_root, allow_prefab_as_mesh_lossy=False) -> dict:
    """Publish only the root after full all-options source-closure admission."""

    # Imported lazily because the closure adapter reuses this module's pure
    # Blender extraction functions.
    from .export_closure import (
        CLOSURE_MODE_ROOT,
        export_composite_closure_collection,
    )
    return export_composite_closure_collection(
        collection,
        output_dir,
        source_root=source_root,
        mode=CLOSURE_MODE_ROOT,
        allow_prefab_as_mesh_lossy=allow_prefab_as_mesh_lossy,
    )
