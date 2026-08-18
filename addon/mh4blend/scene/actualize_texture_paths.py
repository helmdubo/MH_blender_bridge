"""Manual Source Root texture-path actualization for on-disk materials."""

from __future__ import annotations

import json
import os

from ..core.material_source import (
    prepare_material_export,
    validate_material_document,
    write_material_payload_atomic,
)
from ..core.texture_actualize import (
    actualize_material_document,
    assert_texture_tree_stable,
    capture_texture_tree,
)

EXPORTER_VERSION = "0.6.0"

__all__ = ["actualize_texture_paths"]


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
        source_root: str, *, texture_policy: str = "transitional") -> dict:
    """Repair uniquely resolvable paths in every self-contained material."""
    root = os.path.normpath(os.path.abspath(source_root))
    texture_snapshot = capture_texture_tree(root)
    material_paths = tuple(sorted(
        path for path in texture_snapshot.files
        if path.lower().endswith(".material")))
    assert_texture_tree_stable(texture_snapshot)

    report = {
        "ok": True,
        "source_root": root,
        "materials_scanned": len(material_paths),
        "materials_updated": [],
        "fixed": [],
        "ambiguous": [],
        "missing": [],
        "exact": 0,
        "warnings": [],
    }
    for material_path in material_paths:
        with open(material_path, "rb") as stream:
            original_bytes = stream.read()
        document, diagnostics = validate_material_document(
            json.loads(original_bytes.decode("utf-8")), source_root=root,
            texture_policy=texture_policy)
        uid = document["uid"]
        report["warnings"].extend(row.disk_dict() for row in diagnostics)
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
                target_payload_path=material_path,
            )
            assert_texture_tree_stable(texture_snapshot)
            def unchanged_material_guard():
                assert_texture_tree_stable(texture_snapshot)
                try:
                    with open(material_path, "rb") as stream:
                        current = stream.read()
                except OSError as exc:
                    raise RuntimeError(
                        "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: material "
                        f"disappeared during actualization: {material_path}"
                    ) from exc
                if current != original_bytes:
                    raise RuntimeError(
                        "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: material "
                        f"changed during actualization: {material_path}")
            write_material_payload_atomic(
                prepared, force=True, source_root=root,
                texture_policy=texture_policy,
                pre_replace_guard=unchanged_material_guard)
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
