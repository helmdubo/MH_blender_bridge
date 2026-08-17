"""bpy adapter: COMPOSITS scene -> semantic Composite models (B8).

Walk pattern per dag4blend cmp_export::write_node, rewritten: collections
directly under the COMPOSITS scene are composite definitions; Empties with
instance_collection are placements (kind decided by the TARGET collection:
a composite collection -> composite_ref, a GEOMETRY resource -> mesh);
Empties without an instance are transform groups; hierarchy is object
parenting. Local transforms are parent-relative
(parent_world_inv @ child_world) and converted per §11.

Properties bags: custom props prefixed mh_p_ (QUESTION-6 decision).
Per the QUESTION-9 resolution the levels are strictly separated: mh_p_* on
the placement Empty -> the NODE bag (placement-level); mh_p_* on a resource
collection -> the manifest RESOURCE entry bag (asset-level, gathered by the
exporter, never inherited into nodes).
"""

from ..core.model import Composite, Node, QuantizedTransform
from ..core.transforms import quat_to_ue, scale_to_ue, translation_to_ue
from ..core.uid import PROP_UID
from ..core.validate import MHValidationError

__all__ = ["extract_composites", "PROP_PREFIX_PROPERTIES"]

PROP_PREFIX_PROPERTIES = "mh_p_"


def _bag(carrier):
    out = {}
    for key in carrier.keys():
        if isinstance(key, str) and key.startswith(PROP_PREFIX_PROPERTIES):
            out[key[len(PROP_PREFIX_PROPERTIES):]] = carrier[key]
    return out


def _node_kind(obj, node_uid, composite_uids):
    target = obj.instance_collection
    if target is None:
        return "group", None
    target_uid = target.get(PROP_UID)
    if not target_uid:
        raise MHValidationError(
            "MH_E_MISSING_COLLECTION_UID", [node_uid],
            f"'{obj.name}' instances collection '{target.name}' "
            f"without {PROP_UID}")
    kind = "composite_ref" if target_uid in composite_uids else "mesh"
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


def extract_composites(composits_scene):
    """-> list[Composite] for every collection directly under the COMPOSITS
    scene collection. UIDs must already be assigned (lazy pass runs first)."""
    collections = list(composits_scene.collection.children)
    composite_uids = set()
    for collection in collections:
        uid = collection.get(PROP_UID)
        if not uid:
            raise MHValidationError(
                "MH_E_MISSING_COLLECTION_UID", [collection.name],
                f"composite collection '{collection.name}' has no {PROP_UID}")
        composite_uids.add(uid)

    composites = []
    for collection in collections:
        if len(collection.children) > 0:
            raise MHValidationError(
                "MH_E_NESTED_COMPOSITE_COLLECTION", [collection.get(PROP_UID)],
                f"'{collection.name}' contains sub-collections; "
                "composites are flat")
        members = {obj.name: obj for obj in collection.objects}
        nodes = []
        for obj in collection.objects:
            uid = obj.get(PROP_UID)
            if not uid:
                raise ValueError(
                    f"node '{obj.name}' has no {PROP_UID}; run UID "
                    "assignment before extraction")
            kind, resource_uid = _node_kind(obj, uid, composite_uids)

            parent_uid = None
            if obj.parent is not None:
                if obj.parent.name not in members:
                    # Parent outside this composite's node table: exported
                    # parent_uid would dangle (§4.1) — surface it here.
                    raise MHValidationError(
                        "MH_E_DANGLING_PARENT", [uid],
                        f"'{obj.name}' is parented to '{obj.parent.name}' "
                        f"outside '{collection.name}'")
                parent_uid = obj.parent.get(PROP_UID)

            # Placement-level bag only (Q9: no inheritance from resources).
            properties = _bag(obj)

            nodes.append(Node(
                node_uid=uid,
                parent_uid=parent_uid,
                kind=kind,
                display_name=obj.name,
                resource_uid=resource_uid,
                local_transform=_local_transform(obj),
                properties=properties,
            ))
        composites.append(Composite(
            uid=collection.get(PROP_UID),
            name=collection.name,
            nodes=nodes,
            properties=_bag(collection),
        ))
    composites.sort(key=lambda c: c.uid.encode("utf-8"))
    return composites
