"""Reference bundle differ — the executable spec of the UE Analyzer (§7).

Compares two parsed bundles (manifest documents + their composite
documents) and produces the mh.diff_report JSON. Ledger-only flags
(LOCAL_EDIT, CONFLICT) are NEVER emitted here (§7.2): they require the
three-way base/theirs/ours comparison only the UE side can perform.
EXTERNAL_UNRESOLVED likewise needs project state and is the Analyzer's.

Node comparison happens on canonical forms (§8): quantized integers, NFC
strings — so decimal spelling or float noise cannot fabricate diffs.
"""

import posixpath

from .canonical import composite_canonical_form

__all__ = ["FLAG_ORDER", "diff_bundles"]

# §7.2 table order.
FLAG_ORDER = (
    "CREATE", "REMOVE", "RENAME", "UPDATE_GEOMETRY", "UPDATE_TRANSFORM",
    "UPDATE_PROPERTIES", "REPARENT", "UPDATE_RESOURCE", "UPDATE_KIND",
    "MOVE", "LOCAL_EDIT", "CONFLICT", "EXTERNAL_UNRESOLVED",
)


def _ordered(flags):
    index = {flag: i for i, flag in enumerate(FLAG_ORDER)}
    return sorted(set(flags), key=index.__getitem__)


def _resources_by_uid(manifest):
    out = {}
    for entry in manifest.get("resources", []):
        out[entry["uid"]] = entry
    for entry in manifest.get("materials", []):
        out[entry["uid"]] = entry
    return out


def _canon_nodes(composite_doc):
    canon = composite_canonical_form(composite_doc)
    return {node["node_uid"]: node for node in canon["nodes"]}


def _composite_properties(resource_row, composite_doc):
    """Select the versioned authority while v1 and v2 coexist."""
    if composite_doc.get("schema_version") == 2:
        return composite_doc.get("properties")
    return resource_row.get("properties", {})


def _diff_nodes(old_doc, new_doc):
    """Node-space diff of one composite present in both versions."""
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


def diff_bundles(old_manifest, new_manifest, old_composites, new_composites,
                 old_rel_path="", new_rel_path=""):
    """Produce the mh.diff_report for two bundle versions.

    old_composites / new_composites: dict composite_uid -> parsed document.
    old_rel_path / new_rel_path: the bundle directory's path relative to
    source_root (D27) — when it differs, surviving resources get MOVE.
    """
    old_res = _resources_by_uid(old_manifest)
    new_res = _resources_by_uid(new_manifest)
    moved = posixpath.normpath(old_rel_path or ".") != \
        posixpath.normpath(new_rel_path or ".")

    resources = {}
    nodes = {}

    for uid in old_res.keys() - new_res.keys():
        resources[uid] = ["REMOVE"]
    for uid in new_res.keys() - old_res.keys():
        resources[uid] = ["CREATE"]
        # CREATE of a composite resource implies its nodes (§7.2): the node
        # space intentionally stays silent about them.

    for uid in old_res.keys() & new_res.keys():
        old, new = old_res[uid], new_res[uid]
        flags = []
        if old.get("name") != new.get("name"):
            flags.append("RENAME")
        if moved:
            flags.append("MOVE")
        kind = new.get("kind")
        if kind == "static_mesh":
            if old.get("properties") != new.get("properties"):
                flags.append("UPDATE_PROPERTIES")
            if old.get("content_hash") != new.get("content_hash"):
                flags.append("UPDATE_GEOMETRY")
            if (old.get("material_slots") != new.get("material_slots")
                    and "UPDATE_PROPERTIES" not in flags):
                flags.append("UPDATE_PROPERTIES")
        elif kind == "material":
            if (old.get("params") != new.get("params")
                    or old.get("textures") != new.get("textures")
                    or old.get("shader_class") != new.get("shader_class")):
                flags.append("UPDATE_PROPERTIES")
        elif kind == "composite":
            if _composite_properties(old, old_composites[uid]) != \
                    _composite_properties(new, new_composites[uid]):
                flags.append("UPDATE_PROPERTIES")
            node_ops = _diff_nodes(old_composites[uid], new_composites[uid])
            if node_ops:
                nodes[uid] = dict(sorted(node_ops.items()))
        if flags:
            resources[uid] = _ordered(flags)

    return {
        "schema": "mh.diff_report",
        "schema_version": 1,
        "resources": dict(sorted(resources.items())),
        "nodes": dict(sorted(nodes.items())),
    }
