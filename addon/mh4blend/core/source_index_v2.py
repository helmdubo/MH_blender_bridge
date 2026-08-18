"""Host-local passport-first source index for MH Source Protocol v2.

The index is disposable acceleration data.  Authority remains in standalone
payloads: FBX identity comes from equal carrier copies supplied by a host
callback, while ``.material`` and ``.composite`` identity is read from their
JSON envelopes.  This module never reads or writes export manifests/markers.
"""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import sys
from typing import Callable, Iterable, Sequence
import unicodedata
import uuid

from .canonical import validate_resource_name
from .fbx_passport import (
    descriptor_hash as fbx_descriptor_hash,
    parse_passport_text,
)
from .payload_publish_v2 import atomic_publish_json, canonical_payload_path

__all__ = [
    "FbxPassportExtractor",
    "INDEX_SCHEMA",
    "INDEX_SCHEMA_VERSION",
    "PayloadObservation",
    "SourceInventoryToken",
    "ResourceStabilityToken",
    "ResolvedResourceV2",
    "SourceIndexV2Error",
    "assert_resource_stable",
    "assert_source_inventory_stable",
    "canonical_source_root",
    "capture_resource_stability_token",
    "capture_source_inventory",
    "default_index_path",
    "fork_document",
    "load_index",
    "project_uid_for_source_root",
    "rebuild_and_publish_index",
    "rebuild_index",
    "resolve_resource_index_first",
    "scan_source_root",
]


INDEX_SCHEMA = "mh.local_index"
INDEX_SCHEMA_VERSION = 1
PASSPORT_SCHEMA = "mh.fbx_passport"
PASSPORT_SCHEMA_VERSION = 1
PROJECT_NAMESPACE = uuid.uuid5(
    uuid.NAMESPACE_URL, "https://mimirhead.studio/mhbridge/source-index-v2")
PROVISIONAL_EMPTY_REBUILD_NOTE = (
    "No prior resolution existed; identical copies were provisionally "
    "resolved by canonical path order. Host policy may replace this choice.")

_MATERIAL_FIELDS = frozenset({
    "schema", "schema_version", "uid", "name", "shader_class", "params",
    "textures",
})
_COMPOSITE_V2_FIELDS = frozenset({
    "schema", "schema_version", "uid", "name", "properties", "nodes",
})
_PAYLOAD_SUFFIXES = (".mesh.fbx", ".material", ".composite")

FbxPassportExtractor = Callable[[Path], Sequence[str]]


class SourceIndexV2Error(RuntimeError):
    """A fail-closed local-index/resolution error."""

    def __init__(self, code: str, message: str):
        self.code = code
        super().__init__(f"{code}: {message}")


@dataclass(frozen=True)
class PayloadObservation:
    path: Path
    payload_fingerprint: str
    size: int
    mtime_ns: int
    fingerprint_fast_path: bool
    parse_status: str
    parsed_passport: dict | None
    descriptor_hash: str | None
    diagnostics: tuple[str, ...] = ()


@dataclass(frozen=True)
class ResolvedResourceV2:
    resource_uid: str
    payload_path: str
    kind: str
    parsed_passport: dict
    descriptor_hash: str
    payload_fingerprint: str
    resolution_status: str
    diagnostics: tuple[str, ...]
    resolution_source: str
    index: dict


@dataclass(frozen=True)
class ResourceStabilityToken:
    resource_uid: str
    payload_path: str
    kind: str
    size: int
    mtime_ns: int
    payload_fingerprint: str
    descriptor_hash: str


@dataclass(frozen=True)
class SourceInventoryToken:
    source_root: str
    payload_paths: tuple[str, ...]


def _nfc(value):
    if isinstance(value, str):
        return unicodedata.normalize("NFC", value)
    if isinstance(value, list):
        return [_nfc(item) for item in value]
    if isinstance(value, dict):
        normalized = {_nfc(key): _nfc(item) for key, item in value.items()}
        if len(normalized) != len(value):
            raise ValueError("JSON keys collide after Unicode NFC")
        return normalized
    return value


def _canonical_json_bytes(value) -> bytes:
    return json.dumps(
        _nfc(value), ensure_ascii=False, allow_nan=False, sort_keys=True,
        separators=(",", ":")).encode("utf-8")


def _sha256_bytes(payload: bytes) -> str:
    return "sha256:" + hashlib.sha256(payload).hexdigest()


def _descriptor_hash(document: dict) -> str:
    descriptor = deepcopy(document)
    descriptor.pop("descriptor_hash", None)
    return _sha256_bytes(_canonical_json_bytes(descriptor))


def canonical_source_root(source_root: str | os.PathLike) -> str:
    root = canonical_payload_path(source_root)
    if not os.path.isdir(root):
        raise SourceIndexV2Error(
            "MH_E_SOURCE_INDEX_INVALID",
            f"source_root is not an existing directory: {root}")
    return root


def project_uid_for_source_root(source_root: str | os.PathLike) -> str:
    """Derive a host-cache namespace only; this UUID is never asset identity."""
    root = canonical_source_root(source_root)
    return str(uuid.uuid5(PROJECT_NAMESPACE, root))


def default_index_path(
        project_uid: str, *, platform_name: str | None = None,
        local_appdata: str | os.PathLike | None = None,
        home: str | os.PathLike | None = None,
        xdg_cache_home: str | os.PathLike | None = None) -> Path:
    try:
        normalized_uid = str(uuid.UUID(project_uid))
    except (ValueError, TypeError, AttributeError) as exc:
        raise ValueError("project_uid must be a UUID") from exc
    platform_name = platform_name or sys.platform
    user_home = Path(home) if home is not None else Path.home()
    if platform_name.startswith("win"):
        base = Path(
            local_appdata if local_appdata is not None else
            os.environ.get("LOCALAPPDATA", user_home / "AppData" / "Local"))
    elif platform_name == "darwin":
        base = user_home / "Library" / "Caches"
    else:
        base = Path(
            xdg_cache_home if xdg_cache_home is not None else
            os.environ.get("XDG_CACHE_HOME", user_home / ".cache"))
    return base / "MimirHead" / "MHBridge" / normalized_uid / "index.json"


def _inside(root: str, path: str) -> bool:
    try:
        return os.path.commonpath([root, path]) == root
    except ValueError:
        return False


def _stat_token(stat_result) -> tuple[int, int, int, int, int]:
    return (
        int(stat_result.st_size), int(stat_result.st_mtime_ns),
        int(getattr(stat_result, "st_ctime_ns", 0)),
        int(getattr(stat_result, "st_dev", 0)),
        int(getattr(stat_result, "st_ino", 0)),
    )


def _content_stat_token(stat_result) -> tuple[int, int]:
    """Portable pathname/handle comparison fields.

    Windows can expose different ``st_ino``/``st_dev`` values for ``fstat``
    and a pathname ``stat`` of the same live file.  Size and nanosecond mtime
    are the cross-platform cache contract; the richer token is still used to
    detect mutation on the same open handle and around host callbacks.
    """
    return int(stat_result.st_size), int(stat_result.st_mtime_ns)


def _read_stable_bytes(path: Path) -> tuple[bytes, os.stat_result]:
    try:
        with path.open("rb") as stream:
            before = os.fstat(stream.fileno())
            payload = stream.read()
            after = os.fstat(stream.fileno())
        current = path.stat()
    except OSError as exc:
        raise SourceIndexV2Error(
            "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
            f"cannot read stable payload {path}: {exc}") from exc
    if (_stat_token(before) != _stat_token(after)
            or _content_stat_token(after) != _content_stat_token(current)
            or len(payload) != current.st_size):
        raise SourceIndexV2Error(
            "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
            f"payload changed while reading: {path}")
    return payload, current


def _normalize_uid(value, field: str) -> str:
    try:
        return str(uuid.UUID(value))
    except (ValueError, TypeError, AttributeError) as exc:
        raise ValueError(f"{field} must be a UUID") from exc


def _canonical_uid(value, field: str) -> str:
    normalized = _normalize_uid(value, field)
    if value != normalized:
        raise ValueError(
            f"{field} must use lowercase canonical UUID spelling with dashes")
    return normalized


def _passport_invalid(message: str):
    prefix = "MH_E_PASSPORT_INVALID: "
    if message.startswith(prefix):
        message = message[len(prefix):]
    return "malformed", None, None, (f"MH_E_PASSPORT_INVALID: {message}",)


def _parse_fbx_passport(copies: Sequence[str]):
    copies = tuple(copies)
    if not copies:
        return (
            "legacy_missing_passport", None, None,
            ("MH_W_LEGACY_PAYLOAD_NO_PASSPORT",))
    if any(not isinstance(copy, str) for copy in copies):
        return _passport_invalid("carrier copies must be strings")
    if len(set(copies)) != 1:
        return _passport_invalid(
            "MESH carrier copies differ byte-for-byte")
    try:
        document = json.loads(copies[0])
    except (json.JSONDecodeError, UnicodeError) as exc:
        return _passport_invalid(str(exc))
    if not isinstance(document, dict):
        return _passport_invalid("root must be an object")
    if document.get("schema") != PASSPORT_SCHEMA:
        return _passport_invalid("unknown schema")
    if document.get("schema_version") != PASSPORT_SCHEMA_VERSION:
        return (
            "unsupported_version", None, None,
            ("MH_E_PASSPORT_INVALID: unsupported schema_version",))
    if "lod_level" in document:
        return (
            "deprecated_per_lod_passport", None, None,
            ("MH_W_DEPRECATED_PER_LOD_PASSPORT_MIGRATION_REQUIRED",))
    try:
        # One production codec owns canonical spelling, exact fields and all
        # semantic constraints.  The index must not become a second, weaker
        # interpretation of Carrier B.
        document = parse_passport_text(copies[0])
        descriptor = fbx_descriptor_hash(document)
    except (ValueError, TypeError) as exc:
        return _passport_invalid(str(exc))
    return "ok", document, descriptor, ()


def _parse_json_identity(payload: bytes, suffix: str):
    try:
        document = json.loads(payload.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        return _passport_invalid(str(exc))
    if not isinstance(document, dict):
        return _passport_invalid("JSON payload root must be an object")
    if suffix == ".material":
        if (document.get("schema") != "mh.material"
                or isinstance(document.get("schema_version"), bool)
                or document.get("schema_version") != 1):
            return _passport_invalid("unsupported mh.material envelope")
        expected = _MATERIAL_FIELDS
        kind = "material"
        status = "ok"
    else:
        if document.get("schema") != "mh.composite":
            return _passport_invalid("unsupported mh.composite envelope")
        version = document.get("schema_version")
        if isinstance(version, bool):
            return _passport_invalid("composite schema_version must be integer")
        if version == 1:
            return (
                "legacy_composite_v1", None, None,
                ("MH_W_LEGACY_COMPOSITE_V1_MIGRATION_REQUIRED",))
        elif version == 2:
            expected = _COMPOSITE_V2_FIELDS
            status = "ok"
        else:
            return (
                "unsupported_version", None, None,
                ("MH_E_PASSPORT_INVALID: unsupported composite version",))
        kind = "composite"
    if set(document) != expected:
        return _passport_invalid(
            f"invalid {suffix} fields; missing={sorted(expected - set(document))}, "
            f"unknown={sorted(set(document) - expected)}")
    try:
        uid = _canonical_uid(document.get("uid"), "uid")
        normalized = _nfc(document)
    except (ValueError, TypeError) as exc:
        return _passport_invalid(str(exc))
    name = normalized.get("name")
    if not isinstance(name, str) or not name:
        return _passport_invalid("name must be a non-empty string")
    try:
        validate_resource_name(name)
    except (TypeError, ValueError) as exc:
        return _passport_invalid(f"invalid resource name: {exc}")
    if suffix == ".material":
        if (not isinstance(normalized.get("shader_class"), str)
                or not normalized["shader_class"]
                or not isinstance(normalized.get("params"), dict)
                or not isinstance(normalized.get("textures"), dict)):
            return _passport_invalid("invalid material semantic payload")
        for slot, texture_path in normalized["textures"].items():
            if (slot not in {f"tex{index}" for index in range(16)}
                    or not isinstance(texture_path, str)
                    or not texture_path):
                return _passport_invalid(
                    "material textures must contain only non-empty "
                    "tex0..tex15 strings")
    else:
        if not isinstance(normalized.get("nodes"), list):
            return _passport_invalid("composite nodes must be an array")
        if normalized["schema_version"] == 2 \
                and not isinstance(normalized.get("properties"), dict):
            return _passport_invalid("composite properties must be an object")
    identity = {
        "schema": normalized["schema"],
        "schema_version": normalized["schema_version"],
        "resource_uid": uid,
        "kind": kind,
        "name": name,
    }
    try:
        descriptor = _descriptor_hash(normalized)
    except (TypeError, ValueError) as exc:
        return _passport_invalid(f"payload is not finite canonical JSON: {exc}")
    return status, identity, descriptor, ()


def _observe_path(
        path: Path, *, cached_row: dict | None = None,
        fbx_passport_extractor: FbxPassportExtractor | None = None,
        source_root: str | None = None,
        force_fingerprint: bool = False) -> PayloadObservation:
    canonical = canonical_payload_path(path)
    if source_root is not None and not _inside(source_root, canonical):
        raise SourceIndexV2Error(
            "MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT",
            f"payload resolves outside source_root: {path}")
    payload, stat_result = _read_stable_bytes(path)
    fast_path = bool(
        not force_fingerprint and cached_row
        and cached_row.get("size") == stat_result.st_size
        and cached_row.get("mtime_ns") == stat_result.st_mtime_ns
        and isinstance(cached_row.get("payload_fingerprint"), str)
        and cached_row["payload_fingerprint"].startswith("sha256:"))
    # Size+mtime selects the cheap cache path, but the exact SHA-256 is still
    # confirmed from this stable byte snapshot before conflict resolution.
    # This catches sync tools or manual edits that preserve both stat fields.
    exact_fingerprint = _sha256_bytes(payload)
    fingerprint = exact_fingerprint
    fast_path = bool(
        fast_path and cached_row["payload_fingerprint"] == exact_fingerprint)
    suffix = path.suffix.lower()
    if suffix == ".fbx":
        if fbx_passport_extractor is None:
            status, parsed, descriptor, diagnostics = (
                "carrier_unavailable", None, None,
                ("MH_W_FBX_CARRIER_READER_UNAVAILABLE",))
        else:
            try:
                before_callback = _stat_token(path.stat())
            except OSError as exc:
                raise SourceIndexV2Error(
                    "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
                    f"payload disappeared before carrier extraction: {path}"
                ) from exc
            try:
                copies = tuple(fbx_passport_extractor(path))
            except Exception as exc:
                status, parsed, descriptor, diagnostics = _passport_invalid(
                    f"FBX carrier extraction failed: {exc}")
            else:
                status, parsed, descriptor, diagnostics = \
                    _parse_fbx_passport(copies)
            try:
                after_callback = _stat_token(path.stat())
            except OSError as exc:
                raise SourceIndexV2Error(
                    "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
                    f"payload disappeared during carrier extraction: {path}"
                ) from exc
            if before_callback != after_callback:
                raise SourceIndexV2Error(
                    "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
                    f"payload changed during carrier extraction: {path}")
    else:
        status, parsed, descriptor, diagnostics = \
            _parse_json_identity(payload, suffix)
    return PayloadObservation(
        Path(canonical), fingerprint, int(stat_result.st_size),
        int(stat_result.st_mtime_ns), fast_path, status, parsed, descriptor,
        tuple(diagnostics))


def _path_row(observation: PayloadObservation, old: dict | None) -> dict:
    diagnostics = list(observation.diagnostics)
    old_parsed = (old or {}).get("parsed_passport")
    authored_semantics_unchanged = bool(
        old and observation.parsed_passport is not None
        and isinstance(old_parsed, dict)
        and old.get("descriptor_hash") == observation.descriptor_hash)
    if (authored_semantics_unchanged
            and observation.parsed_passport.get("kind") == "static_mesh"):
        # geometry_hash is deliberately excluded from descriptor_hash because
        # it has its own authored write trigger. A changed geometry_hash is a
        # legitimate new FBX revision, not an unexplained external byte edit.
        authored_semantics_unchanged = (
            old_parsed.get("geometry_hash")
            == observation.parsed_passport.get("geometry_hash"))
    if (authored_semantics_unchanged
            and old.get("payload_fingerprint")
            != observation.payload_fingerprint):
        diagnostics.append("MH_W_PAYLOAD_EXTERNAL_MODIFIED")
    return {
        "path": str(observation.path),
        "size": observation.size,
        "mtime_ns": observation.mtime_ns,
        "payload_fingerprint": observation.payload_fingerprint,
        "fingerprint_fast_path": observation.fingerprint_fast_path,
        "parse_status": observation.parse_status,
        "parsed_passport": observation.parsed_passport,
        "descriptor_hash": observation.descriptor_hash,
        "diagnostics": diagnostics,
        "quarantined": observation.parse_status in {
            "malformed", "unsupported_version"},
    }


def _prior_resolved(previous_index: dict | None, uid: str) -> str | None:
    if not previous_index:
        return None
    return previous_index.get("uids", {}).get(uid, {}).get("resolved_path")


def rebuild_index(
        observations: Iterable[PayloadObservation], *, source_root: str,
        project_uid: str, previous_index: dict | None = None) -> dict:
    """Apply the v2 UID candidate/conflict matrix to stable observations."""
    path_rows = {}
    grouped: dict[str, list[str]] = {}
    invalidated: dict[str, list[str]] = {}
    legacy_paths = []
    quarantine_paths = []
    previous_paths = (previous_index or {}).get("paths", {})
    for observation in observations:
        key = str(observation.path)
        if key in path_rows:
            raise SourceIndexV2Error(
                "MH_E_SOURCE_INDEX_INVALID",
                f"duplicate canonical payload observation: {key}")
        row = _path_row(observation, previous_paths.get(key))
        path_rows[key] = row
        parsed = observation.parsed_passport
        if parsed is not None:
            grouped.setdefault(parsed["resource_uid"], []).append(key)
        elif row["quarantined"]:
            quarantine_paths.append(key)
            old_parsed = (previous_paths.get(key) or {}).get(
                "parsed_passport")
            if isinstance(old_parsed, dict) and isinstance(
                    old_parsed.get("resource_uid"), str):
                invalidated.setdefault(
                    old_parsed["resource_uid"], []).append(key)
        else:
            legacy_paths.append(key)

    uid_rows = {}
    provisional = []
    previous_uids = (previous_index or {}).get("uids", {})
    for uid, unsorted_candidates in sorted(grouped.items()):
        candidates = sorted(
            unsorted_candidates, key=lambda item: (item.casefold(), item))
        fingerprints = {
            path_rows[path]["payload_fingerprint"] for path in candidates}
        prior = _prior_resolved(previous_index, uid)
        known_before = uid in previous_uids
        chosen = None
        diagnostics = []
        if len(candidates) == 1:
            chosen = candidates[0]
            if prior and prior != chosen and prior not in path_rows:
                status = "moved"
                basis = "single_candidate_after_prior_disappeared"
            else:
                status = "resolved" if known_before else "adopt_new"
                basis = "single_candidate"
        elif len(fingerprints) == 1:
            if prior in candidates:
                chosen = prior
                basis = "preserved_prior_resolved_path"
            else:
                chosen = candidates[0]
                basis = "provisional_canonical_path"
                provisional.append({
                    "resource_uid": uid, "chosen_path": chosen,
                    "note": PROVISIONAL_EMPTY_REBUILD_NOTE,
                })
            status = "duplicate_copy_warning"
            diagnostics.append(
                "MH_W_DUPLICATE_IDENTICAL_PAYLOAD: " + ", ".join(candidates))
        else:
            status = "hard_conflict"
            basis = "divergent_revisions"
            diagnostics.append(
                "MH_E_DIVERGENT_REVISIONS: " + ", ".join(candidates))
        if (chosen is not None
                and "MH_W_PAYLOAD_EXTERNAL_MODIFIED"
                in path_rows[chosen]["diagnostics"]):
            status = "external_modified"
            basis = "external_modification_requires_confirmation"
            diagnostics.append("MH_W_PAYLOAD_EXTERNAL_MODIFIED")
        descriptor_path = chosen or candidates[0]
        descriptor = path_rows[descriptor_path]["parsed_passport"]
        uid_rows[uid] = {
            "candidate_paths": candidates,
            "candidate_fingerprints": sorted(fingerprints),
            "resolved_path": chosen,
            "resolution_basis": basis,
            "resolution_status": status,
            "kind": descriptor["kind"],
            "diagnostics": diagnostics,
        }

    # A previously proven UID whose same live path became malformed is not a
    # generic "missing" placeholder. Preserve enough cache history to fail
    # that requested resource hard with the passport diagnostic.
    for uid, paths in sorted(invalidated.items()):
        if uid in uid_rows:
            continue
        old_uid_row = previous_uids.get(uid, {})
        diagnostics = []
        for path in sorted(paths):
            diagnostics.extend(path_rows[path].get("diagnostics", ()))
        uid_rows[uid] = {
            "candidate_paths": sorted(paths),
            "candidate_fingerprints": sorted({
                path_rows[path]["payload_fingerprint"] for path in paths}),
            "resolved_path": None,
            "resolution_basis": "previous_identity_now_malformed",
            "resolution_status": "invalid_payload",
            "kind": old_uid_row.get("kind"),
            "diagnostics": diagnostics or ["MH_E_PASSPORT_INVALID"],
        }

    available_materials = {
        uid for uid, row in uid_rows.items()
        if row["kind"] == "material"
        and row["resolution_status"] not in {
            "hard_conflict", "external_modified"}
        and row["resolved_path"] is not None
    }
    for uid, row in uid_rows.items():
        if row["kind"] != "static_mesh":
            continue
        descriptor_path = row["resolved_path"] or row["candidate_paths"][0]
        passport = path_rows[descriptor_path]["parsed_passport"]
        if passport is None:
            row["material_status"] = "unavailable"
            row["missing_material_uids"] = []
            continue
        referenced = {
            slot["material_uid"] for slot in passport.get("material_slots", ())}
        missing = sorted(referenced - available_materials)
        row["material_status"] = "missing" if missing else "complete"
        row["missing_material_uids"] = missing
        if missing:
            row["diagnostics"].append(
                "MH_W_MATERIAL_NOT_FOUND: " + ", ".join(missing))

    return {
        "schema": INDEX_SCHEMA,
        "schema_version": INDEX_SCHEMA_VERSION,
        "cache_only": True,
        "source_root": source_root,
        "project_uid": project_uid,
        "paths": dict(sorted(path_rows.items())),
        "uids": uid_rows,
        "legacy_paths": sorted(legacy_paths),
        "quarantine_paths": sorted(quarantine_paths),
        "provisional_resolutions": provisional,
    }


def _is_reserved_temp(filename: str) -> bool:
    return ".mh-tmp-" in filename or ".writing." in filename


def _discover_payloads(root: str) -> tuple[Path, ...]:
    discovered = {}
    for directory, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames.sort(key=lambda item: (_nfc(item).casefold(), _nfc(item)))
        filenames.sort(key=lambda item: (_nfc(item).casefold(), _nfc(item)))
        for filename in filenames:
            lower = filename.lower()
            if _is_reserved_temp(filename) or not lower.endswith(_PAYLOAD_SUFFIXES):
                continue
            path = Path(directory, filename)
            canonical = canonical_payload_path(path)
            if not _inside(root, canonical):
                raise SourceIndexV2Error(
                    "MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT",
                    f"payload resolves outside source_root: {path}")
            if not path.is_file():
                continue
            prior = discovered.get(canonical)
            if prior is not None and prior != path:
                raise SourceIndexV2Error(
                    "MH_E_SOURCE_INDEX_INVALID",
                    f"payload paths alias after canonicalization: {prior}, {path}")
            discovered[canonical] = path
    return tuple(discovered[key] for key in sorted(discovered))


def capture_source_inventory(
        source_root: str | os.PathLike) -> SourceInventoryToken:
    """Capture a stable path-only inventory for one importer transaction."""
    root = canonical_source_root(source_root)
    first = tuple(map(canonical_payload_path, _discover_payloads(root)))
    second = tuple(map(canonical_payload_path, _discover_payloads(root)))
    if first != second:
        raise SourceIndexV2Error(
            "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
            "source payload set changed while capturing import inventory")
    return SourceInventoryToken(root, first)


def assert_source_inventory_stable(token: SourceInventoryToken) -> None:
    """Reject any payload pathname appearance/disappearance since capture."""
    if not isinstance(token, SourceInventoryToken):
        raise TypeError("token must be SourceInventoryToken")
    first = tuple(map(
        canonical_payload_path, _discover_payloads(token.source_root)))
    second = tuple(map(
        canonical_payload_path, _discover_payloads(token.source_root)))
    if first != token.payload_paths or second != token.payload_paths:
        raise SourceIndexV2Error(
            "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
            "source payload set changed after importer preflight")


def _validate_previous_index(
        index: dict | None, root: str, project_uid: str) -> dict | None:
    if index is None:
        return None
    if (not isinstance(index, dict) or index.get("schema") != INDEX_SCHEMA
            or index.get("schema_version") != INDEX_SCHEMA_VERSION
            or index.get("cache_only") is not True
            or index.get("source_root") != root
            or index.get("project_uid") != project_uid
            or not isinstance(index.get("paths"), dict)
            or not isinstance(index.get("uids"), dict)):
        return None
    return index


def scan_source_root(
        source_root: str | os.PathLike, *,
        fbx_passport_extractor: FbxPassportExtractor | None = None,
        previous_index: dict | None = None) -> dict:
    """Full stable scan and rebuild; no manifest or source-tree marker access."""
    root = canonical_source_root(source_root)
    project_uid = project_uid_for_source_root(root)
    previous_index = _validate_previous_index(
        previous_index, root, project_uid)
    first_inventory = _discover_payloads(root)
    previous_paths = (previous_index or {}).get("paths", {})
    observations = [
        _observe_path(
            path, cached_row=previous_paths.get(canonical_payload_path(path)),
            fbx_passport_extractor=fbx_passport_extractor,
            source_root=root)
        for path in first_inventory
    ]
    second_inventory = _discover_payloads(root)
    if tuple(map(canonical_payload_path, first_inventory)) != tuple(
            map(canonical_payload_path, second_inventory)):
        raise SourceIndexV2Error(
            "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
            "source payload set changed during scan")
    return rebuild_index(
        observations, source_root=root, project_uid=project_uid,
        previous_index=previous_index)


def load_index(path: str | os.PathLike) -> dict:
    try:
        document = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise SourceIndexV2Error(
            "MH_E_SOURCE_INDEX_INVALID", f"cannot load local index: {exc}") from exc
    if not isinstance(document, dict):
        raise SourceIndexV2Error(
            "MH_E_SOURCE_INDEX_INVALID", "local index root must be an object")
    return document


def rebuild_and_publish_index(
        source_root: str | os.PathLike, *,
        fbx_passport_extractor: FbxPassportExtractor | None,
        index_path: str | os.PathLike | None = None,
        lock_root: str | os.PathLike | None = None) -> dict:
    """Full-scan payload authority and atomically replace its local cache."""
    root = canonical_source_root(source_root)
    project_uid = project_uid_for_source_root(root)
    cache_path = Path(index_path) if index_path is not None else \
        default_index_path(project_uid)
    if _inside(root, canonical_payload_path(cache_path)):
        raise SourceIndexV2Error(
            "MH_E_SOURCE_INDEX_INVALID",
            "local index must be outside source_root")
    previous = None
    if cache_path.is_file():
        try:
            previous = _validate_previous_index(
                load_index(cache_path), root, project_uid)
        except SourceIndexV2Error:
            previous = None
    rebuilt = scan_source_root(
        root, fbx_passport_extractor=fbx_passport_extractor,
        previous_index=previous)
    atomic_publish_json(
        cache_path, rebuilt, lock_root=lock_root, source_root=root)
    return rebuilt


def _resolution_from_index(
        index: dict, uid: str, *, resolution_source: str,
        confirm_external_modified: bool = False) -> ResolvedResourceV2:
    uid_row = index.get("uids", {}).get(uid)
    if uid_row is None:
        raise SourceIndexV2Error(
            "MH_E_RESOURCE_NOT_FOUND", f"resource UID is absent: {uid}")
    if uid_row.get("resolution_status") == "invalid_payload":
        raise SourceIndexV2Error(
            "MH_E_PASSPORT_INVALID",
            "; ".join(uid_row.get("diagnostics", ())))
    if uid_row.get("resolution_status") == "hard_conflict":
        raise SourceIndexV2Error(
            "MH_E_DIVERGENT_REVISIONS",
            "; ".join(uid_row.get("diagnostics", ())))
    if (uid_row.get("resolution_status") == "external_modified"
            and not confirm_external_modified):
        raise SourceIndexV2Error(
            "MH_E_EXTERNAL_MODIFICATION_CONFIRMATION_REQUIRED",
            f"payload bytes changed without a descriptor change for {uid}")
    path = uid_row.get("resolved_path")
    path_row = index.get("paths", {}).get(path)
    if not path or not path_row or not path_row.get("parsed_passport"):
        raise SourceIndexV2Error(
            "MH_E_RESOURCE_NOT_FOUND", f"resource has no active payload: {uid}")
    diagnostics = list(uid_row.get("diagnostics", ()))
    diagnostics.extend(
        item for item in path_row.get("diagnostics", ())
        if item not in diagnostics)
    return ResolvedResourceV2(
        uid, path, path_row["parsed_passport"]["kind"],
        deepcopy(path_row["parsed_passport"]), path_row["descriptor_hash"],
        path_row["payload_fingerprint"], uid_row["resolution_status"],
        tuple(diagnostics), resolution_source, index)


def _verify_cached_resolution(
        index: dict, uid: str, *, root: str,
        fbx_passport_extractor: FbxPassportExtractor | None,
        confirm_external_modified: bool = False):
    uid_row = index.get("uids", {}).get(uid)
    if not uid_row or uid_row.get("resolution_status") == "hard_conflict":
        return None
    path = uid_row.get("resolved_path")
    candidates = uid_row.get("candidate_paths")
    if (not path or not isinstance(candidates, list) or not candidates
            or path not in candidates):
        return None
    # A cached UID row is usable only after every candidate known to that row
    # still proves its exact bytes and embedded identity.  Checking only the
    # chosen path would silently miss a formerly-identical copy diverging.
    for candidate in candidates:
        old = index.get("paths", {}).get(candidate)
        if (not old or not os.path.isfile(candidate)
                or not _inside(root, candidate)):
            return None
        observation = _observe_path(
            Path(candidate), cached_row=old,
            fbx_passport_extractor=fbx_passport_extractor,
            source_root=root, force_fingerprint=True)
        if (observation.parsed_passport is None
                or observation.parsed_passport.get("resource_uid") != uid
                or observation.payload_fingerprint
                != old.get("payload_fingerprint")
                or observation.descriptor_hash != old.get("descriptor_hash")):
            return None
    return _resolution_from_index(
        index, uid, resolution_source="index",
        confirm_external_modified=confirm_external_modified)


def capture_resource_stability_token(
        resolved: ResolvedResourceV2) -> ResourceStabilityToken:
    """Capture the exact resolved row that an importer is about to consume."""
    if not isinstance(resolved, ResolvedResourceV2):
        raise TypeError("resolved must be ResolvedResourceV2")
    path_row = resolved.index.get("paths", {}).get(resolved.payload_path)
    if path_row is None:
        raise SourceIndexV2Error(
            "MH_E_SOURCE_INDEX_INVALID",
            "resolved payload row is absent from its index")
    return ResourceStabilityToken(
        resolved.resource_uid, resolved.payload_path, resolved.kind,
        int(path_row["size"]), int(path_row["mtime_ns"]),
        resolved.payload_fingerprint, resolved.descriptor_hash)


def assert_resource_stable(
        token: ResourceStabilityToken, *,
        fbx_passport_extractor: FbxPassportExtractor | None = None) -> None:
    """Re-read exact bytes/descriptor and reject a mixed importer snapshot."""
    if not isinstance(token, ResourceStabilityToken):
        raise TypeError("token must be ResourceStabilityToken")
    if not os.path.isfile(token.payload_path):
        raise SourceIndexV2Error(
            "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
            f"resolved payload disappeared: {token.payload_path}")
    cached = {
        "size": token.size,
        "mtime_ns": token.mtime_ns,
        "payload_fingerprint": token.payload_fingerprint,
    }
    observation = _observe_path(
        Path(token.payload_path), cached_row=cached,
        fbx_passport_extractor=fbx_passport_extractor,
        force_fingerprint=True)
    parsed = observation.parsed_passport
    if (str(observation.path) != token.payload_path
            or parsed is None
            or parsed.get("resource_uid") != token.resource_uid
            or parsed.get("kind") != token.kind
            or observation.payload_fingerprint != token.payload_fingerprint
            or observation.descriptor_hash != token.descriptor_hash):
        raise SourceIndexV2Error(
            "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
            f"resolved payload changed after index verification: "
            f"{token.payload_path}")


def resolve_resource_index_first(
        source_root: str | os.PathLike, resource_uid: str, *,
        fbx_passport_extractor: FbxPassportExtractor | None = None,
        index_path: str | os.PathLike | None = None,
        publish_index: bool = True,
        lock_root: str | os.PathLike | None = None,
        confirm_external_modified: bool = False) -> ResolvedResourceV2:
    """Verify the cached active payload, then full-scan on any cache miss.

    The disposable cache accelerates payload parsing, never source discovery.
    Every resolve first compares the complete current payload pathname set to
    the cached inventory.  A copied/deleted candidate therefore forces a scan
    immediately, so a newly introduced divergent same-UID revision can never
    be hidden behind a still-valid cached winner.
    """
    root = canonical_source_root(source_root)
    uid = _normalize_uid(resource_uid, "resource_uid")
    project_uid = project_uid_for_source_root(root)
    cache_path = Path(index_path) if index_path is not None else \
        default_index_path(project_uid)
    if _inside(root, canonical_payload_path(cache_path)):
        raise SourceIndexV2Error(
            "MH_E_SOURCE_INDEX_INVALID",
            "local index must be outside source_root")
    previous = None
    if cache_path.is_file():
        try:
            previous = _validate_previous_index(
                load_index(cache_path), root, project_uid)
        except SourceIndexV2Error:
            previous = None
    if previous is not None:
        discovered = {
            canonical_payload_path(path) for path in _discover_payloads(root)}
        cached_paths = set(previous.get("paths", {}))
        if discovered == cached_paths:
            cached = _verify_cached_resolution(
                previous, uid, root=root,
                fbx_passport_extractor=fbx_passport_extractor,
                confirm_external_modified=confirm_external_modified)
            if cached is not None:
                confirmed = {
                    canonical_payload_path(path)
                    for path in _discover_payloads(root)}
                if confirmed == cached_paths:
                    return cached

    rebuilt = scan_source_root(
        root, fbx_passport_extractor=fbx_passport_extractor,
        previous_index=previous)
    if publish_index:
        atomic_publish_json(
            cache_path, rebuilt, lock_root=lock_root, source_root=root)
    return _resolution_from_index(
        rebuilt, uid, resolution_source="scan",
        confirm_external_modified=confirm_external_modified)


def fork_document(
        document: dict, *, new_resource_uid: str | None = None,
        dependency_uid_replacements: dict[str, str] | None = None) -> dict:
    """Purely fork a passport/material/composite without mutating its input."""
    if not isinstance(document, dict):
        raise TypeError("document must be a dict")
    forked = deepcopy(document)
    replacement_uid = _normalize_uid(
        new_resource_uid or str(uuid.uuid4()), "new_resource_uid")
    if "resource_uid" in forked:
        identity_key = "resource_uid"
    elif "uid" in forked:
        identity_key = "uid"
    else:
        raise ValueError("document has no resource_uid or uid")
    forked[identity_key] = replacement_uid
    replacements = {
        _normalize_uid(old, "old dependency UID"):
        _normalize_uid(new, "new dependency UID")
        for old, new in (dependency_uid_replacements or {}).items()
    }
    if forked.get("schema") == "mh.composite":
        nodes = forked.get("nodes")
        if not isinstance(nodes, list):
            raise ValueError("composite nodes must be an array")
        for node in nodes:
            if not isinstance(node, dict) or "resource_uid" not in node:
                continue
            try:
                current = _normalize_uid(node["resource_uid"], "node resource_uid")
            except ValueError:
                continue
            if current in replacements:
                node["resource_uid"] = replacements[current]
    return forked
