"""Standalone export of one Blender collection to one ``.composite``.

This path deliberately does not gather GEOMETRY, export FBX, or scan every
scene. It atomically replaces one payload and upserts that one resource in the
directory-local ``export_manifest.json`` index.
"""

import json
import os

import bpy

from ..core.canonical import composite_content_hash
from ..core.model import Manifest, composite_disk_dict, composite_hash
from ..core.source_resolver import (
    assert_source_snapshot_stable,
    pending_writer_uid,
    resolve_resource,
    resolve_resource_for_writer,
    scan_source_root,
)
from ..core.uid import ensure_uid
from ..core.validate import MHValidationError, validate_manifest
from .composite_extract import PROP_KIND, extract_composite
from .import_composite import CompositeImportError, _validate_document
from .source_manifest import (
    abandon_staged_manifest,
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


def _existing_payload_matches(writer_resolution, target, expected_hash):
    """Prove semantic equality before applying the payload hash-skip."""
    owner = writer_resolution.owner
    if writer_resolution.recovery or owner is None:
        return False
    if os.path.normcase(os.path.normpath(owner.payload_path)) != \
            os.path.normcase(os.path.normpath(target)):
        return False
    if owner.manifest_row.get("content_hash") != expected_hash:
        return False
    payload = writer_resolution.snapshot.payload_bytes.get(owner.payload_path)
    if payload is None:
        return False
    try:
        raw = json.loads(payload.decode("utf-8"))
        _validate_document(raw, owner.payload_path)
        return composite_content_hash(raw) == expected_hash
    except (UnicodeError, json.JSONDecodeError, CompositeImportError,
            TypeError, ValueError):
        return False


def _dependency_snapshot(document, *, source_root, registry_path,
                         texture_policy, snapshot=None):
    """Resolve the complete non-material closure and reject export cycles."""
    if snapshot is None:
        root = os.path.abspath(bpy.path.abspath(source_root))
        registry = (os.path.abspath(bpy.path.abspath(registry_path))
                    if registry_path else None)
        snapshot = scan_source_root(
            root, registry_path=registry, texture_policy=texture_policy)
    documents = {document["uid"]: document}
    visiting = []
    visited = set()
    missing_materials = set()

    def require(uid, kind):
        resolved = resolve_resource(snapshot, uid, expected_kind=kind)
        if resolved is None:
            raise MHValidationError(
                "MH_E_UNRESOLVED_EXTERNAL", [uid],
                f"{kind} resource {uid} is not exported under Source Root")
        return resolved

    def visit(uid):
        if uid in visiting:
            cycle = visiting[visiting.index(uid):] + [uid]
            raise MHValidationError(
                "MH_E_COMPOSITE_CYCLE", cycle,
                "composite dependency cycle: " + " -> ".join(cycle))
        if uid in visited:
            return
        visiting.append(uid)
        current = documents[uid]
        for node in current["nodes"]:
            target_uid = node.get("resource_uid")
            if target_uid is None:
                continue
            if node["kind"] == "mesh":
                mesh = require(target_uid, "static_mesh")
                for slot in mesh.manifest_row.get("material_slots", []):
                    material_uid = slot["material_uid"]
                    if resolve_resource(
                            snapshot, material_uid,
                            expected_kind="material") is None:
                        missing_materials.add(material_uid)
                continue
            if node["kind"] != "composite_ref":
                continue
            if target_uid in visiting:
                cycle = visiting[visiting.index(target_uid):] + [target_uid]
                raise MHValidationError(
                    "MH_E_COMPOSITE_CYCLE", cycle,
                    "composite dependency cycle: " + " -> ".join(cycle))
            resolved = require(target_uid, "composite")
            if target_uid not in documents:
                try:
                    payload = snapshot.payload_bytes[resolved.payload_path]
                    raw = json.loads(payload.decode("utf-8"))
                    documents[target_uid] = _validate_document(
                        raw, resolved.payload_path)
                except (KeyError, UnicodeError, json.JSONDecodeError,
                        CompositeImportError) as exc:
                    raise MHValidationError(
                        "MH_E_INVALID_COMPOSITE", [target_uid], str(exc)) \
                        from exc
            visit(target_uid)
        visiting.pop()
        visited.add(uid)

    visit(document["uid"])
    assert_source_snapshot_stable(snapshot)
    warnings = [{
        "code": diagnostic.code,
        "subjects": ([diagnostic.uid] if diagnostic.uid else []),
        "message": diagnostic.message,
    } for diagnostic in snapshot.diagnostics]
    if missing_materials:
        warnings.append({
            "code": "MH_W_MATERIAL_NOT_FOUND",
            "subjects": sorted(missing_materials),
            "message": (
                "referenced meshes use materials that are not exported under "
                "Project Source Root"),
        })
    return warnings


def export_composite(
        collection, output, composite_collections=(), *, source_root=None,
        registry_path=None, texture_policy="transitional"):
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
        exporter_version="0.5.0",
        composites=[composite],
    ))
    if validation["errors"]:
        return {"ok": False, "path": None, "resource_entry": None,
                "validation": validation,
                "warnings": validation.get("warnings", []),
                "written": False, "manifest_written": False}

    resolved = bpy.path.abspath(output)
    if resolved.lower().endswith(".composite"):
        target = resolved
    else:
        target = os.path.join(resolved, composite.filename())
    target = os.path.abspath(target)
    document = composite_disk_dict(composite)
    resolved_source_root = os.path.abspath(bpy.path.abspath(source_root))
    resolved_registry_path = (
        os.path.abspath(bpy.path.abspath(registry_path))
        if registry_path else None)
    pending = pending_writer_uid(
        resolved_source_root, expected_kind="composite")
    if pending is not None and pending.uid != composite.uid:
        raise MHValidationError(
            "MH_E_INVALID_EXPORT_MANIFEST", [pending.uid, composite.uid],
            f"pending composite recovery {pending.uid} is not the selected "
            f"composite {composite.uid}")
    writer_resolution = resolve_resource_for_writer(
        resolved_source_root,
        composite.uid,
        expected_kind="composite",
        expected_source=os.path.basename(target),
        registry_path=resolved_registry_path,
        texture_policy=texture_policy,
    )
    if writer_resolution.recovery:
        expected_manifest = os.path.normcase(os.path.normpath(os.path.join(
            os.path.dirname(target), "export_manifest.json")))
        actual_manifest = os.path.normcase(os.path.normpath(
            writer_resolution.owner.owning_manifest_path))
        expected_payload = os.path.normcase(os.path.normpath(target))
        actual_payload = os.path.normcase(os.path.normpath(
            writer_resolution.owner.payload_path))
        if (actual_manifest != expected_manifest
                or actual_payload != expected_payload):
            raise MHValidationError(
                "MH_E_INVALID_EXPORT_MANIFEST", [composite.uid],
                "pending composite recovery must use its exact owning "
                "manifest and payload path")
    dependency_warnings = _dependency_snapshot(
        document,
        source_root=resolved_source_root,
        registry_path=resolved_registry_path,
        texture_policy=texture_policy,
        snapshot=writer_resolution.snapshot,
    )
    validation["warnings"].extend(dependency_warnings)
    # v2 payload is the authority for composite resource properties, so its
    # receipt hashes the exact document which will be written.
    content_hash = composite_hash(composite)
    payload_needs_write = not _existing_payload_matches(
        writer_resolution, target, content_hash)
    resource_entry = {
        "uid": composite.uid,
        "kind": "composite",
        "name": composite.name,
        "source": os.path.basename(target),
        "content_hash": content_hash,
    }
    manifest = prepare_manifest_update(
        os.path.dirname(target), resources=[resource_entry],
        exporter_version="0.5.0",
        blend_file=os.path.basename(bpy.data.filepath) or None,
        source_root=resolved_source_root)
    # Fail closed for readers before replacing the payload. A failed payload
    # write deliberately leaves the marker for an explicit recovery export.
    stage_manifest(
        os.path.dirname(target), manifest,
        snapshot_guard=lambda: assert_source_snapshot_stable(
            writer_resolution.snapshot),
    )
    try:
        if payload_needs_write:
            _write_json_atomic(target, document)
        commit_staged_manifest(os.path.dirname(target))
    except BaseException:
        abandon_staged_manifest(os.path.dirname(target))
        raise
    return {"ok": True, "path": target, "validation": validation,
            "warnings": validation.get("warnings", []),
            "resource_entry": resource_entry,
            "uid": composite.uid, "node_count": len(composite.nodes),
            "written": payload_needs_write, "manifest_written": True}


def export_composite_collection(
        collection, output_dir, composite_collections=(), *, source_root=None,
        registry_path=None, texture_policy="transitional"):
    """UI-facing alias with the collection/folder vocabulary."""
    return export_composite(
        collection, output_dir, composite_collections,
        source_root=source_root,
        registry_path=registry_path,
        texture_policy=texture_policy)
