"""One-shot migration from frozen manifest-owned v1 sources to v2 payloads.

This module is deliberately isolated from the production resolver and source
index.  It contains the small amount of legacy ``export_manifest.json``
knowledge needed for migration and is never imported by production writers,
the production resolver, or the lazy reader index.  Only the explicit
migration operator imports it.

Migration is fail-closed at source-root scope: every legacy manifest, source
payload, Blender mesh collection and clean-name destination is preflighted
before the first filesystem write.  A legacy manifest is removed only after
all of its rows have produced valid v2 payloads.  Legacy UID-suffixed payloads
are never removed before their clean-name replacements exist.
"""

from __future__ import annotations

import contextlib
from dataclasses import dataclass
import hashlib
import json
import ntpath
import os
from pathlib import Path
import posixpath
import re
import unicodedata
import uuid

import bpy

from ..core.canonical import sanitize_name, validate_resource_name
from ..core.material_source import validate_material_document
from ..core.payload_publish_v2 import atomic_publish_json
from ..core.uid import PROP_UID
from .export_fbx import export_fbx_collection
from .import_composite import _validate_document as _validate_composite_v2

__all__ = [
    "MigrationSourcesV2Error",
    "migrate_sources_v2",
]


MANIFEST_NAME = "export_manifest.json"
PENDING_MANIFEST_NAME = "export_manifest.json.tmp"
RECEIPT_SCHEMA = "mh.sources_v2_migration_receipt"
RECEIPT_SCHEMA_VERSION = 1

_TOP_FIELDS = frozenset({
    "schema", "schema_version", "exporter_version", "resources",
})
_COMMON_ROW_FIELDS = frozenset({
    "uid", "kind", "name", "source", "content_hash",
})
_ROW_FIELDS = {
    "static_mesh": _COMMON_ROW_FIELDS | {
        "material_slots", "properties", "lod_policy", "lods",
    },
    "composite": _COMMON_ROW_FIELDS | {"properties"},
    "material": _COMMON_ROW_FIELDS,
}
_SUFFIXES = {
    "static_mesh": ".mesh.fbx",
    "composite": ".composite",
    "material": ".material",
}
_CONTENT_HASH = re.compile(r"^xxh3:[0-9a-f]{16}$")


class MigrationSourcesV2Error(RuntimeError):
    """An internal migration preflight/commit failure with a stable code."""

    def __init__(self, code: str, message: str):
        self.code = code
        super().__init__(f"{code}: {message}")


@dataclass(frozen=True)
class _ResourcePlan:
    manifest_path: Path
    row: dict
    source_path: Path
    source_digest: str
    additional_sources: tuple[tuple[Path, str], ...]
    target_path: Path
    document: dict | None = None
    collection: object | None = None


@dataclass(frozen=True)
class _ManifestPlan:
    path: Path
    original_bytes: bytes
    resources: tuple[_ResourcePlan, ...]


def _error(code: str, message: str) -> None:
    raise MigrationSourcesV2Error(code, message)


def _pairs_without_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON object key {key!r}")
        result[key] = value
    return result


def _decode_json(payload: bytes, owner: Path) -> dict:
    try:
        document = json.loads(
            payload.decode("utf-8"),
            object_pairs_hook=_pairs_without_duplicates)
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        _error("MH_E_V1_MIGRATION_INVALID", f"cannot parse {owner}: {exc}")
    if not isinstance(document, dict):
        _error("MH_E_V1_MIGRATION_INVALID", f"{owner} is not a JSON object")
    return document


def _read_stable(path: Path) -> bytes:
    try:
        first = path.read_bytes()
        second = path.read_bytes()
    except OSError as exc:
        _error("MH_E_V1_MIGRATION_INVALID", f"cannot read {path}: {exc}")
    if first != second:
        _error("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED", f"file changed: {path}")
    return first


def _digest_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        _error("MH_E_V1_MIGRATION_INVALID", f"cannot hash {path}: {exc}")
    return digest.hexdigest()


def _canonical_uid(value, owner: str) -> str:
    if not isinstance(value, str):
        _error("MH_E_V1_MIGRATION_INVALID", f"{owner} must be a UUID")
    try:
        canonical = str(uuid.UUID(value))
    except (ValueError, AttributeError, TypeError) as exc:
        _error("MH_E_V1_MIGRATION_INVALID", f"{owner} is not a UUID: {exc}")
    if canonical != value:
        _error(
            "MH_E_V1_MIGRATION_INVALID",
            f"{owner} must use canonical lowercase UUID spelling")
    return value


def _json_bag(value, owner: str) -> dict:
    if not isinstance(value, dict):
        _error("MH_E_V1_MIGRATION_INVALID", f"{owner} must be an object")
    try:
        json.dumps(value, ensure_ascii=False, allow_nan=False)
    except (TypeError, ValueError) as exc:
        _error("MH_E_V1_MIGRATION_INVALID", f"{owner} is not finite JSON: {exc}")
    return value


def _relative_source(value, owner: str) -> str:
    if not isinstance(value, str) or not value or "\x00" in value:
        _error("MH_E_INVALID_RESOURCE_SOURCE", f"{owner} is not a path")
    normalized = unicodedata.normalize("NFC", value)
    drive, _tail = ntpath.splitdrive(normalized)
    canonical = posixpath.normpath(normalized)
    if (normalized != value or drive or normalized.startswith(("/", "\\"))
            or "\\" in normalized or canonical in {"", ".", ".."}
            or canonical.startswith("../") or canonical != normalized):
        _error(
            "MH_E_INVALID_RESOURCE_SOURCE",
            f"{owner} must be a normalized relative forward-slash path")
    return value


def _validate_lods(value, uid: str) -> list[dict]:
    if not isinstance(value, list):
        _error("MH_E_V1_MIGRATION_INVALID", f"static_mesh {uid}.lods must be an array")
    levels = set()
    for index, row in enumerate(value):
        owner = f"static_mesh {uid}.lods[{index}]"
        if not isinstance(row, dict) or set(row) != {
                "level", "source", "content_hash"}:
            _error(
                "MH_E_V1_MIGRATION_INVALID",
                f"{owner} must contain level/source/content_hash")
        level = row["level"]
        if (not isinstance(level, int) or isinstance(level, bool)
                or level < 1 or level in levels):
            _error("MH_E_V1_MIGRATION_INVALID", f"{owner}.level is invalid")
        levels.add(level)
        source = _relative_source(row["source"], f"{owner}.source")
        expected = f"__{uid[:8]}.lod{level}.mesh.fbx"
        if not posixpath.basename(source).endswith(expected):
            _error(
                "MH_E_INVALID_RESOURCE_SOURCE",
                f"{owner}.source must end with {expected!r}")
        if not isinstance(row["content_hash"], str) \
                or not _CONTENT_HASH.fullmatch(row["content_hash"]):
            _error(
                "MH_E_V1_MIGRATION_INVALID",
                f"{owner}.content_hash is invalid")
    if list(levels) and levels != set(range(1, max(levels) + 1)):
        _error(
            "MH_E_LOD_LEVELS_SPARSE",
            f"static_mesh {uid}.lods must contain contiguous levels from 1")
    return list(value)


def _validate_manifest(document: dict, path: Path) -> list[dict]:
    if set(document) != _TOP_FIELDS:
        _error(
            "MH_E_INVALID_EXPORT_MANIFEST",
            f"{path} fields differ; missing={sorted(_TOP_FIELDS - set(document))}, "
            f"unknown={sorted(set(document) - _TOP_FIELDS)}")
    if document.get("schema") != "mh.export_manifest":
        _error("MH_E_INVALID_EXPORT_MANIFEST", f"{path} has wrong schema")
    version = document.get("schema_version")
    if not isinstance(version, int) or isinstance(version, bool) or version != 1:
        _error("MH_E_UNKNOWN_SCHEMA_VERSION", f"{path} is not schema_version 1")
    if not isinstance(document.get("exporter_version"), str) \
            or not document["exporter_version"]:
        _error(
            "MH_E_INVALID_EXPORT_MANIFEST",
            f"{path}.exporter_version must be a non-empty string")
    rows = document.get("resources")
    if not isinstance(rows, list):
        _error("MH_E_INVALID_EXPORT_MANIFEST", f"{path}.resources is not an array")

    normalized = []
    for index, raw in enumerate(rows):
        owner = f"{path}.resources[{index}]"
        if not isinstance(raw, dict):
            _error("MH_E_INVALID_EXPORT_MANIFEST", f"{owner} is not an object")
        uid = _canonical_uid(raw.get("uid"), f"{owner}.uid")
        kind = raw.get("kind")
        if kind not in _ROW_FIELDS:
            _error("MH_E_INVALID_EXPORT_MANIFEST", f"{owner}.kind is invalid")
        missing = _COMMON_ROW_FIELDS - set(raw)
        unknown = set(raw) - _ROW_FIELDS[kind]
        if missing or unknown:
            _error(
                "MH_E_INVALID_EXPORT_MANIFEST",
                f"{owner} fields differ; missing={sorted(missing)}, "
                f"unknown={sorted(unknown)}")
        name = raw.get("name")
        try:
            validate_resource_name(name)
        except (TypeError, ValueError) as exc:
            _error("MH_E_NON_ASCII_RESOURCE_NAME", f"{owner}.name: {exc}")
        source = _relative_source(raw.get("source"), f"{owner}.source")
        expected = f"__{uid[:8]}{_SUFFIXES[kind]}"
        if not posixpath.basename(source).endswith(expected):
            _error(
                "MH_E_INVALID_RESOURCE_SOURCE",
                f"{owner}.source must end with {expected!r}")
        content_hash = raw.get("content_hash")
        if not isinstance(content_hash, str) or not _CONTENT_HASH.fullmatch(content_hash):
            _error("MH_E_INVALID_EXPORT_MANIFEST", f"{owner}.content_hash is invalid")
        if "properties" in raw:
            _json_bag(raw["properties"], f"{owner}.properties")
        if kind == "static_mesh":
            if "lod_policy" in raw and raw["lod_policy"] not in {
                    "authored", "generated", "nanite"}:
                _error("MH_E_INVALID_EXPORT_MANIFEST", f"{owner}.lod_policy is invalid")
            if "lods" in raw:
                _validate_lods(raw["lods"], uid)
            if "material_slots" in raw:
                slots = raw["material_slots"]
                if not isinstance(slots, list):
                    _error("MH_E_INVALID_EXPORT_MANIFEST", f"{owner}.material_slots is invalid")
                for slot_index, slot in enumerate(slots):
                    slot_owner = f"{owner}.material_slots[{slot_index}]"
                    if not isinstance(slot, dict) or set(slot) != {
                            "slot_name", "material_uid"}:
                        _error("MH_E_INVALID_EXPORT_MANIFEST", f"{slot_owner} is invalid")
                    if not isinstance(slot["slot_name"], str) or not slot["slot_name"]:
                        _error("MH_E_INVALID_EXPORT_MANIFEST", f"{slot_owner}.slot_name is invalid")
                    _canonical_uid(slot["material_uid"], f"{slot_owner}.material_uid")
        normalized.append(dict(raw))

    uids = [row["uid"] for row in normalized]
    if len(uids) != len(set(uids)) or uids != sorted(uids):
        _error(
            "MH_E_INVALID_EXPORT_MANIFEST",
            f"{path}.resources must be UID-sorted and unique")
    return normalized


def _resolve_payload(owner: Path, root: Path, source: str, label: str) -> Path:
    candidate = owner.joinpath(*source.split("/"))
    try:
        resolved_root = root.resolve(strict=True)
        resolved_owner = owner.resolve(strict=True)
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(resolved_root)
        resolved.relative_to(resolved_owner)
    except (OSError, ValueError) as exc:
        _error(
            "MH_E_INVALID_RESOURCE_SOURCE",
            f"{label} escapes its owner/source_root or is missing: {candidate}: {exc}")
    if candidate.is_symlink() or not candidate.is_file():
        _error(
            "MH_E_INVALID_RESOURCE_SOURCE",
            f"{label} is not a regular non-symlink file: {candidate}")
    return candidate.absolute()


def _collections_by_uid(uid: str) -> list:
    return [collection for collection in bpy.data.collections
            if collection.get(PROP_UID) == uid]


def _fbx_dry_run(collection, output_dir: Path, root: Path) -> dict:
    """Run the production planner without persisting lazy UID assignment.

    ``export_fbx_collection(dry_run=True)`` intentionally exercises the same
    extraction/hash/passport path as a real export, and that production path
    lazily assigns IDs to Blender data.  Migration preflight must be read-only,
    so this adapter restores the exact presence/value of every possibly
    affected UID property afterwards.
    """
    carriers = []
    for dataset_name in ("collections", "objects", "meshes", "materials"):
        carriers.extend(getattr(bpy.data, dataset_name))
    snapshot = []
    for carrier in carriers:
        present = PROP_UID in carrier.keys()
        snapshot.append((carrier, present, carrier.get(PROP_UID)))
    try:
        return export_fbx_collection(
            collection, output_dir, dry_run=True, source_root=root)
    finally:
        for carrier, present, value in snapshot:
            with contextlib.suppress(ReferenceError):
                if present:
                    carrier[PROP_UID] = value
                elif PROP_UID in carrier.keys():
                    del carrier[PROP_UID]


def _material_plan(manifest_path: Path, row: dict, source: Path,
                   source_digest: str, root: Path) -> _ResourcePlan:
    raw = _decode_json(_read_stable(source), source)
    document, _diagnostics = validate_material_document(
        raw, source_root=root, texture_policy="transitional")
    if document["uid"] != row["uid"] or document["name"] != row["name"]:
        _error(
            "MH_E_V1_MIGRATION_IDENTITY_MISMATCH",
            f"material {source} identity disagrees with its manifest row")
    target = source.with_name(sanitize_name(document["name"]) + ".material")
    return _ResourcePlan(
        manifest_path, row, source, source_digest, (), target,
        document=document)


def _composite_plan(manifest_path: Path, row: dict, source: Path,
                    source_digest: str) -> _ResourcePlan:
    raw = _decode_json(_read_stable(source), source)
    expected_fields = {"schema", "schema_version", "uid", "name", "nodes"}
    version = raw.get("schema_version")
    if (set(raw) != expected_fields or raw.get("schema") != "mh.composite"
            or not isinstance(version, int) or isinstance(version, bool)
            or version != 1):
        _error(
            "MH_E_V1_MIGRATION_INVALID",
            f"legacy composite {source} is not exact mh.composite v1")
    if raw.get("uid") != row["uid"] or raw.get("name") != row["name"]:
        _error(
            "MH_E_V1_MIGRATION_IDENTITY_MISMATCH",
            f"composite {source} identity disagrees with its manifest row")
    document = {
        "schema": "mh.composite",
        "schema_version": 2,
        "uid": raw["uid"],
        "name": raw["name"],
        "properties": row.get("properties", {}),
        "nodes": raw["nodes"],
    }
    target = source.with_name(sanitize_name(document["name"]) + ".composite")
    _validate_composite_v2(document, str(target))
    return _ResourcePlan(
        manifest_path, row, source, source_digest, (), target,
        document=document)


def _mesh_plan(manifest_path: Path, row: dict, source: Path,
               source_digest: str, additional_sources: tuple[tuple[Path, str], ...],
               root: Path) -> _ResourcePlan:
    owners = _collections_by_uid(row["uid"])
    if len(owners) != 1:
        detail = "missing" if not owners else f"ambiguous ({len(owners)} collections)"
        _error(
            "MH_E_MIGRATION_MESH_COLLECTION_NOT_FOUND",
            f"static_mesh {row['uid']} Blender Collection is {detail}")
    collection = owners[0]
    report = _fbx_dry_run(collection, source.parent, root)
    if not report.get("ok"):
        _error(
            "MH_E_V1_MIGRATION_FAILED",
            f"static_mesh {row['uid']} failed export validation: "
            f"{report.get('validation')}")
    if report.get("passport", {}).get("resource_uid") != row["uid"]:
        _error(
            "MH_E_V1_MIGRATION_IDENTITY_MISMATCH",
            f"static_mesh {row['uid']} dry-run passport has another UID")
    passport = report["passport"]
    expected_slots = sorted(
        row.get("material_slots", []), key=lambda item: item["slot_name"])
    actual_slots = sorted(({
        "slot_name": item["slot_name"],
        "material_uid": item["material_uid"],
    } for item in passport["material_slots"]),
        key=lambda item: item["slot_name"])
    expected_lod_policy = row.get(
        "lod_policy", "authored" if row.get("lods") else "generated")
    descriptor_mismatches = []
    if passport["name"] != row["name"]:
        descriptor_mismatches.append("name")
    if passport["properties"] != row.get("properties", {}):
        descriptor_mismatches.append("properties")
    if actual_slots != expected_slots:
        descriptor_mismatches.append("material_slots")
    if passport["lod_policy"] != expected_lod_policy:
        descriptor_mismatches.append("lod_policy")
    if descriptor_mismatches:
        _error(
            "MH_E_V1_MIGRATION_IDENTITY_MISMATCH",
            f"static_mesh {row['uid']} Blender descriptor disagrees with "
            "legacy manifest fields: " + ", ".join(descriptor_mismatches))
    if "lods" in row:
        expected_levels = [0] + sorted(item["level"] for item in row["lods"])
        actual_levels = report.get("passport", {}).get("lod_levels")
        if actual_levels != expected_levels:
            _error(
                "MH_E_LOD_PASSPORT_MISMATCH",
                f"static_mesh {row['uid']} loaded Collection exports levels "
                f"{actual_levels!r}, legacy manifest declares {expected_levels!r}")
    target = Path(report["filepath"]).absolute()
    return _ResourcePlan(
        manifest_path, row, source, source_digest, additional_sources, target,
        collection=collection)


def _pending_markers(root: Path) -> list[Path]:
    return sorted(
        (path for path in root.rglob(PENDING_MANIFEST_NAME)
         if path.exists() or path.is_symlink()),
        key=lambda path: str(path).casefold())


def _path_key(path: Path) -> str:
    return os.path.normcase(os.path.realpath(os.fspath(path)))


def _failure(exc: BaseException, *, path: Path | None = None,
             uid: str | None = None) -> dict:
    return {
        "code": getattr(exc, "code", "MH_E_V1_MIGRATION_FAILED"),
        "message": str(exc),
        **({"path": str(path)} if path is not None else {}),
        **({"uid": uid} if uid is not None else {}),
    }


def _empty_receipt(root: Path, dry_run: bool) -> dict:
    return {
        "schema": RECEIPT_SCHEMA,
        "schema_version": RECEIPT_SCHEMA_VERSION,
        "source_root": str(root),
        "dry_run": bool(dry_run),
        "ok": False,
        "blocked": False,
        "migrated": [],
        "renamed": [],
        "conflicted": [],
        "failed": [],
        "manifests_removed": [],
        "rollback_instructions": (
            "Before committing migration results, restore deleted legacy "
            "manifests/payloads from VCS if rollback is required."),
    }


def _build_plan(root: Path, receipt: dict) -> list[_ManifestPlan]:
    pending = _pending_markers(root)
    if pending:
        receipt["failed"].extend({
            "code": "MH_E_PENDING_EXPORT_MARKER",
            "message": f"pending legacy writer marker blocks migration: {path}",
            "path": str(path),
        } for path in pending)
        return []

    manifests = sorted(root.rglob(MANIFEST_NAME), key=lambda path: str(path).casefold())
    plans = []
    target_owners: dict[str, _ResourcePlan] = {}
    uid_owners: dict[str, Path] = {}
    for manifest_path in manifests:
        try:
            if manifest_path.is_symlink() or not manifest_path.is_file():
                _error(
                    "MH_E_INVALID_EXPORT_MANIFEST",
                    f"manifest is not a regular non-symlink file: {manifest_path}")
            original = _read_stable(manifest_path)
            rows = _validate_manifest(_decode_json(original, manifest_path), manifest_path)
            resources = []
            for row in rows:
                previous_owner = uid_owners.get(row["uid"])
                if previous_owner is not None:
                    _error(
                        "MH_E_AMBIGUOUS_RESOURCE_OWNER",
                        f"resource {row['uid']} is owned by both "
                        f"{previous_owner} and {manifest_path}")
                uid_owners[row["uid"]] = manifest_path
                source = _resolve_payload(
                    manifest_path.parent, root, row["source"],
                    f"{row['kind']} {row['uid']}.source")
                digest = _digest_file(source)
                if row["kind"] == "material":
                    planned = _material_plan(
                        manifest_path, row, source, digest, root)
                elif row["kind"] == "composite":
                    planned = _composite_plan(
                        manifest_path, row, source, digest)
                else:
                    additional_sources = []
                    for lod in row.get("lods", []):
                        lod_source = _resolve_payload(
                            manifest_path.parent, root, lod["source"],
                            f"static_mesh {row['uid']} LOD {lod['level']}.source")
                        additional_sources.append(
                            (lod_source, _digest_file(lod_source)))
                    planned = _mesh_plan(
                        manifest_path, row, source, digest,
                        tuple(additional_sources), root)
                if _path_key(planned.target_path) == _path_key(source):
                    _error(
                        "MH_E_V1_MIGRATION_INVALID",
                        f"legacy source unexpectedly already has clean path: {source}")
                if os.path.lexists(planned.target_path):
                    _error(
                        "MH_E_NAME_COLLISION_DIFFERENT_UID",
                        f"clean migration target is occupied: {planned.target_path}")
                key = _path_key(planned.target_path)
                other = target_owners.get(key)
                if other is not None:
                    _error(
                        "MH_E_NAME_COLLISION_DIFFERENT_UID",
                        f"two legacy rows map to {planned.target_path}: "
                        f"{other.row['uid']} and {row['uid']}")
                target_owners[key] = planned
                resources.append(planned)
            plans.append(_ManifestPlan(
                manifest_path.absolute(), original, tuple(resources)))
        except BaseException as exc:
            failure = _failure(exc, path=manifest_path)
            receipt["failed"].append(failure)
            if failure["code"] in {
                    "MH_E_NAME_COLLISION_DIFFERENT_UID",
                    "MH_E_AMBIGUOUS_RESOURCE_OWNER"}:
                receipt["conflicted"].append(dict(failure))
    return plans


def _assert_plan_stable(root: Path, plans: list[_ManifestPlan]) -> None:
    pending = _pending_markers(root)
    if pending:
        _error(
            "MH_E_PENDING_EXPORT_MARKER",
            f"pending marker appeared after preflight: {pending[0]}")
    for manifest in plans:
        if _read_stable(manifest.path) != manifest.original_bytes:
            _error(
                "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
                f"manifest changed after preflight: {manifest.path}")
        for resource in manifest.resources:
            if _digest_file(resource.source_path) != resource.source_digest:
                _error(
                    "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
                    f"legacy payload changed after preflight: {resource.source_path}")
            for path, digest in resource.additional_sources:
                if _digest_file(path) != digest:
                    _error(
                        "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
                        f"legacy LOD payload changed after preflight: {path}")
            if os.path.lexists(resource.target_path):
                _error(
                    "MH_E_NAME_COLLISION_DIFFERENT_UID",
                    f"clean target appeared after preflight: {resource.target_path}")
            if resource.row["kind"] == "static_mesh":
                owners = _collections_by_uid(resource.row["uid"])
                if len(owners) != 1 or owners[0] != resource.collection:
                    _error(
                        "MH_E_MIGRATION_MESH_COLLECTION_NOT_FOUND",
                        f"static_mesh {resource.row['uid']} collection changed")
                report = _fbx_dry_run(
                    resource.collection, resource.source_path.parent, root)
                if (not report.get("ok")
                        or _path_key(Path(report["filepath"]))
                        != _path_key(resource.target_path)):
                    _error(
                        "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
                        f"static_mesh {resource.row['uid']} export plan changed")


def _assert_legacy_authority_stable(
        root: Path, manifest: _ManifestPlan) -> None:
    """Recheck only legacy authority, including after clean writes exist."""
    pending = _pending_markers(root)
    if pending:
        _error(
            "MH_E_PENDING_EXPORT_MARKER",
            f"pending marker appeared during migration: {pending[0]}")
    if _read_stable(manifest.path) != manifest.original_bytes:
        _error(
            "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
            f"manifest changed during migration: {manifest.path}")
    for resource in manifest.resources:
        legacy = ((resource.source_path, resource.source_digest),
                  *resource.additional_sources)
        for path, digest in legacy:
            if _digest_file(path) != digest:
                _error(
                    "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
                    f"legacy payload changed during migration: {path}")


def _receipt_row(resource: _ResourcePlan, *, dry_run: bool) -> dict:
    return {
        "uid": resource.row["uid"],
        "kind": resource.row["kind"],
        "manifest": str(resource.manifest_path),
        "legacy_payload": str(resource.source_path),
        "legacy_additional_payloads": [
            str(path) for path, _digest in resource.additional_sources],
        "payload": str(resource.target_path),
        "status": "would_migrate" if dry_run else "migrated",
    }


def _write_resource(resource: _ResourcePlan, root: Path) -> None:
    kind = resource.row["kind"]
    if kind == "static_mesh":
        report = export_fbx_collection(
            resource.collection, resource.source_path.parent,
            dry_run=False, source_root=root)
        if not report.get("ok") or not report.get("written"):
            _error(
                "MH_E_V1_MIGRATION_FAILED",
                f"static_mesh {resource.row['uid']} was not written")
        if _path_key(Path(report["filepath"])) != _path_key(resource.target_path):
            _error(
                "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
                f"static_mesh {resource.row['uid']} wrote an unexpected path")
    else:
        atomic_publish_json(
            resource.target_path, resource.document,
            source_root=root,
            pre_replace_guard=lambda: (
                _error(
                    "MH_E_NAME_COLLISION_DIFFERENT_UID",
                    f"clean target appeared during migration: {resource.target_path}")
                if os.path.lexists(resource.target_path) else None))


def migrate_sources_v2(source_root, *, dry_run: bool = False) -> dict:
    """Migrate every frozen-v1 owner manifest below ``source_root``.

    The returned receipt always contains ``migrated``, ``renamed`` and
    ``failed`` arrays.  Global preflight failures return ``blocked=True`` and
    guarantee that no filesystem write was attempted.  ``dry_run`` performs
    the same complete preflight (including the production FBX writer's dry
    run) and reports the intended operations without touching the source tree.
    """
    root = Path(bpy.path.abspath(os.fspath(source_root))).absolute()
    if root.is_symlink() or not root.is_dir():
        raise ValueError(
            f"source_root must be an existing non-symlink directory: {root}")
    receipt = _empty_receipt(root, dry_run)
    plans = _build_plan(root, receipt)
    if receipt["failed"]:
        receipt["blocked"] = True
        return receipt

    try:
        _assert_plan_stable(root, plans)
    except BaseException as exc:
        receipt["failed"].append(_failure(exc, path=root))
        receipt["blocked"] = True
        return receipt

    all_resources = [resource for manifest in plans for resource in manifest.resources]
    if dry_run:
        receipt["migrated"] = [
            _receipt_row(resource, dry_run=True) for resource in all_resources]
        receipt["renamed"] = [{
            "uid": resource.row["uid"],
            "from": str(resource.source_path),
            "to": str(resource.target_path),
        } for resource in all_resources]
        receipt["manifests_removed"] = [
            {"path": str(plan.path), "status": "would_remove"}
            for plan in plans]
        receipt["ok"] = True
        return receipt

    cleanup_failures = []
    for manifest in plans:
        created = []
        try:
            _assert_plan_stable(root, [manifest])
            # Publish the whole replacement set first.  Until this succeeds,
            # the legacy manifest and every UID-suffixed payload stay intact.
            for resource in manifest.resources:
                _write_resource(resource, root)
                created.append(resource)
            _assert_legacy_authority_stable(root, manifest)
        except BaseException as exc:
            # Targets were absent at preflight and are migration-owned.  A
            # failed manifest attempt may safely remove them; legacy authority
            # has not yet been deleted at this point in the normal failure
            # path.  Cleanup errors are retained in the failure message.
            cleanup_errors = []
            for resource in reversed(created):
                try:
                    if resource.target_path.exists():
                        resource.target_path.unlink()
                except OSError as cleanup_exc:
                    cleanup_errors.append(str(cleanup_exc))
            failure = _failure(exc, path=manifest.path)
            if cleanup_errors:
                failure["cleanup_errors"] = cleanup_errors
            receipt["failed"].append(failure)
            receipt["blocked"] = True
            return receipt

        legacy_paths = [
            path
            for resource in manifest.resources
            for path in (
                resource.source_path,
                *(item[0] for item in resource.additional_sources),
            )
        ]
        quarantined = []
        try:
            # Renaming every old payload out of its recognized extension is
            # reversible until the manifest unlink commits the migration.
            for old_path in legacy_paths:
                quarantine = old_path.with_name(
                    f".{old_path.name}.mh-v1-delete-{uuid.uuid4().hex}")
                os.replace(old_path, quarantine)
                quarantined.append((quarantine, old_path))
            manifest.path.unlink()
        except BaseException as exc:
            restore_errors = []
            for quarantine, old_path in reversed(quarantined):
                try:
                    if quarantine.exists():
                        os.replace(quarantine, old_path)
                except OSError as restore_exc:
                    restore_errors.append(str(restore_exc))
            for resource in reversed(created):
                try:
                    if resource.target_path.exists():
                        resource.target_path.unlink()
                except OSError as cleanup_exc:
                    restore_errors.append(str(cleanup_exc))
            failure = _failure(exc, path=manifest.path)
            if restore_errors:
                failure["cleanup_errors"] = restore_errors
            receipt["failed"].append(failure)
            receipt["blocked"] = True
            return receipt

        for quarantine, _old_path in quarantined:
            try:
                quarantine.unlink()
            except OSError as exc:
                cleanup_failures.append({
                    "code": "MH_E_V1_MIGRATION_CLEANUP_FAILED",
                    "message": str(exc),
                    "path": str(quarantine),
                })

        receipt["migrated"].extend(
            _receipt_row(resource, dry_run=False)
            for resource in manifest.resources)
        receipt["renamed"].extend({
            "uid": resource.row["uid"],
            "from": str(resource.source_path),
            "to": str(resource.target_path),
        } for resource in manifest.resources)
        receipt["manifests_removed"].append({
            "path": str(manifest.path), "status": "removed"})

    receipt["failed"].extend(cleanup_failures)
    receipt["ok"] = not cleanup_failures
    return receipt
