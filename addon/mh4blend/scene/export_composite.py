"""Standalone export of one Blender collection to one ``.composite``.

This path deliberately does not gather GEOMETRY, export FBX, or scan every
scene. It atomically replaces one payload and upserts that one resource in the
directory-local ``export_manifest.json`` index.
"""

import json
import os

import bpy

from ..core.model import Manifest, composite_disk_dict, composite_hash
from ..core.uid import ensure_uid
from ..core.validate import validate_manifest
from .composite_extract import PROP_KIND, extract_composite
from .source_manifest import (
    commit_staged_manifest,
    prepare_manifest_update,
    stage_manifest,
)

__all__ = ["export_composite", "export_composite_collection"]


def _write_json_atomic(path, document):
    temporary = path + ".tmp"
    try:
        with open(temporary, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(document, stream, indent=2, ensure_ascii=False)
            stream.write("\n")
        os.replace(temporary, path)
    except Exception:
        try:
            os.remove(temporary)
        except OSError:
            pass
        raise


def _hint_uids(composite_collections):
    values = set()
    for item in composite_collections or ():
        if isinstance(item, str):
            values.add(item)
        else:
            values.add(ensure_uid(item))
    return values


def export_composite(collection, output, composite_collections=()):
    """Export ``collection`` as a single atomic ``.composite`` file.

    Args:
        collection: selected Blender collection definition.
        output: destination folder, or an explicit ``.composite`` path.
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

    # Lazy identity assignment is scoped to direct Empty nodes and their
    # referenced resource collections. Literal child collections are ignored.
    for obj in collection.objects:
        if obj.type != "EMPTY":
            continue
        ensure_uid(obj)
        if obj.instance_collection is not None:
            ensure_uid(obj.instance_collection)

    composite = extract_composite(collection, hinted_uids)
    validation = validate_manifest(Manifest(
        bundle_uid=collection_uid,
        bundle_name=collection.name,
        blend_file=os.path.basename(bpy.data.filepath) or "untitled.blend",
        exporter_version="0.3.0",
        composites=[composite],
    ))
    if validation["errors"]:
        return {"ok": False, "path": None, "resource_entry": None,
                "validation": validation,
                "warnings": validation.get("warnings", [])}

    resolved = bpy.path.abspath(output)
    if resolved.lower().endswith(".composite"):
        target = resolved
    else:
        target = os.path.join(resolved, composite.filename())
    target = os.path.abspath(target)
    os.makedirs(os.path.dirname(target), exist_ok=True)
    document = composite_disk_dict(composite)
    content_hash = composite_hash(composite)
    resource_entry = {
        "uid": composite.uid,
        "kind": "composite",
        "name": composite.name,
        "source": os.path.basename(target),
        "content_hash": content_hash,
    }
    if composite.properties:
        resource_entry["properties"] = composite.properties
    manifest = prepare_manifest_update(
        os.path.dirname(target), resources=[resource_entry],
        exporter_version="0.3.0",
        blend_file=os.path.basename(bpy.data.filepath) or "untitled.blend")
    # Fail closed for readers before replacing the payload. A failed payload
    # write deliberately leaves the marker for an explicit recovery export.
    stage_manifest(os.path.dirname(target), manifest)
    _write_json_atomic(target, document)
    commit_staged_manifest(os.path.dirname(target))
    return {"ok": True, "path": target, "validation": validation,
            "warnings": validation.get("warnings", []),
            "resource_entry": resource_entry,
            "uid": composite.uid, "node_count": len(composite.nodes)}


def export_composite_collection(collection, output_dir,
                                composite_collections=()):
    """UI-facing alias with the collection/folder vocabulary."""
    return export_composite(collection, output_dir, composite_collections)
