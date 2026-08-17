"""Blender-free global Source Schema v1 manifest resolver.

The registry is only a candidate hint. Authority always comes from a complete
scan of every exact ``export_manifest.json`` basename below ``source_root``.
One resolved UID is represented by its payload path, owning manifest path and
the exact normalized manifest row that supplied ownership.
"""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
from typing import Iterable
import uuid

from .material_source import MaterialSourceError, validate_material_document
from ..scene.source_manifest import (
    MANIFEST_NAME,
    PENDING_MANIFEST_NAME,
    ManifestError,
    validate_export_manifest,
    validate_relative_path,
)


class ResolverError(ManifestError):
    """A global source snapshot cannot safely resolve a resource."""


@dataclass(frozen=True)
class ResolverDiagnostic:
    code: str
    message: str
    uid: str | None = None


@dataclass(frozen=True)
class ResolvedResource:
    payload_path: str
    owning_manifest_path: str
    manifest_row: dict


@dataclass(frozen=True)
class WriterResolution:
    """Owner lookup plus the snapshot/recovery state used by a writer."""

    owner: ResolvedResource | None
    snapshot: "SourceSnapshot"
    recovery: bool
    pending_manifest_path: str | None = None


@dataclass(frozen=True)
class PendingWriterResource:
    """The one resource upsert proven by the only pending marker under root."""

    uid: str
    kind: str
    source: str
    pending_manifest_path: str


@dataclass
class SourceSnapshot:
    source_root: str
    texture_policy: str
    manifest_bytes: dict[str, bytes]
    manifests: dict[str, dict]
    owners: dict[str, tuple[tuple[str, dict], ...]]
    registry_path: str | None = None
    registry_bytes: bytes | None = None
    registry_hints: dict[str, dict] = field(default_factory=dict)
    diagnostics: list[ResolverDiagnostic] = field(default_factory=list)
    payload_bytes: dict[str, bytes] = field(default_factory=dict)
    missing_payload_paths: set[str] = field(default_factory=set)
    recovery_marker_path: str | None = None
    recovery_marker_bytes: bytes | None = None


def _error(code: str, message: str) -> None:
    raise ResolverError(message, code)


def _canonical_root(source_root: str) -> str:
    if not isinstance(source_root, str) or not source_root:
        _error("MH_E_INVALID_EXPORT_MANIFEST", "source_root must be an absolute directory")
    if not os.path.isabs(source_root):
        _error("MH_E_INVALID_EXPORT_MANIFEST", "source_root must be absolute")
    root = os.path.normpath(os.path.abspath(source_root))
    if not os.path.isdir(root):
        _error("MH_E_INVALID_EXPORT_MANIFEST", f"source_root is not a directory: {root}")
    return root


def _path_key(path: str) -> str:
    return os.path.normcase(os.path.normpath(os.path.abspath(path)))


def _under_root(root: str, path: str, owner: str, *,
                owning_directory: str | None = None) -> str:
    absolute = os.path.normpath(os.path.abspath(path))
    try:
        inside = os.path.normcase(os.path.commonpath([root, absolute])) \
            == os.path.normcase(root)
    except ValueError:
        inside = False
    # Payload ownership may not escape through a directory symlink/junction.
    real_root = os.path.realpath(root)
    real_path = os.path.realpath(absolute)
    try:
        real_inside = os.path.normcase(os.path.commonpath([real_root, real_path])) \
            == os.path.normcase(real_root)
    except ValueError:
        real_inside = False
    if not inside or not real_inside:
        _error("MH_E_INVALID_RESOURCE_SOURCE", f"{owner} escapes source_root")
    if owning_directory is not None:
        real_owner = os.path.realpath(owning_directory)
        try:
            inside_owner = os.path.normcase(os.path.commonpath([
                real_owner, real_path])) == os.path.normcase(real_owner)
        except ValueError:
            inside_owner = False
        if not inside_owner:
            _error(
                "MH_E_INVALID_RESOURCE_SOURCE",
                f"{owner} escapes its owning manifest directory")
    return absolute


def _inventory(root: str, ignored_pending_paths: frozenset[str]) -> tuple[str, ...]:
    paths = []
    for directory, dirs, files in os.walk(root, followlinks=False):
        dirs.sort()
        files.sort()
        for filename in files:
            path = os.path.join(directory, filename)
            if filename == PENDING_MANIFEST_NAME:
                _under_root(root, path, "pending manifest marker")
                if _path_key(path) not in ignored_pending_paths:
                    _error(
                        "MH_E_INVALID_EXPORT_MANIFEST",
                        f"pending marker blocks source snapshot: {path}")
            elif filename == MANIFEST_NAME:
                _under_root(root, path, "owning manifest")
                paths.append(os.path.normpath(path))
    return tuple(sorted(paths, key=lambda value: value.replace("\\", "/").encode("utf-8")))


def _pending_inventory(root: str) -> tuple[str, ...]:
    paths = []
    for directory, dirs, files in os.walk(root, followlinks=False):
        dirs.sort()
        files.sort()
        for filename in files:
            if filename != PENDING_MANIFEST_NAME:
                continue
            path = os.path.join(directory, filename)
            _under_root(root, path, "pending manifest marker")
            paths.append(os.path.normpath(path))
    return tuple(sorted(
        paths, key=lambda value: value.replace("\\", "/").encode("utf-8")))


def _read_bytes(path: str) -> bytes:
    try:
        with open(path, "rb") as stream:
            return stream.read()
    except OSError as exc:
        _error("MH_E_INVALID_EXPORT_MANIFEST", f"cannot read {path}: {exc}")


def _decode_manifest(payload: bytes, path: str) -> dict:
    try:
        document = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        _error("MH_E_INVALID_EXPORT_MANIFEST", f"cannot parse {path}: {exc}")
    return validate_export_manifest(document)


def _resolve_registry_path(root: str, registry_path: str | None) -> str | None:
    if registry_path is None:
        return None
    if not isinstance(registry_path, str) or not registry_path:
        _error("MH_E_INVALID_EXPORT_MANIFEST", "registry_path must be a path or None")
    path = registry_path if os.path.isabs(registry_path) else os.path.join(root, registry_path)
    return _under_root(root, path, "registry_path")


def _read_optional(path: str | None) -> bytes | None:
    if path is None:
        return None
    try:
        with open(path, "rb") as stream:
            return stream.read()
    except FileNotFoundError:
        return None
    except OSError as exc:
        _error("MH_E_INVALID_EXPORT_MANIFEST", f"cannot read registry {path}: {exc}")


def _validate_registry_path(value: object, owner: str) -> str:
    return validate_relative_path(value, owner)


def _registry_hints(payload: bytes | None, path: str | None,
                    diagnostics: list[ResolverDiagnostic]) -> dict[str, dict]:
    if path is None:
        return {}
    if payload is None:
        diagnostics.append(ResolverDiagnostic(
            "MH_W_REGISTRY_INVALID", f"configured registry not found: {path}"))
        return {}
    try:
        document = json.loads(payload.decode("utf-8"))
        if not isinstance(document, dict):
            raise ValueError("registry must be a JSON object")
        if document.get("schema") != "mh.registry":
            raise ValueError("schema must be 'mh.registry'")
        version = document.get("schema_version")
        if not isinstance(version, int) or isinstance(version, bool) or version != 1:
            raise ValueError("schema_version must be integer 1")
        rows = document.get("resources")
        if not isinstance(rows, list):
            raise ValueError("resources must be an array")
        result = {}
        for index, raw in enumerate(rows):
            owner = f"registry resources[{index}]"
            if not isinstance(raw, dict):
                raise ValueError(f"{owner} must be an object")
            allowed = {"uid", "kind", "name", "source_path", "manifest_path"}
            required = {"uid", "kind", "name", "source_path"}
            if set(raw) - allowed or required - set(raw):
                raise ValueError(f"{owner} has invalid fields")
            uid = raw["uid"]
            kind = raw["kind"]
            name = raw["name"]
            if not isinstance(uid, str) or not isinstance(kind, str) \
                    or not isinstance(name, str) or not name:
                raise ValueError(f"{owner} has invalid uid/kind/name")
            try:
                canonical_uid = str(uuid.UUID(uid))
            except (ValueError, AttributeError) as exc:
                raise ValueError(f"{owner}.uid is not a UUID") from exc
            if canonical_uid != uid:
                raise ValueError(f"{owner}.uid is not canonical lowercase UUID")
            if kind not in {"static_mesh", "composite", "material"}:
                raise ValueError(f"{owner} has unsupported kind {kind!r}")
            if uid in result:
                raise ValueError(f"registry contains duplicate uid {uid}")
            hint = {
                "uid": uid,
                "kind": kind,
                "name": name,
                "source_path": _validate_registry_path(
                    raw["source_path"], f"{owner}.source_path"),
            }
            if "manifest_path" in raw:
                hint["manifest_path"] = _validate_registry_path(
                    raw["manifest_path"], f"{owner}.manifest_path")
            result[uid] = hint
        return result
    except (UnicodeDecodeError, json.JSONDecodeError, ValueError, ManifestError) as exc:
        diagnostics.append(ResolverDiagnostic(
            "MH_W_REGISTRY_INVALID", f"registry ignored: {exc}"))
        return {}


def _scan_source_root(
        source_root: str, *, registry_path: str | None = None,
        texture_policy: str = "transitional",
        ignored_pending_paths: Iterable[str] = (),
        ) -> SourceSnapshot:
    """Capture a stable full-manifest snapshot below one absolute source root."""
    root = _canonical_root(source_root)
    if texture_policy not in {"transitional", "strict"}:
        _error(
            "MH_E_INVALID_MATERIAL_VALUE",
            "texture_policy must be 'transitional' or 'strict'")
    ignored = frozenset(_path_key(path) for path in ignored_pending_paths)
    paths = _inventory(root, ignored)
    first = {path: _read_bytes(path) for path in paths}
    registry = _resolve_registry_path(root, registry_path)
    registry_first = _read_optional(registry)

    # A second complete inventory/read closes ordinary appearance, removal and
    # replacement races before the snapshot is exposed to a consumer.
    if _inventory(root, ignored) != paths:
        _error("MH_E_INVALID_EXPORT_MANIFEST", "manifest set changed while scanning")
    second = {path: _read_bytes(path) for path in paths}
    if second != first:
        _error("MH_E_INVALID_EXPORT_MANIFEST", "manifest bytes changed while scanning")
    if _read_optional(registry) != registry_first:
        _error("MH_E_INVALID_EXPORT_MANIFEST", "registry changed while scanning")

    manifests = {path: _decode_manifest(payload, path)
                 for path, payload in first.items()}
    owners_mutable: dict[str, list[tuple[str, dict]]] = {}
    uid8_owners: dict[str, str] = {}
    payload_owners: dict[str, tuple[str, str]] = {}
    for manifest_path, document in manifests.items():
        directory = os.path.dirname(manifest_path)
        for row in document["resources"]:
            uid = row["uid"]
            uid8 = uid[:8]
            previous_uid = uid8_owners.get(uid8)
            if previous_uid is not None and previous_uid != uid:
                _error(
                    "MH_E_UID8_COLLISION",
                    f"UIDs {previous_uid} and {uid} share uid8 {uid8}")
            uid8_owners[uid8] = uid
            sources = [(row["source"], "source")]
            sources.extend((lod["source"], f"LOD {lod['level']} source")
                           for lod in row.get("lods", []))
            for source, source_label in sources:
                payload_path = _under_root(
                    root, os.path.join(directory, *source.split("/")),
                    f"resource {uid} {source_label}",
                    owning_directory=directory)
                payload_key = _path_key(os.path.realpath(payload_path))
                previous_owner = payload_owners.get(payload_key)
                current_owner = (uid, manifest_path, source_label)
                if previous_owner is not None and previous_owner != current_owner:
                    _error(
                        "MH_E_INVALID_RESOURCE_SOURCE",
                        f"payload {payload_path} is shared by "
                        f"{previous_owner[0]} and {uid}")
                payload_owners[payload_key] = current_owner
            owners_mutable.setdefault(uid, []).append((manifest_path, row))

    diagnostics: list[ResolverDiagnostic] = []
    hints = _registry_hints(registry_first, registry, diagnostics)
    owners = {uid: tuple(entries) for uid, entries in owners_mutable.items()}
    return SourceSnapshot(
        source_root=root,
        texture_policy=texture_policy,
        manifest_bytes=first,
        manifests=manifests,
        owners=owners,
        registry_path=registry,
        registry_bytes=registry_first,
        registry_hints=hints,
        diagnostics=diagnostics,
    )


def scan_source_root(
        source_root: str, *, registry_path: str | None = None,
        texture_policy: str = "transitional",
        ) -> SourceSnapshot:
    """Capture a fail-closed snapshot; no pending marker may be bypassed."""
    return _scan_source_root(
        source_root, registry_path=registry_path,
        texture_policy=texture_policy)


def _capture_writer_recovery(
        root: str, *, registry_path: str | None,
        texture_policy: str,
        ) -> tuple[str, dict, SourceSnapshot] | None:
    markers = _pending_inventory(root)
    if not markers:
        return None
    if len(markers) != 1:
        _error(
            "MH_E_INVALID_EXPORT_MANIFEST",
            "writer recovery requires exactly one pending marker under "
            f"source_root; found {len(markers)}")
    marker_path = markers[0]
    first = _read_bytes(marker_path)
    if _pending_inventory(root) != markers:
        _error(
            "MH_E_INVALID_EXPORT_MANIFEST",
            "pending marker set changed while preparing writer recovery")
    second = _read_bytes(marker_path)
    if first != second:
        _error(
            "MH_E_INVALID_EXPORT_MANIFEST",
            "pending marker changed while preparing writer recovery")
    marker = _decode_manifest(first, marker_path)
    snapshot = _scan_source_root(
        root, registry_path=registry_path,
        texture_policy=texture_policy,
        ignored_pending_paths=[marker_path],
    )
    if _pending_inventory(root) != markers or _read_bytes(marker_path) != first:
        _error(
            "MH_E_INVALID_EXPORT_MANIFEST",
            "pending marker changed during full writer recovery scan")
    snapshot.recovery_marker_path = marker_path
    snapshot.recovery_marker_bytes = first
    return marker_path, marker, snapshot


def _validate_writer_request(uid: str, expected_kind: str,
                             expected_source: str | None) -> str | None:
    try:
        canonical_uid = str(uuid.UUID(uid))
    except (ValueError, AttributeError) as exc:
        _error("MH_E_INVALID_EXPORT_MANIFEST", f"writer UID is invalid: {exc}")
    if canonical_uid != uid:
        _error(
            "MH_E_INVALID_EXPORT_MANIFEST",
            "writer UID must be canonical lowercase UUID")
    if expected_kind not in {"static_mesh", "composite", "material"}:
        _error(
            "MH_E_INVALID_EXPORT_MANIFEST",
            f"writer expected_kind is unsupported: {expected_kind!r}")
    if expected_source is None:
        return None
    return validate_relative_path(expected_source, "writer expected_source")


def _recovery_marker_row(
        snapshot: SourceSnapshot, marker_path: str, marker: dict,
        uid: str, expected_kind: str, expected_source: str | None,
        ) -> ResolvedResource:
    marker_rows = {row["uid"]: row for row in marker["resources"]}
    row = marker_rows.get(uid)
    if row is None:
        _error(
            "MH_E_INVALID_EXPORT_MANIFEST",
            f"pending marker does not contain selected writer UID {uid}")
    if row["kind"] != expected_kind:
        _error(
            "MH_E_INVALID_EXPORT_MANIFEST",
            f"pending marker resource {uid} is {row['kind']}, "
            f"expected {expected_kind}")
    if expected_source is not None and row["source"] != expected_source:
        _error(
            "MH_E_INVALID_RESOURCE_SOURCE",
            f"pending marker resource {uid} source {row['source']!r} does not "
            f"match expected {expected_source!r}")

    marker_directory = os.path.dirname(marker_path)
    owning_manifest_path = _under_root(
        snapshot.source_root,
        os.path.join(marker_directory, MANIFEST_NAME),
        f"resource {uid} owning manifest")
    stable = snapshot.manifests.get(owning_manifest_path)
    stable_entries = snapshot.owners.get(uid, ())
    if len(stable_entries) > 1:
        manifests = ", ".join(path for path, _row in stable_entries)
        _error(
            "MH_E_AMBIGUOUS_RESOURCE_OWNER",
            f"resource {uid} is owned by multiple manifests: {manifests}")
    if stable_entries and stable_entries[0][0] != owning_manifest_path:
        _error(
            "MH_E_AMBIGUOUS_RESOURCE_OWNER",
            f"pending marker would duplicate existing owner "
            f"{stable_entries[0][0]} for resource {uid}")

    if stable is None:
        if set(marker_rows) != {uid}:
            _error(
                "MH_E_INVALID_EXPORT_MANIFEST",
                "first-create recovery marker may contain only selected UID")
    else:
        stable_rows = {item["uid"]: item for item in stable["resources"]}
        changed = {
            item_uid for item_uid in stable_rows.keys() | marker_rows.keys()
            if stable_rows.get(item_uid) != marker_rows.get(item_uid)
        }
        if changed != {uid}:
            _error(
                "MH_E_INVALID_EXPORT_MANIFEST",
                "recovery marker must differ from stable manifest by exactly "
                f"selected UID {uid}; changed={sorted(changed)}")
        stable_without_selected = deepcopy(stable)
        marker_without_selected = deepcopy(marker)
        stable_without_selected["exporter_version"] = marker_without_selected[
            "exporter_version"]
        stable_without_selected["resources"] = [
            item for item in stable_without_selected["resources"]
            if item["uid"] != uid]
        marker_without_selected["resources"] = [
            item for item in marker_without_selected["resources"]
            if item["uid"] != uid]
        if stable_without_selected != marker_without_selected:
            _error(
                "MH_E_INVALID_EXPORT_MANIFEST",
                "recovery marker changed unrelated manifest data")
        stable_row = stable_rows.get(uid)
        if stable_row is not None:
            if stable_row["kind"] != row["kind"]:
                _error(
                    "MH_E_INVALID_EXPORT_MANIFEST",
                    f"resource {uid} recovery changed kind")
            if stable_row["source"] != row["source"]:
                _error(
                    "MH_E_INVALID_RESOURCE_SOURCE",
                    f"resource {uid} recovery changed existing source")

    for existing_uid in snapshot.owners:
        if existing_uid != uid and existing_uid[:8] == uid[:8]:
            _error(
                "MH_E_UID8_COLLISION",
                f"UIDs {existing_uid} and {uid} share uid8 {uid[:8]}")
    recovery_sources = [(row["source"], "source")]
    recovery_sources.extend((lod["source"], f"LOD {lod['level']} source")
                            for lod in row.get("lods", []))
    recovery_paths = []
    for recovery_source, label in recovery_sources:
        recovery_paths.append(_under_root(
            snapshot.source_root,
            os.path.join(marker_directory, *recovery_source.split("/")),
            f"resource {uid} recovery {label}",
            owning_directory=marker_directory))
    payload_path = recovery_paths[0]
    recovery_path_keys = {
        _path_key(os.path.realpath(path)) for path in recovery_paths}
    if len(recovery_path_keys) != len(recovery_paths):
        _error(
            "MH_E_INVALID_RESOURCE_SOURCE",
            f"resource {uid} recovery payload/LOD paths alias each other")
    for existing_uid, entries in snapshot.owners.items():
        if existing_uid == uid:
            continue
        for existing_manifest, existing_row in entries:
            existing_directory = os.path.dirname(existing_manifest)
            existing_sources = [existing_row["source"]]
            existing_sources.extend(
                lod["source"] for lod in existing_row.get("lods", []))
            for existing_source in existing_sources:
                existing_path = _under_root(
                    snapshot.source_root,
                    os.path.join(
                        existing_directory, *existing_source.split("/")),
                    f"resource {existing_uid} source",
                    owning_directory=existing_directory)
                if _path_key(os.path.realpath(existing_path)) in recovery_path_keys:
                    _error(
                        "MH_E_INVALID_RESOURCE_SOURCE",
                        f"recovery payload {payload_path} is already owned by "
                        f"resource {existing_uid}")
    return ResolvedResource(
        payload_path=payload_path,
        owning_manifest_path=owning_manifest_path,
        manifest_row=deepcopy(row),
    )


def pending_writer_uid(
        source_root: str, *, expected_kind: str | None = None,
        ) -> PendingWriterResource | None:
    """Identify the one valid pending resource upsert without message parsing.

    ``None`` means the root has no pending marker. Multiple markers, malformed
    markers, deletion-only diffs, or a marker changing more than one resource
    are hard errors. The returned UID/kind has already passed the same stable
    diff, owner, UID8 and source proof used by explicit writer recovery.
    """
    if (expected_kind is not None
            and expected_kind not in {"static_mesh", "composite", "material"}):
        _error(
            "MH_E_INVALID_EXPORT_MANIFEST",
            f"writer expected_kind is unsupported: {expected_kind!r}")
    root = _canonical_root(source_root)
    recovery_state = _capture_writer_recovery(
        root, registry_path=None, texture_policy="transitional")
    if recovery_state is None:
        return None
    marker_path, marker, snapshot = recovery_state
    owning_manifest_path = os.path.normpath(os.path.join(
        os.path.dirname(marker_path), MANIFEST_NAME))
    stable = snapshot.manifests.get(owning_manifest_path)
    marker_rows = {row["uid"]: row for row in marker["resources"]}
    if stable is None:
        if len(marker_rows) != 1:
            _error(
                "MH_E_INVALID_EXPORT_MANIFEST",
                "first-create recovery marker must contain one resource upsert")
        uid = next(iter(marker_rows))
    else:
        stable_rows = {row["uid"]: row for row in stable["resources"]}
        changed = {
            uid for uid in stable_rows.keys() | marker_rows.keys()
            if stable_rows.get(uid) != marker_rows.get(uid)
        }
        if len(changed) != 1:
            _error(
                "MH_E_INVALID_EXPORT_MANIFEST",
                "pending marker must contain exactly one changed resource "
                f"upsert; changed={sorted(changed)}")
        uid = next(iter(changed))
        if uid not in marker_rows:
            _error(
                "MH_E_INVALID_EXPORT_MANIFEST",
                f"pending marker change for {uid} is a deletion, not an upsert")
    row = marker_rows[uid]
    kind = row["kind"]
    if expected_kind is not None and kind != expected_kind:
        _error(
            "MH_E_INVALID_EXPORT_MANIFEST",
            f"pending writer resource {uid} is {kind}, "
            f"expected {expected_kind}")
    _recovery_marker_row(
        snapshot, marker_path, marker, uid, kind, expected_source=None)
    return PendingWriterResource(
        uid=uid,
        kind=kind,
        source=row["source"],
        pending_manifest_path=marker_path,
    )


def resolve_resource_for_writer(
        source_root: str, uid: str, *, expected_kind: str,
        expected_source: str | None = None,
        registry_path: str | None = None,
        texture_policy: str = "transitional",
        allow_missing_lod_payloads: bool = False,
        ) -> WriterResolution:
    """Resolve one writer owner, allowing only its one explicit crash marker.

    With no marker this is exactly ``scan_source_root`` plus
    ``resolve_resource``. With one marker, recovery is admitted only when that
    valid marker contains the selected UID and all unrelated stable data is
    unchanged. Payload existence is deliberately not required in recovery: a
    crash may have happened before or after payload replacement, and the writer
    must replace it unconditionally before committing the marker.

    An LOD-aware writer may set ``allow_missing_lod_payloads`` to repair an
    owned mesh whose authored LOD payloads are absent. Existing LOD payloads
    are still captured as opaque stable bytes, and absent paths are captured
    as absence tokens. The default remains strict for writers which do not
    prove that they will replace every missing authored LOD.
    """
    expected_source = _validate_writer_request(
        uid, expected_kind, expected_source)
    root = _canonical_root(source_root)
    recovery_state = _capture_writer_recovery(
        root, registry_path=registry_path,
        texture_policy=texture_policy)
    if recovery_state is None:
        snapshot = scan_source_root(
            root, registry_path=registry_path,
            texture_policy=texture_policy)
        owner = _resolve_resource_for_writer_normal(
            snapshot, uid, expected_kind=expected_kind,
            expected_source=expected_source,
            allow_missing_lod_payloads=allow_missing_lod_payloads)
        return WriterResolution(owner, snapshot, False)
    marker_path, marker, snapshot = recovery_state
    owner = _recovery_marker_row(
        snapshot, marker_path, marker, uid, expected_kind, expected_source)
    return WriterResolution(
        owner, snapshot, True, pending_manifest_path=marker_path)


def _resolve_resource_for_writer_normal(
        snapshot: SourceSnapshot, uid: str, *, expected_kind: str,
        expected_source: str | None,
        allow_missing_lod_payloads: bool = False,
        ) -> ResolvedResource | None:
    """Resolve an owned payload as rewrite input, without trusting its schema.

    The manifest owner remains strict and authoritative. A regular primary
    payload is captured as opaque stable bytes; an absent primary is captured
    as an absence token. The standalone writer will decide whether to rewrite
    or hash-skip it. Non-file entries remain hard errors. Authored LOD absence
    is a hard error unless the calling writer explicitly opts into repairing
    missing LOD payloads.
    """
    entries = snapshot.owners.get(uid, ())
    hint = snapshot.registry_hints.get(uid)
    if not entries:
        if hint is not None:
            _append_diagnostic_once(snapshot, ResolverDiagnostic(
                "MH_W_REGISTRY_STALE",
                "registry hint has no owning manifest in full scan", uid))
        return None
    if len(entries) > 1:
        manifests = ", ".join(path for path, _row in entries)
        _error(
            "MH_E_AMBIGUOUS_RESOURCE_OWNER",
            f"resource {uid} is owned by multiple manifests: {manifests}")
    manifest_path, row = entries[0]
    if row["kind"] != expected_kind:
        _error(
            "MH_E_UNRESOLVED_EXTERNAL",
            f"resource {uid} is {row['kind']}, expected {expected_kind}")
    if expected_source is not None and row["source"] != expected_source:
        _error(
            "MH_E_INVALID_RESOURCE_SOURCE",
            f"resource {uid} existing source {row['source']!r} does not match "
            f"expected {expected_source!r}")
    payload_path = _under_root(
        snapshot.source_root,
        os.path.join(os.path.dirname(manifest_path), *row["source"].split("/")),
        f"resource {uid} source",
        owning_directory=os.path.dirname(manifest_path))
    if os.path.lexists(payload_path):
        if not os.path.isfile(payload_path):
            _error(
                "MH_E_UNRESOLVED_EXTERNAL",
                f"resource {uid} payload path is not a file: {payload_path}")
        first = _read_bytes(payload_path)
        second = _read_bytes(payload_path)
        if first != second:
            _error(
                "MH_E_INVALID_EXPORT_MANIFEST",
                f"payload changed while reading: {payload_path}")
        snapshot.payload_bytes[payload_path] = first
    else:
        snapshot.missing_payload_paths.add(payload_path)
    _validate_lod_payloads(
        snapshot, manifest_path, row,
        allow_missing=allow_missing_lod_payloads)
    result = ResolvedResource(
        payload_path=payload_path,
        owning_manifest_path=manifest_path,
        manifest_row=deepcopy(row),
    )
    if hint is not None and not _hint_is_current(snapshot, hint, result):
        _append_diagnostic_once(snapshot, ResolverDiagnostic(
            "MH_W_REGISTRY_STALE",
            "registry hint disagrees with full manifest scan; scan result used",
            uid))
    return result


def _append_diagnostic_once(snapshot: SourceSnapshot,
                            diagnostic: ResolverDiagnostic) -> None:
    if diagnostic not in snapshot.diagnostics:
        snapshot.diagnostics.append(diagnostic)


def _hint_is_current(snapshot: SourceSnapshot, hint: dict,
                     result: ResolvedResource) -> bool:
    row = result.manifest_row
    payload_relative = Path(result.payload_path).relative_to(
        snapshot.source_root).as_posix()
    if (hint["kind"] != row["kind"] or hint["name"] != row["name"]
            or hint["source_path"] != payload_relative):
        return False
    manifest_hint = hint.get("manifest_path")
    if manifest_hint is None:
        return True
    manifest_relative = Path(result.owning_manifest_path).relative_to(
        snapshot.source_root).as_posix()
    return manifest_hint == manifest_relative


def _validate_payload_identity(snapshot: SourceSnapshot, payload: bytes,
                               path: str, row: dict) -> None:
    if row["kind"] == "static_mesh":
        return
    try:
        document = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        _error("MH_E_UNRESOLVED_EXTERNAL", f"cannot parse payload {path}: {exc}")
    expected_schema = "mh.composite" if row["kind"] == "composite" else "mh.material"
    if not isinstance(document, dict) or document.get("schema") != expected_schema:
        _error("MH_E_UNRESOLVED_EXTERNAL", f"payload {path} has wrong schema")
    version = document.get("schema_version")
    if not isinstance(version, int) or isinstance(version, bool) or version != 1:
        _error("MH_E_UNKNOWN_SCHEMA_VERSION", f"payload {path} has unsupported version")
    if document.get("uid") != row["uid"] or document.get("name") != row["name"]:
        _error(
            "MH_E_UNRESOLVED_EXTERNAL",
            f"payload {path} UID/name does not match owning manifest row")
    if row["kind"] == "material":
        try:
            _normalized, diagnostics = validate_material_document(
                document,
                source_root=snapshot.source_root,
                texture_policy=snapshot.texture_policy,
            )
        except MaterialSourceError as exc:
            _error(exc.code, f"invalid material payload {path}: {exc}")
        for diagnostic in diagnostics:
            _append_diagnostic_once(snapshot, ResolverDiagnostic(
                diagnostic.code,
                f"{path}: {diagnostic.path}: {diagnostic.message}",
                row["uid"],
            ))


def resolve_resource(
        snapshot: SourceSnapshot, uid: str, *, expected_kind: str | None = None,
        ) -> ResolvedResource | None:
    """Resolve one UID once against a full snapshot; zero owners returns None."""
    entries = snapshot.owners.get(uid, ())
    hint = snapshot.registry_hints.get(uid)
    if not entries:
        if hint is not None:
            _append_diagnostic_once(snapshot, ResolverDiagnostic(
                "MH_W_REGISTRY_STALE",
                "registry hint has no owning manifest in full scan", uid))
        return None
    if len(entries) > 1:
        manifests = ", ".join(path for path, _row in entries)
        _error(
            "MH_E_AMBIGUOUS_RESOURCE_OWNER",
            f"resource {uid} is owned by multiple manifests: {manifests}")
    manifest_path, row = entries[0]
    if expected_kind is not None and row["kind"] != expected_kind:
        _error(
            "MH_E_UNRESOLVED_EXTERNAL",
            f"resource {uid} is {row['kind']}, expected {expected_kind}")
    payload_path = _under_root(
        snapshot.source_root,
        os.path.join(os.path.dirname(manifest_path), *row["source"].split("/")),
        f"resource {uid} source",
        owning_directory=os.path.dirname(manifest_path))
    if not os.path.isfile(payload_path):
        _error("MH_E_UNRESOLVED_EXTERNAL", f"resource {uid} payload is missing: {payload_path}")
    first = _read_bytes(payload_path)
    second = _read_bytes(payload_path)
    if first != second:
        _error("MH_E_INVALID_EXPORT_MANIFEST", f"payload changed while reading: {payload_path}")
    _validate_payload_identity(snapshot, first, payload_path, row)
    snapshot.payload_bytes[payload_path] = first
    _validate_lod_payloads(snapshot, manifest_path, row)
    result = ResolvedResource(
        payload_path=payload_path,
        owning_manifest_path=manifest_path,
        manifest_row=deepcopy(row),
    )
    if hint is not None and not _hint_is_current(snapshot, hint, result):
        _append_diagnostic_once(snapshot, ResolverDiagnostic(
            "MH_W_REGISTRY_STALE",
            "registry hint disagrees with full manifest scan; scan result used",
            uid))
    return result


def _validate_lod_payloads(
        snapshot: SourceSnapshot, manifest_path: str, row: dict, *,
        allow_missing: bool = False,
        ) -> None:
    """Require and snapshot every authored LOD payload for a mesh row."""
    uid = row["uid"]
    for lod in row.get("lods", []):
        lod_path = _under_root(
            snapshot.source_root,
            os.path.join(
                os.path.dirname(manifest_path), *lod["source"].split("/")),
            f"resource {uid} LOD {lod['level']} source",
            owning_directory=os.path.dirname(manifest_path))
        if os.path.lexists(lod_path):
            if not os.path.isfile(lod_path):
                _error(
                    "MH_E_UNRESOLVED_EXTERNAL",
                    f"resource {uid} LOD {lod['level']} payload path is not "
                    f"a file: {lod_path}")
        elif allow_missing:
            snapshot.missing_payload_paths.add(lod_path)
            continue
        else:
            _error(
                "MH_E_UNRESOLVED_EXTERNAL",
                f"resource {uid} LOD {lod['level']} payload is missing: "
                f"{lod_path}")
        lod_first = _read_bytes(lod_path)
        lod_second = _read_bytes(lod_path)
        if lod_first != lod_second:
            _error(
                "MH_E_INVALID_EXPORT_MANIFEST",
                f"LOD payload changed while reading: {lod_path}")
        snapshot.payload_bytes[lod_path] = lod_first


def resolve_resource_for_import(
        snapshot: SourceSnapshot, uid: str, *,
        expected_kind: str | None = None,
        ) -> ResolvedResource | None:
    """Resolve an import reference while preserving a missing-payload owner.

    A manifest row remains authoritative enough to build an unresolved Blender
    placeholder even when its primary payload file is absent. All ordinary
    owner, kind, source-containment and payload validation remains strict; only
    primary payload absence is admitted. The absent path is added to the
    snapshot guard so a file appearing during import invalidates the operation.
    """
    entries = snapshot.owners.get(uid, ())
    hint = snapshot.registry_hints.get(uid)
    if not entries:
        if hint is not None:
            _append_diagnostic_once(snapshot, ResolverDiagnostic(
                "MH_W_REGISTRY_STALE",
                "registry hint has no owning manifest in full scan", uid))
        return None
    if len(entries) > 1:
        manifests = ", ".join(path for path, _row in entries)
        _error(
            "MH_E_AMBIGUOUS_RESOURCE_OWNER",
            f"resource {uid} is owned by multiple manifests: {manifests}")
    manifest_path, row = entries[0]
    if expected_kind is not None and row["kind"] != expected_kind:
        _error(
            "MH_E_UNRESOLVED_EXTERNAL",
            f"resource {uid} is {row['kind']}, expected {expected_kind}")
    payload_path = _under_root(
        snapshot.source_root,
        os.path.join(os.path.dirname(manifest_path), *row["source"].split("/")),
        f"resource {uid} source",
        owning_directory=os.path.dirname(manifest_path))
    if os.path.lexists(payload_path):
        if not os.path.isfile(payload_path):
            _error(
                "MH_E_UNRESOLVED_EXTERNAL",
                f"resource {uid} payload path is not a file: {payload_path}")
        return resolve_resource(
            snapshot, uid, expected_kind=expected_kind)

    _validate_lod_payloads(snapshot, manifest_path, row)
    result = ResolvedResource(
        payload_path=payload_path,
        owning_manifest_path=manifest_path,
        manifest_row=deepcopy(row),
    )
    snapshot.missing_payload_paths.add(payload_path)
    if hint is not None and not _hint_is_current(snapshot, hint, result):
        _append_diagnostic_once(snapshot, ResolverDiagnostic(
            "MH_W_REGISTRY_STALE",
            "registry hint disagrees with full manifest scan; scan result used",
            uid))
    return result


def resolve_resources(
        snapshot: SourceSnapshot,
        requests: Iterable[tuple[str, str | None]],
        ) -> dict[str, ResolvedResource | None]:
    """Resolve a request wave with UID deduplication."""
    expected_by_uid = {}
    for uid, expected_kind in requests:
        previous_expected = expected_by_uid.get(uid)
        if (uid in expected_by_uid and previous_expected != expected_kind
                and previous_expected is not None and expected_kind is not None):
            _error(
                "MH_E_UNRESOLVED_EXTERNAL",
                f"resource {uid} requested with conflicting expected kinds "
                f"{previous_expected!r} and {expected_kind!r}")
        if uid not in expected_by_uid or previous_expected is None:
            expected_by_uid[uid] = expected_kind
    return {
        uid: resolve_resource(snapshot, uid, expected_kind=expected_kind)
        for uid, expected_kind in expected_by_uid.items()
    }


def assert_source_snapshot_stable(snapshot: SourceSnapshot) -> None:
    """Recheck manifest set/bytes, registry and every consumed payload."""
    expected_paths = tuple(snapshot.manifest_bytes)
    recovery_path = snapshot.recovery_marker_path
    recovery_bytes = snapshot.recovery_marker_bytes
    ignored = frozenset({_path_key(recovery_path)}) \
        if recovery_path is not None else frozenset()
    expected_markers = (recovery_path,) if recovery_path is not None else ()
    if _pending_inventory(snapshot.source_root) != expected_markers:
        _error(
            "MH_E_INVALID_EXPORT_MANIFEST",
            "pending marker set changed after snapshot")
    if _inventory(snapshot.source_root, ignored) != expected_paths:
        _error("MH_E_INVALID_EXPORT_MANIFEST", "manifest set changed after snapshot")
    first_manifests = {
        path: _read_bytes(path) for path in snapshot.manifest_bytes}
    first_recovery = _read_optional(recovery_path)
    first_registry = _read_optional(snapshot.registry_path)
    first_payloads = {
        path: _read_bytes(path) for path in snapshot.payload_bytes}
    first_missing = {
        path: os.path.lexists(path) for path in snapshot.missing_payload_paths}
    if _pending_inventory(snapshot.source_root) != expected_markers:
        _error(
            "MH_E_INVALID_EXPORT_MANIFEST",
            "pending marker set changed after snapshot")
    if _inventory(snapshot.source_root, ignored) != expected_paths:
        _error("MH_E_INVALID_EXPORT_MANIFEST", "manifest set changed after snapshot")
    second_manifests = {
        path: _read_bytes(path) for path in snapshot.manifest_bytes}
    second_recovery = _read_optional(recovery_path)
    second_registry = _read_optional(snapshot.registry_path)
    second_payloads = {
        path: _read_bytes(path) for path in snapshot.payload_bytes}
    second_missing = {
        path: os.path.lexists(path) for path in snapshot.missing_payload_paths}

    for path, token in snapshot.manifest_bytes.items():
        if first_manifests[path] != token or second_manifests[path] != token:
            _error("MH_E_INVALID_EXPORT_MANIFEST", f"manifest changed after snapshot: {path}")
    if first_recovery != recovery_bytes or second_recovery != recovery_bytes:
        _error("MH_E_INVALID_EXPORT_MANIFEST", "recovery marker changed after snapshot")
    if (first_registry != snapshot.registry_bytes
            or second_registry != snapshot.registry_bytes):
        _error("MH_E_INVALID_EXPORT_MANIFEST", "registry changed after snapshot")
    for path, token in snapshot.payload_bytes.items():
        if first_payloads[path] != token or second_payloads[path] != token:
            _error("MH_E_INVALID_EXPORT_MANIFEST", f"payload changed after snapshot: {path}")
    for path in snapshot.missing_payload_paths:
        if first_missing[path] or second_missing[path]:
            _error(
                "MH_E_INVALID_EXPORT_MANIFEST",
                f"missing payload appeared after snapshot: {path}")
