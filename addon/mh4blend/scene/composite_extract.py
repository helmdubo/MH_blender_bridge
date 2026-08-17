"""bpy adapter: Blender collection definitions -> semantic composites.

Walk pattern per dag4blend cmp_export::write_node, rewritten: a selected
collection is one composite definition; its directly linked Empties with
instance_collection are placements (kind decided by the TARGET collection:
a collection tagged ``mh_kind=composite`` (or named by the caller as a
composite definition) -> composite_ref, otherwise a mesh resource);
Empties without an instance are transform groups; hierarchy is object
parenting. Local transforms are parent-relative
(parent_world_inv @ child_world) and converted per §11.

Literal child collections are intentionally ignored: Blender collections do
not carry per-placement transforms. A collection placement is always an Empty
with ``instance_collection``.  Likewise, non-Empty objects directly linked to
the definition are authoring helpers rather than composite nodes.

Properties bags: custom props prefixed mh_p_ (QUESTION-6 decision).
Per the QUESTION-9 resolution the levels are strictly separated: mh_p_* on
the placement Empty -> the NODE bag (placement-level); mh_p_* on a resource
collection -> the manifest RESOURCE entry bag (asset-level, gathered by the
exporter, never inherited into nodes).
"""

import json

from ..core.model import Composite, Node, QuantizedTransform
from ..core.transforms import quat_to_ue, scale_to_ue, translation_to_ue
from ..core.uid import PROP_UID
from ..core.validate import MHValidationError

__all__ = [
    "extract_composite",
    "extract_composites",
    "PROP_PREFIX_PROPERTIES",
    "PROP_KIND",
    "PROP_RESOURCE_UID",
    "PROP_PARENT_UID",
    "PROP_DISPLAY_NAME",
]

PROP_PREFIX_PROPERTIES = "mh_p_"
PROP_KIND = "mh_kind"
PROP_RESOURCE_UID = "mh_resource_uid"
PROP_PARENT_UID = "mh_parent_uid"
PROP_DISPLAY_NAME = "mh_display_name"


def _plain_id_property(value):
    """Convert Blender IDProperty containers to ordinary JSON-ish values."""
    if hasattr(value, "to_dict"):
        return {str(k): _plain_id_property(v)
                for k, v in value.to_dict().items()}
    if hasattr(value, "keys") and not isinstance(value, dict):
        return {str(k): _plain_id_property(value[k]) for k in value.keys()}
    if isinstance(value, dict):
        return {str(k): _plain_id_property(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)) or (
            hasattr(value, "to_list") and not isinstance(value, str)):
        items = value.to_list() if hasattr(value, "to_list") else value
        return [_plain_id_property(v) for v in items]
    return value


def _bag(carrier):
    out = {}
    fallback = carrier.get("mh_properties_fallback_json")
    if isinstance(fallback, str) and fallback:
        try:
            decoded = json.loads(fallback)
            if isinstance(decoded, dict):
                out.update(decoded)
        except (TypeError, ValueError):
            # A user-edited invalid inspection snapshot must not make ordinary
            # authored mh_p_* values unreadable.
            pass
    for key in carrier.keys():
        if isinstance(key, str) and key.startswith(PROP_PREFIX_PROPERTIES):
            out[key[len(PROP_PREFIX_PROPERTIES):]] = \
                _plain_id_property(carrier[key])
    return out


def _node_kind(obj, node_uid, composite_uids):
    target = obj.instance_collection
    if target is None:
        return "group", None
    if obj.instance_type != "COLLECTION":
        raise MHValidationError(
            "MH_E_INVALID_COMPOSITE", [node_uid],
            f"'{obj.name}' retains instance_collection '{target.name}' but "
            f"instance_type is {obj.instance_type!r}; enable Collection "
            "instancing or clear the stale target")
    target_uid = target.get(PROP_UID)
    if not target_uid:
        raise MHValidationError(
            "MH_E_MISSING_COLLECTION_UID", [node_uid],
            f"'{obj.name}' instances collection '{target.name}' "
            f"without {PROP_UID}")
    offset = tuple(float(v) for v in target.instance_offset)
    if any(abs(v) > 1.0e-9 for v in offset):
        raise MHValidationError(
            "MH_E_INVALID_COLLECTION_OFFSET", [node_uid, target_uid],
            f"'{target.name}' has non-zero instance_offset {offset}; "
            "put placement transforms on the Empty")
    declared_kind = target.get(PROP_KIND)
    if declared_kind == "composite":
        kind = "composite_ref"
    elif declared_kind in {"static_mesh", "mesh"}:
        kind = "mesh"
    elif target_uid in composite_uids:
        kind = "composite_ref"
    else:
        # Order-independent authoring fallback. A referenced definition with
        # recursive mesh content is a mesh resource; an empty or Empty-only
        # definition is a composite. Imported unresolved placeholders always
        # carry an explicit mh_kind and therefore never rely on this heuristic.
        has_mesh = any(candidate.type == "MESH"
                       for candidate in target.all_objects)
        kind = "mesh" if has_mesh else "composite_ref"
    return kind, target_uid


def _local_transform(obj):
    if obj.parent is not None:
        matrix = obj.parent.matrix_world.inverted() @ obj.matrix_world
    else:
        matrix = obj.matrix_world
    location, rotation, scale = matrix.decompose()
    # mathutils.Quaternion stores (w, x, y, z) — reorder for §11.
    quat_xyzw = (rotation.x, rotation.y, rotation.z, rotation.w)
    return QuantizedTransform(
        translation=translation_to_ue(tuple(location)),
        rotation=quat_to_ue(quat_xyzw),
        scale=scale_to_ue(tuple(scale)),
    )


def extract_composite(collection, composite_uids=()):
    """Extract exactly one collection definition.

    ``composite_uids`` is an optional caller-supplied set used to classify
    untagged instance targets. Imported definitions carry ``mh_kind`` and do
    not need this hint. UIDs must already be assigned by the caller's lazy UID
    pass.
    """
    collection_uid = collection.get(PROP_UID)
    if not collection_uid:
        raise MHValidationError(
            "MH_E_MISSING_COLLECTION_UID", [collection.name],
            f"composite collection '{collection.name}' has no {PROP_UID}")

    known_composite_uids = set(composite_uids)
    known_composite_uids.add(collection_uid)
    members = {obj.as_pointer(): obj for obj in collection.objects
               if obj.type == "EMPTY"}
    nodes = []
    for obj in collection.objects:
        if obj.type != "EMPTY":
            continue
        uid = obj.get(PROP_UID)
        if not uid:
            raise ValueError(
                f"node '{obj.name}' has no {PROP_UID}; run UID "
                "assignment before extraction")
        kind, resource_uid = _node_kind(obj, uid, known_composite_uids)

        parent_uid = None
        if obj.parent is not None:
            if (obj.parent.type != "EMPTY"
                    or obj.parent.as_pointer() not in members):
                # Parent outside this composite's node table: exported
                # parent_uid would dangle (§4.1) — surface it here.
                raise MHValidationError(
                    "MH_E_DANGLING_PARENT", [uid],
                    f"'{obj.name}' is parented to '{obj.parent.name}' "
                    f"outside '{collection.name}'")
            parent_uid = obj.parent.get(PROP_UID)

        # Placement-level bag only (Q9: no inheritance from resources).
        properties = _bag(obj)

        display_name = obj.get(PROP_DISPLAY_NAME)
        if not isinstance(display_name, str) or not display_name:
            display_name = obj.name
        nodes.append(Node(
            node_uid=uid,
            parent_uid=parent_uid,
            kind=kind,
            display_name=display_name,
            resource_uid=resource_uid,
            local_transform=_local_transform(obj),
            properties=properties,
        ))
    return Composite(
        uid=collection_uid,
        name=collection.name,
        nodes=nodes,
        properties=_bag(collection),
    )


def extract_composites(composits_scene):
    """Compatibility adapter: extract every top-level definition in a scene.

    Standalone UI paths should call :func:`extract_composite` for the selected
    collection. This wrapper retains the Stage-B all-scene API.
    """
    collections = list(composits_scene.collection.children)
    composite_uids = set()
    for collection in collections:
        uid = collection.get(PROP_UID)
        if not uid:
            raise MHValidationError(
                "MH_E_MISSING_COLLECTION_UID", [collection.name],
                f"composite collection '{collection.name}' has no {PROP_UID}")
        composite_uids.add(uid)

    composites = [extract_composite(collection, composite_uids)
                  for collection in collections]
    composites.sort(key=lambda c: c.uid.encode("utf-8"))
    return composites
