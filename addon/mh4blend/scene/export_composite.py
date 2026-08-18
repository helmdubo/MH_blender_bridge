"""Standalone v2 export of one self-contained ``.composite`` payload."""

import json
import os

import bpy

from ..core.canonical import sanitize_name
from ..core.model import composite_disk_dict, composite_hash
from ..core.payload_publish_v2 import atomic_publish_json
from ..core.uid import PROP_UID, ensure_uid
from .composite_extract import PROP_KIND, extract_composite
from .import_composite import CompositeImportError, _validate_document

__all__ = ["export_composite", "export_composite_collection"]


def _hint_uids(composite_collections):
    values = set()
    for item in composite_collections or ():
        if isinstance(item, str):
            values.add(item)
        else:
            values.add(ensure_uid(item))
    return values


def _existing_payload(target, expected_uid):
    """Return a validated same-UID document or reject an occupied clean name."""
    if not os.path.exists(target):
        return None
    try:
        with open(target, "rb") as stream:
            raw = json.loads(stream.read().decode("utf-8"))
        document = _validate_document(raw, target)
    except (OSError, UnicodeError, json.JSONDecodeError,
            CompositeImportError, TypeError, ValueError) as exc:
        raise ValueError(
            "MH_E_PASSPORT_INVALID: occupied composite path has no valid "
            "embedded identity") from exc
    if document["uid"] != expected_uid:
        raise ValueError(
            "MH_E_NAME_COLLISION_DIFFERENT_UID: clean composite filename "
            f"is owned by {document['uid']}")
    return document


def _extract_acyclic_authoring_graph(root, composite_uids):
    """Extract ``root`` and reject cycles in reachable Blender definitions.

    This walks Collection Instance pointers already present in the scene.  It
    never resolves or scans disk dependencies; unresolved external targets are
    still legal leaves.  The traversal exists solely to enforce the composite
    graph invariant before publishing the selected definition.
    """
    state = {}
    stack = []
    root_composite = None

    def visit(collection):
        nonlocal root_composite
        identity = collection.as_pointer()
        status = state.get(identity, 0)
        if status == 1:
            cycle_start = next(
                (index for index, item in enumerate(stack)
                 if item.as_pointer() == identity), 0)
            path = stack[cycle_start:] + [collection]
            raise ValueError(
                "MH_E_COMPOSITE_CYCLE: "
                + " -> ".join(item.get(PROP_UID, item.name) for item in path))
        if status == 2:
            return

        ensure_uid(collection)
        for obj in collection.objects:
            if obj.type != "EMPTY":
                continue
            ensure_uid(obj)
            if obj.instance_collection is not None:
                ensure_uid(obj.instance_collection)
        composite = extract_composite(collection, composite_uids)
        if collection == root:
            root_composite = composite
        objects_by_uid = {
            obj.get(PROP_UID): obj for obj in collection.objects
            if obj.type == "EMPTY" and obj.get(PROP_UID)}

        state[identity] = 1
        stack.append(collection)
        try:
            for node in composite.nodes:
                if node.kind != "composite_ref":
                    continue
                instance = objects_by_uid.get(node.node_uid)
                target = instance.instance_collection if instance else None
                if target is not None:
                    visit(target)
        finally:
            stack.pop()
        state[identity] = 2

    visit(root)
    return root_composite


def export_composite(
        collection, output, composite_collections=(), *, source_root=None):
    """Export ``collection`` as a single atomic ``.composite`` file.

    Args:
        collection: selected Blender collection definition.
        output: destination folder. The clean filename is derived from the
            composite resource name; explicit payload paths are rejected.
        composite_collections: optional collection/UID hints used only to
            classify untagged collection-instance targets as composite refs.

    Returns a compact operator-friendly report. Validation errors are returned
    without touching the destination.
    """
    if collection is None:
        raise ValueError("MH_E_NO_COLLECTION: select a composite collection")

    collection_uid = ensure_uid(collection)
    collection[PROP_KIND] = "composite"
    hinted_uids = _hint_uids(composite_collections)
    hinted_uids.add(collection_uid)

    composite = _extract_acyclic_authoring_graph(collection, hinted_uids)
    resolved = bpy.path.abspath(os.fspath(output))
    if resolved.lower().endswith(".composite"):
        raise ValueError(
            "MH_E_INVALID_RESOURCE_SOURCE: choose an output directory; "
            "the clean composite filename is derived from the resource name")
    target = os.path.join(
        resolved, sanitize_name(composite.name) + ".composite")
    target = os.path.abspath(target)
    effective_source_root = bpy.path.abspath(os.fspath(
        source_root if source_root is not None else os.path.dirname(target)))
    root_key = os.path.normcase(os.path.realpath(effective_source_root))
    target_key = os.path.normcase(os.path.realpath(target))
    try:
        inside_root = os.path.commonpath([root_key, target_key]) == root_key
    except ValueError:
        inside_root = False
    if not inside_root:
        raise ValueError(
            "MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: composite output must be "
            "inside Project Source Root")
    document = composite_disk_dict(composite)
    _validate_document(document, target)
    warnings = []
    _existing_payload(target, composite.uid)
    content_hash = composite_hash(composite)
    published = atomic_publish_json(
        target, document, source_root=effective_source_root,
        pre_replace_guard=lambda: _existing_payload(target, composite.uid))
    validation = {"errors": [], "warnings": warnings}
    return {"ok": True, "path": target, "validation": validation,
            "warnings": warnings,
            "uid": composite.uid, "node_count": len(composite.nodes),
            "content_hash": content_hash, "written": True,
            "published": published}


def export_composite_collection(
        collection, output_dir, composite_collections=(), *, source_root=None):
    """UI-facing alias with the collection/folder vocabulary."""
    return export_composite(
        collection, output_dir, composite_collections,
        source_root=source_root)
