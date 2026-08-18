"""Manual Source Root texture-path actualization for on-disk materials."""

from __future__ import annotations

import json
import os

from ..core.material_source import (
    prepare_material_export,
    write_material_payload_atomic,
)
from ..core.source_resolver import (
    assert_source_snapshot_stable,
    resolve_resource,
    scan_source_root,
)
from ..core.texture_actualize import (
    actualize_material_document,
    assert_texture_tree_stable,
    capture_texture_tree,
)
from .source_manifest import (
    abandon_staged_manifest,
    commit_staged_manifest,
    prepare_manifest_update,
    stage_manifest,
)

EXPORTER_VERSION = "0.5.0"

__all__ = ["actualize_texture_paths"]


def _material_uids(snapshot) -> tuple[str, ...]:
    return tuple(sorted(
        uid for uid, entries in snapshot.owners.items()
        if any(row.get("kind") == "material" for _manifest, row in entries)))


def _guard(resolver_snapshot, texture_snapshot) -> None:
    assert_source_snapshot_stable(resolver_snapshot)
    assert_texture_tree_stable(texture_snapshot)


def _result_row(uid: str, name: str, slot: str, result) -> dict:
    row = {
        "material_uid": uid,
        "material_name": name,
        "slot": slot,
        "basename": result.basename,
        "path": result.authored_path,
    }
    if result.code is not None:
        row["code"] = result.code
    if result.resolved_path is not None:
        row["resolved_path"] = result.resolved_path
    if result.candidates:
        row["candidates"] = list(result.candidates)
    return row


def actualize_texture_paths(
        source_root: str, *, registry_path: str | None = None,
        texture_policy: str = "transitional") -> dict:
    """Repair uniquely resolvable stale texture paths in all v1 materials.

    Source Schema v1 permits one resource upsert per marker transaction, so a
    mass run commits materials one at a time. Each commit gets a fresh global
    manifest snapshot and is guarded by both that snapshot and the immutable
    basename-index pathname set. A failure therefore leaves at most the
    current material's ordinary fail-closed recovery marker.
    """
    root = os.path.normpath(os.path.abspath(source_root))
    texture_snapshot = capture_texture_tree(root)
    inventory_snapshot = scan_source_root(
        root, registry_path=registry_path, texture_policy=texture_policy)
    uids = _material_uids(inventory_snapshot)
    assert_source_snapshot_stable(inventory_snapshot)
    assert_texture_tree_stable(texture_snapshot)

    report = {
        "ok": True,
        "source_root": root,
        "materials_scanned": len(uids),
        "materials_updated": [],
        "fixed": [],
        "ambiguous": [],
        "missing": [],
        "exact": 0,
        "warnings": [],
    }
    for diagnostic in inventory_snapshot.diagnostics:
        report["warnings"].append({
            "code": diagnostic.code,
            "subjects": ([diagnostic.uid] if diagnostic.uid else []),
            "message": diagnostic.message,
        })

    for uid in uids:
        # Earlier iterations legitimately changed manifest bytes. Re-scan so
        # the next resource transaction starts from the committed authority.
        snapshot = scan_source_root(
            root, registry_path=registry_path, texture_policy=texture_policy)
        if _material_uids(snapshot) != uids:
            raise RuntimeError(
                "MH_E_INVALID_EXPORT_MANIFEST: material owner set changed "
                "during texture actualization")
        owner = resolve_resource(snapshot, uid, expected_kind="material")
        if owner is None:  # Defensive: the stable UID inventory says it exists.
            raise RuntimeError(
                f"MH_E_UNRESOLVED_EXTERNAL: material {uid} disappeared")
        document = json.loads(
            snapshot.payload_bytes[owner.payload_path].decode("utf-8"))
        updated, results = actualize_material_document(
            document, texture_snapshot)
        fixes = [(slot, result) for slot, result in results
                 if result.status == "fixed"]

        if fixes:
            prepared = prepare_material_export(
                uid=updated["uid"],
                name=updated["name"],
                shader_class=updated["shader_class"],
                params=updated["params"],
                textures=updated["textures"],
                source_root=root,
                texture_policy=texture_policy,
                target_payload_path=owner.payload_path,
                owning_manifest_path=owner.owning_manifest_path,
                existing_source=owner.manifest_row["source"],
            )
            owner_dir = os.path.dirname(owner.owning_manifest_path)
            manifest = prepare_manifest_update(
                owner_dir,
                resources=[prepared.resource_row],
                exporter_version=EXPORTER_VERSION,
                source_root=root,
            )
            stage_manifest(
                owner_dir, manifest,
                snapshot_guard=lambda current=snapshot: _guard(
                    current, texture_snapshot),
            )
            try:
                write_material_payload_atomic(
                    prepared,
                    force=manifest.is_recovery,
                    source_root=root,
                    texture_policy=texture_policy,
                )
                commit_staged_manifest(owner_dir)
            except BaseException:
                abandon_staged_manifest(owner_dir)
                raise
            report["materials_updated"].append(uid)

        for slot, result in results:
            if result.status == "exact":
                report["exact"] += 1
                continue
            row = _result_row(uid, document["name"], slot, result)
            report[result.status].append(row)
            if result.code is not None:
                report["warnings"].append({
                    "code": result.code,
                    "subjects": [uid],
                    "message": (
                        f"{document['name']}.{slot}: {result.basename}: "
                        + ("multiple basename candidates; path unchanged"
                           if result.status == "ambiguous"
                           else "texture was not found under Source Root")),
                })

    assert_texture_tree_stable(texture_snapshot)
    return report
