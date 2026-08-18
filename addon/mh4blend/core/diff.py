"""Reader-side semantic differ for MH Source Protocol v2 snapshots.

The differ consumes in-memory snapshots built from primary payloads.  It never
reads manifests, writes a cache/index, or decides whether an exporter should
write.  Static-mesh semantics come from the embedded FBX passport; materials
and composites are compared in their canonical value spaces.

Ledger-only flags (``LOCAL_EDIT``/``CONFLICT``) and project-resolution state
(``EXTERNAL_UNRESOLVED``) are intentionally not inferred here.
"""

from __future__ import annotations

import posixpath

from .canonical import composite_canonical_form
from .materials import material_canonical_form

__all__ = ["FLAG_ORDER", "diff_source_snapshots"]


FLAG_ORDER = (
    "CREATE", "REMOVE", "RENAME", "UPDATE_GEOMETRY", "UPDATE_TRANSFORM",
    "UPDATE_PROPERTIES", "REPARENT", "UPDATE_RESOURCE", "UPDATE_KIND",
    "MOVE", "LOCAL_EDIT", "CONFLICT", "EXTERNAL_UNRESOLVED",
)


def _ordered(flags):
    index = {flag: i for i, flag in enumerate(FLAG_ORDER)}
    return sorted(set(flags), key=index.__getitem__)


def _resources(snapshot):
    if not isinstance(snapshot, dict):
        raise TypeError("source snapshot must be an object")
    resources = snapshot.get("resources")
    if not isinstance(resources, dict):
        raise ValueError("source snapshot resources must be a UID mapping")
    return resources


def _canon_nodes(composite_doc):
    canon = composite_canonical_form(composite_doc)
    return {node["node_uid"]: node for node in canon["nodes"]}


def _diff_nodes(old_doc, new_doc):
    """Node-space diff of one v2 composite present in both snapshots."""
    old_nodes = _canon_nodes(old_doc)
    new_nodes = _canon_nodes(new_doc)
    result = {}
    for uid in old_nodes.keys() - new_nodes.keys():
        result[uid] = ["REMOVE"]
    for uid in new_nodes.keys() - old_nodes.keys():
        result[uid] = ["CREATE"]
    for uid in old_nodes.keys() & new_nodes.keys():
        old, new = old_nodes[uid], new_nodes[uid]
        flags = []
        if old.get("display_name") != new.get("display_name"):
            flags.append("RENAME")
        if old.get("local_transform") != new.get("local_transform"):
            flags.append("UPDATE_TRANSFORM")
        if old.get("properties") != new.get("properties"):
            flags.append("UPDATE_PROPERTIES")
        if old.get("parent_uid") != new.get("parent_uid"):
            flags.append("REPARENT")
        if old.get("resource_uid") != new.get("resource_uid"):
            flags.append("UPDATE_RESOURCE")
        if old.get("kind") != new.get("kind"):
            flags.append("UPDATE_KIND")
        if flags:
            result[uid] = _ordered(flags)
    return result


def _path(row):
    value = row.get("source_path")
    if not isinstance(value, str) or not value:
        raise ValueError("snapshot resource source_path must be non-empty text")
    return posixpath.normpath(value)


def _document(row, expected_schema):
    document = row.get("document")
    if not isinstance(document, dict) or document.get("schema") != expected_schema:
        raise ValueError(
            f"snapshot resource has no validated {expected_schema} document")
    return document


def _mesh_descriptor_semantics(document):
    """Descriptor fields that change the imported asset, excluding labels.

    The production descriptor hash is still compared as the fast-path.  A hash
    difference is then classified structurally so a rename remains ``RENAME``
    and exporter-version churn alone does not fabricate an asset update.
    """
    return {
        "lod_levels": document["lod_levels"],
        "lod_policy": document["lod_policy"],
        "material_slots": document["material_slots"],
        "properties": document["properties"],
    }


def _diff_same_resource(old, new):
    flags = []
    if old.get("name") != new.get("name"):
        flags.append("RENAME")
    if _path(old) != _path(new):
        flags.append("MOVE")

    old_kind = old.get("kind")
    new_kind = new.get("kind")
    if old_kind != new_kind:
        flags.append("UPDATE_KIND")
        return _ordered(flags), None

    node_ops = None
    if new_kind == "static_mesh":
        old_doc = _document(old, "mh.fbx_passport")
        new_doc = _document(new, "mh.fbx_passport")
        if old.get("geometry_hash") != new.get("geometry_hash"):
            flags.append("UPDATE_GEOMETRY")
        if (old.get("descriptor_hash") != new.get("descriptor_hash")
                and _mesh_descriptor_semantics(old_doc)
                != _mesh_descriptor_semantics(new_doc)):
            flags.append("UPDATE_PROPERTIES")
    elif new_kind == "material":
        old_doc = _document(old, "mh.material")
        new_doc = _document(new, "mh.material")
        old_semantic = material_canonical_form(
            old_doc["shader_class"], old_doc["params"], old_doc["textures"])
        new_semantic = material_canonical_form(
            new_doc["shader_class"], new_doc["params"], new_doc["textures"])
        if old_semantic != new_semantic:
            flags.append("UPDATE_PROPERTIES")
    elif new_kind == "composite":
        old_doc = _document(old, "mh.composite")
        new_doc = _document(new, "mh.composite")
        old_canon = composite_canonical_form(old_doc)
        new_canon = composite_canonical_form(new_doc)
        if old_canon.get("properties") != new_canon.get("properties"):
            flags.append("UPDATE_PROPERTIES")
        node_ops = _diff_nodes(old_doc, new_doc)
    else:
        raise ValueError(f"unsupported snapshot resource kind: {new_kind!r}")
    return _ordered(flags), node_ops


def diff_source_snapshots(old_snapshot, new_snapshot):
    """Produce one ``mh.diff_report`` from two validated v2 source snapshots.

    Each snapshot is an in-memory object with ``resources`` mapping ResourceUID
    to a row produced by ``tools.diff_bundles.load_source_snapshot``.  Resource
    path comparison is per UID, so MOVE is independent of semantic changes.
    """
    old_res = _resources(old_snapshot)
    new_res = _resources(new_snapshot)
    resources = {}
    nodes = {}

    for uid in old_res.keys() - new_res.keys():
        resources[uid] = ["REMOVE"]
    for uid in new_res.keys() - old_res.keys():
        resources[uid] = ["CREATE"]

    for uid in old_res.keys() & new_res.keys():
        flags, node_ops = _diff_same_resource(old_res[uid], new_res[uid])
        if flags:
            resources[uid] = flags
        if node_ops:
            nodes[uid] = dict(sorted(node_ops.items()))

    return {
        "schema": "mh.diff_report",
        "schema_version": 1,
        "resources": dict(sorted(resources.items())),
        "nodes": dict(sorted(nodes.items())),
    }
