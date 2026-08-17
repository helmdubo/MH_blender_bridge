"""bpy adapter: COMPOSITS scene -> semantic Composite models (B8).

Walk pattern per dag4blend cmp_export::write_node, rewritten: collections
directly under the COMPOSITS scene are composite definitions; Empties with
instance_collection are placements (kind decided by the TARGET collection:
a composite collection -> composite_ref, a GEOMETRY resource -> mesh);
Empties without an instance are transform groups; hierarchy is object
parenting. Local transforms are parent-relative
(parent_world_inv @ child_world) and converted per §11.

Properties bag: custom props prefixed mh_p_ (QUESTION-6 decision).
TODO(QUESTION-9): resource-collection-level mh_p_* props (e.g. role=decal
on decal_leak) are inherited by every node instancing that collection as
DEFAULTS, object-level mh_p_* override them — the schema keeps the bag on
nodes only, so this is the least-binding transport; recorded in
docs/QUESTIONS.md.
"""

from ..core.model import Composite, Node, QuantizedTransform
from ..core.transforms import quat_to_ue, scale_to_ue, translation_to_ue
from ..core.uid import PROP_UID

__all__ = ["extract_composites", "PROP_PREFIX_PROPERTIES"]

PROP_PREFIX_PROPERTIES = "mh_p_"


def _bag(carrier):
    out = {}
    for key in carrier.keys():
        if isinstance(key, str) and key.startswith(PROP_PREFIX_PROPERTIES):
            out[key[len(PROP_PREFIX_PROPERTIES):]] = carrier[key]
    return out


def _node_kind(obj, composite_uids):
    target = obj.instance_collection
    if target is None:
        return "group", None
    target_uid = target.get(PROP_UID)
    if not target_uid:
        raise ValueError(
            f"MH_E_MISSING_COLLECTION_UID: '{obj.name}' instances collection "
            f"'{target.name}' without {PROP_UID}")
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
            raise ValueError(
                f"MH_E_MISSING_COLLECTION_UID: composite collection "
                f"'{collection.name}' has no {PROP_UID}")
        composite_uids.add(uid)

    composites = []
    for collection in collections:
        if len(collection.children) > 0:
            raise ValueError(
                f"MH_E_NESTED_COMPOSITE_COLLECTION: '{collection.name}' "
                "contains sub-collections; composites are flat")
        members = {obj.name: obj for obj in collection.objects}
        nodes = []
        for obj in collection.objects:
            uid = obj.get(PROP_UID)
            if not uid:
                raise ValueError(
                    f"node '{obj.name}' has no {PROP_UID}; run UID "
                    "assignment before extraction")
            kind, resource_uid = _node_kind(obj, composite_uids)

            parent_uid = None
            if obj.parent is not None:
                if obj.parent.name not in members:
                    # Parent outside this composite's node table: exported
                    # parent_uid would dangle (§4.1) — surface it here.
                    raise ValueError(
                        f"MH_E_DANGLING_PARENT: '{obj.name}' is parented to "
                        f"'{obj.parent.name}' outside '{collection.name}'")
                parent_uid = obj.parent.get(PROP_UID)

            properties = {}
            if obj.instance_collection is not None:
                # TODO(QUESTION-9): resource-collection props as defaults.
                properties.update(_bag(obj.instance_collection))
            properties.update(_bag(obj))

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
        ))
    composites.sort(key=lambda c: c.uid.encode("utf-8"))
    return composites
