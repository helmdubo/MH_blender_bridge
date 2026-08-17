"""Frozen Source Schema v1 manifest codec and transaction helpers.

``export_manifest.json`` is a directory-local inventory of owned resources.
It is intentionally not a bundle, a dependency closure, or Blender provenance
metadata. Materials are ordinary ``kind: material`` resource rows whose
semantic payload lives in a separate ``*.material`` file.

The writer protocol is marker-first and fail-closed: a complete prepared
manifest is written to ``export_manifest.json.tmp`` before its one payload is
replaced, and promotion of that marker is the commit event.
"""

from __future__ import annotations

from copy import deepcopy
import json
import math
import ntpath
import os
import posixpath
import re
import time
import unicodedata
import uuid

from ..core.canonical import validate_resource_name


MANIFEST_NAME = "export_manifest.json"
PENDING_MANIFEST_NAME = f"{MANIFEST_NAME}.tmp"
MANIFEST_SCHEMA = "mh.export_manifest"
MANIFEST_SCHEMA_VERSION = 1

_TOP_LEVEL_FIELDS = frozenset({
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
_RESOURCE_SUFFIX = {
    "static_mesh": ".mesh.fbx",
    "composite": ".composite",
    "material": ".material",
}
_CONTENT_HASH_RE = re.compile(r"^xxh3:[0-9a-f]{16}$")
_STAGED_TOKENS: dict[str, bytes] = {}
_STAGED_LOCKS: dict[str, object] = {}
_ROOT_LOCK_NAME = ".mh_source_root.lock"
_DIRECTORY_LOCK_NAME = ".mh_manifest_writer.lock"
_WINDOWS_REPLACE_TRANSIENT_ERRORS = frozenset({5, 32})
_WINDOWS_REPLACE_RETRY_DELAYS = (0.01, 0.02, 0.04, 0.08, 0.16, 0.20)
_WINDOWS_CLEANUP_RETRY_DELAYS = (0.01, 0.02, 0.04, 0.08, 0.16)


class ManifestError(ValueError):
    """A manifest is malformed, unstable, or unsafe to update."""

    def __init__(self, message: str, code: str = "MH_E_INVALID_EXPORT_MANIFEST"):
        self.code = code
        super().__init__(f"{code}: {message}")


class PreparedManifest(dict):
    """Validated manifest plus the marker state its preparation observed."""

    def __init__(self, document: dict, *, directory: str,
                 previous_marker: bytes | None, source_root: str,
                 incoming_uid: str):
        super().__init__(document)
        self._directory_key = _directory_key(directory)
        self._previous_marker = previous_marker
        self._source_root = os.path.normpath(os.path.abspath(source_root))
        self._incoming_uid = incoming_uid

    @property
    def is_recovery(self) -> bool:
        """Whether payload replacement must bypass semantic hash-skip."""
        return self._previous_marker is not None


def _fail(message: str, code: str = "MH_E_INVALID_EXPORT_MANIFEST") -> None:
    raise ManifestError(message, code)


def _manifest_path(directory: str) -> str:
    return os.path.join(directory, MANIFEST_NAME)


def _pending_path(directory: str) -> str:
    return os.path.join(directory, PENDING_MANIFEST_NAME)


def _directory_key(directory: str) -> str:
    return os.path.normcase(os.path.normpath(os.path.abspath(directory)))


def _acquire_file_lock(path: str, owner: str):
    """Acquire one process-crash-safe nonblocking one-byte filesystem lock."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    stream = open(path, "a+b")
    try:
        stream.seek(0, os.SEEK_END)
        if stream.tell() == 0:
            stream.write(b"\0")
            stream.flush()
        stream.seek(0)
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return stream
    except (OSError, IOError) as exc:
        stream.close()
        _fail(f"another writer holds {owner}: {exc}")


def _release_file_lock(stream) -> None:
    if stream is None or stream.closed:
        return
    try:
        stream.seek(0)
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream.fileno(), fcntl.LOCK_UN)
    finally:
        stream.close()


def _release_staged_lock(directory: str) -> None:
    directory_key = _directory_key(directory)
    stream = _STAGED_LOCKS.pop(directory_key, None)
    _STAGED_TOKENS.pop(directory_key, None)
    _release_file_lock(stream)


def _json_bytes(document: dict) -> bytes:
    return (json.dumps(document, indent=2, ensure_ascii=False) + "\n").encode(
        "utf-8")


def _replace_atomic(source: str, destination: str) -> None:
    """Retry only documented transient Windows sharing/access violations.

    ``winerror`` is absent on ordinary POSIX errors, so errno values such as
    EACCES are never accidentally retried cross-platform. The fixed backoff is
    bounded to 0.51 seconds total and the final original ``OSError`` propagates.
    """
    retry_index = 0
    while True:
        try:
            os.replace(source, destination)
            return
        except OSError as exc:
            if (getattr(exc, "winerror", None)
                    not in _WINDOWS_REPLACE_TRANSIENT_ERRORS
                    or retry_index >= len(_WINDOWS_REPLACE_RETRY_DELAYS)):
                raise
            time.sleep(_WINDOWS_REPLACE_RETRY_DELAYS[retry_index])
            retry_index += 1


def _remove_staging(path: str) -> None:
    """Best-effort transient-safe removal, preserving caller error priority."""
    retry_index = 0
    while True:
        try:
            os.remove(path)
            return
        except FileNotFoundError:
            return
        except OSError as exc:
            if (getattr(exc, "winerror", None)
                    not in _WINDOWS_REPLACE_TRANSIENT_ERRORS
                    or retry_index >= len(_WINDOWS_CLEANUP_RETRY_DELAYS)):
                raise
            time.sleep(_WINDOWS_CLEANUP_RETRY_DELAYS[retry_index])
            retry_index += 1


def _write_bytes_atomic(path: str, payload: bytes) -> None:
    staging = f"{path}.writing.{os.getpid()}.{uuid.uuid4().hex}"
    operation_failed = False
    try:
        with open(staging, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        _replace_atomic(staging, path)
    except BaseException:
        operation_failed = True
        raise
    finally:
        try:
            _remove_staging(staging)
        except OSError:
            if not operation_failed:
                raise


def _write_bytes_exclusive(path: str, payload: bytes) -> None:
    """Install a complete marker without replacing another writer's marker."""
    staging = f"{path}.writing.{os.getpid()}.{uuid.uuid4().hex}"
    operation_failed = False
    try:
        with open(staging, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        try:
            # Same-volume hard-link installation is atomic and fails when the
            # marker already exists on both NTFS and POSIX filesystems.
            os.link(staging, path)
        except FileExistsError:
            _fail(f"another writer already owns {PENDING_MANIFEST_NAME}")
        except OSError:
            # Filesystems without hard links retain exclusivity via O_EXCL.
            # A crash during this rare fallback leaves a malformed fail-closed
            # marker; it can never be mistaken for a committed manifest.
            try:
                descriptor = os.open(
                    path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o666)
            except FileExistsError:
                _fail(f"another writer already owns {PENDING_MANIFEST_NAME}")
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(payload)
                stream.flush()
                os.fsync(stream.fileno())
    except BaseException:
        operation_failed = True
        raise
    finally:
        try:
            _remove_staging(staging)
        except OSError:
            if not operation_failed:
                raise


def _read_file_bytes(path: str) -> bytes:
    """Small test seam for deterministic reader/writer race coverage."""
    with open(path, "rb") as stream:
        return stream.read()


def _reject_pending(directory: str) -> None:
    if os.path.exists(_pending_path(directory)):
        _fail(f"export in progress or interrupted: {PENDING_MANIFEST_NAME} exists")


def _decode_manifest(payload: bytes, path: str) -> dict:
    try:
        return json.loads(payload.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        _fail(f"cannot read {path}: {exc}")


def _required_string(row: dict, field: str, owner: str) -> str:
    value = row.get(field)
    if not isinstance(value, str) or not value:
        _fail(f"{owner} has invalid required {field}")
    return value


def _validate_uuid(value: object, owner: str) -> str:
    if not isinstance(value, str):
        _fail(f"{owner} must be a lowercase UUID string")
    try:
        parsed = uuid.UUID(value)
    except (ValueError, AttributeError):
        _fail(f"{owner} must be a lowercase UUID string")
    if str(parsed) != value:
        _fail(f"{owner} must be a canonical lowercase UUID string")
    return value


def _validate_hash(value: object, owner: str) -> str:
    if not isinstance(value, str) or not _CONTENT_HASH_RE.fullmatch(value):
        _fail(f"{owner} must match 'xxh3:' plus 16 lowercase hex digits")
    return value


def _json_value(value, owner: str):
    """Validate a JSON bag and return an NFC-normalized independent copy."""
    if value is None or isinstance(value, (str, bool)):
        return unicodedata.normalize("NFC", value) if isinstance(value, str) else value
    if isinstance(value, int) and not isinstance(value, bool):
        return value
    if isinstance(value, float):
        if not math.isfinite(value):
            _fail(f"{owner} contains NaN or Infinity")
        return value
    if isinstance(value, (list, tuple)):
        return [_json_value(item, f"{owner}[{index}]")
                for index, item in enumerate(value)]
    if isinstance(value, dict):
        result = {}
        source_keys = {}
        for key, item in value.items():
            if not isinstance(key, str):
                _fail(f"{owner} object keys must be strings")
            normalized = unicodedata.normalize("NFC", key)
            if normalized in result:
                _fail(
                    f"{owner} keys collide after NFC normalization: "
                    f"{source_keys[normalized]!r} and {key!r}")
            source_keys[normalized] = key
            result[normalized] = _json_value(item, f"{owner}.{normalized}")
        return result
    _fail(f"{owner} contains non-JSON value {type(value).__name__}")


def validate_relative_path(path: object, owner: str) -> str:
    """Validate one already-canonical relative v1 path without repairing it."""
    if not isinstance(path, str) or not path or "\x00" in path:
        _fail(f"{owner} must be a non-empty relative path",
              "MH_E_INVALID_RESOURCE_SOURCE")
    normalized = unicodedata.normalize("NFC", path)
    drive, _tail = ntpath.splitdrive(normalized)
    canonical = posixpath.normpath(normalized)
    if (normalized != path or drive
            or normalized.startswith(("/", "\\")) or "\\" in normalized
            or canonical in {"", ".", ".."}
            or canonical.startswith("../") or canonical != normalized):
        _fail(
            f"{owner} must be normalized relative forward-slash path without . or ..",
            "MH_E_INVALID_RESOURCE_SOURCE")
    return normalized


def _validate_resource_source(path: object, uid: str, kind: str,
                              owner: str) -> str:
    source = validate_relative_path(path, owner)
    suffix = _RESOURCE_SUFFIX[kind]
    basename = posixpath.basename(source)
    required = f"__{uid[:8]}{suffix}"
    if not basename.endswith(required):
        _fail(
            f"{owner} must end with {required!r} for {kind}",
            "MH_E_INVALID_RESOURCE_SOURCE")
    return source


def _validate_material_slots(value: object, uid: str) -> list[dict]:
    if not isinstance(value, list):
        _fail(f"static_mesh {uid} material_slots must be an array")
    result = []
    names = set()
    material_uids = set()
    for index, slot in enumerate(value):
        owner = f"static_mesh {uid} material_slots[{index}]"
        if not isinstance(slot, dict) or set(slot) != {"slot_name", "material_uid"}:
            _fail(f"{owner} must contain exactly slot_name and material_uid")
        name = _required_string(slot, "slot_name", owner)
        name = unicodedata.normalize("NFC", name)
        material_uid = _validate_uuid(slot.get("material_uid"),
                                      f"{owner}.material_uid")
        if name in names:
            _fail(f"static_mesh {uid} contains duplicate material slot_name {name!r}")
        if material_uid in material_uids:
            _fail(f"static_mesh {uid} repeats material_uid {material_uid}")
        names.add(name)
        material_uids.add(material_uid)
        result.append({"slot_name": name, "material_uid": material_uid})
    return result


def _validate_lods(value: object, uid: str) -> list[dict]:
    if not isinstance(value, list):
        _fail(f"static_mesh {uid} lods must be an array")
    result = []
    levels = set()
    for index, lod in enumerate(value):
        owner = f"static_mesh {uid} lods[{index}]"
        if not isinstance(lod, dict) or set(lod) != {"level", "source", "content_hash"}:
            _fail(f"{owner} must contain exactly level, source and content_hash")
        level = lod["level"]
        if not isinstance(level, int) or isinstance(level, bool) or level < 1:
            _fail(f"{owner}.level must be an integer >= 1")
        if level in levels:
            _fail(f"static_mesh {uid} contains duplicate LOD level {level}")
        levels.add(level)
        source = validate_relative_path(lod["source"], f"{owner}.source")
        required = f"__{uid[:8]}.lod{level}.mesh.fbx"
        if not posixpath.basename(source).endswith(required):
            _fail(f"{owner}.source must end with {required!r}",
                  "MH_E_INVALID_RESOURCE_SOURCE")
        result.append({
            "level": level,
            "source": source,
            "content_hash": _validate_hash(lod["content_hash"],
                                             f"{owner}.content_hash"),
        })
    return result


def _validate_resource_row(row: object, index: int) -> dict:
    owner = f"export manifest resources[{index}]"
    if not isinstance(row, dict):
        _fail(f"{owner} must be an object")
    uid = _validate_uuid(row.get("uid"), f"{owner}.uid")
    kind = _required_string(row, "kind", owner)
    if kind not in _ROW_FIELDS:
        _fail(f"resource {uid} has unsupported kind {kind!r}")
    unknown = set(row) - _ROW_FIELDS[kind]
    missing = _COMMON_ROW_FIELDS - set(row)
    if missing or unknown:
        _fail(
            f"resource {uid} has invalid fields; missing={sorted(missing)}, "
            f"unknown={sorted(unknown)}")

    name = _required_string(row, "name", owner)
    try:
        validate_resource_name(name)
    except (TypeError, ValueError) as exc:
        _fail(f"resource {uid} has invalid name: {exc}")
    result = {
        "uid": uid,
        "kind": kind,
        "name": name,
        "source": _validate_resource_source(
            row["source"], uid, kind, f"resource {uid}.source"),
        "content_hash": _validate_hash(
            row["content_hash"], f"resource {uid}.content_hash"),
    }

    if kind == "static_mesh":
        if "material_slots" in row:
            result["material_slots"] = _validate_material_slots(
                row["material_slots"], uid)
        if "properties" in row:
            if not isinstance(row["properties"], dict):
                _fail(f"static_mesh {uid} properties must be an object")
            result["properties"] = _json_value(
                row["properties"], f"static_mesh {uid}.properties")
        if "lod_policy" in row:
            if row["lod_policy"] not in {"authored", "generated", "nanite"}:
                _fail(f"static_mesh {uid} has invalid lod_policy")
            result["lod_policy"] = row["lod_policy"]
        if "lods" in row:
            result["lods"] = _validate_lods(row["lods"], uid)
    elif kind == "composite" and "properties" in row:
        if not isinstance(row["properties"], dict):
            _fail(f"composite {uid} properties must be an object")
        result["properties"] = _json_value(
            row["properties"], f"composite {uid}.properties")
    return result


def validate_export_manifest(document: dict) -> dict:
    """Validate and return the canonical frozen-v1 manifest document.

    This reader intentionally has no pre-freeze migration path. Unknown
    top-level/resource fields and the former inline ``materials[]`` form are
    rejected.
    """
    if not isinstance(document, dict):
        _fail("export manifest must be a JSON object")
    unknown = set(document) - _TOP_LEVEL_FIELDS
    missing = _TOP_LEVEL_FIELDS - set(document)
    if missing or unknown:
        _fail(
            f"export manifest has invalid fields; missing={sorted(missing)}, "
            f"unknown={sorted(unknown)}")
    if document.get("schema") != MANIFEST_SCHEMA:
        _fail(f"unsupported export manifest schema {document.get('schema')!r}")
    version = document.get("schema_version")
    if (not isinstance(version, int) or isinstance(version, bool)
            or version != MANIFEST_SCHEMA_VERSION):
        _fail(f"unsupported export manifest schema_version {version!r}",
              "MH_E_UNKNOWN_SCHEMA_VERSION")
    exporter_version = document.get("exporter_version")
    if not isinstance(exporter_version, str) or not exporter_version:
        _fail("export manifest exporter_version must be a non-empty string")
    rows = document.get("resources")
    if not isinstance(rows, list):
        _fail("export manifest resources must be an array")

    normalized = [_validate_resource_row(row, index)
                  for index, row in enumerate(rows)]
    uids = [row["uid"] for row in normalized]
    if len(uids) != len(set(uids)):
        _fail("export manifest resources contain duplicate uid")
    if uids != sorted(uids, key=lambda value: value.encode("utf-8")):
        _fail("export manifest resources must be sorted bytewise by uid")
    uid8_owners = {}
    for uid in uids:
        previous = uid8_owners.get(uid[:8])
        if previous is not None and previous != uid:
            _fail(
                f"UIDs {previous} and {uid} share uid8 {uid[:8]}",
                "MH_E_UID8_COLLISION")
        uid8_owners[uid[:8]] = uid

    source_owners = {}
    for row in normalized:
        sources = [(row["source"], f"resource {row['uid']}")]
        sources.extend((lod["source"], f"resource {row['uid']} LOD {lod['level']}")
                       for lod in row.get("lods", []))
        for source, owner in sources:
            key = source.casefold()
            previous = source_owners.get(key)
            if previous is not None:
                _fail(f"manifest source {source!r} is shared by {previous} and {owner}")
            source_owners[key] = owner

    return {
        "schema": MANIFEST_SCHEMA,
        "schema_version": MANIFEST_SCHEMA_VERSION,
        "exporter_version": exporter_version,
        "resources": normalized,
    }


def _empty_manifest(exporter_version: str) -> dict:
    return {
        "schema": MANIFEST_SCHEMA,
        "schema_version": MANIFEST_SCHEMA_VERSION,
        "exporter_version": exporter_version,
        "resources": [],
    }


def _read_stable_manifest_payload(
        directory: str, *, allow_missing: bool) -> bytes | None:
    """Return one stable raw snapshot, or ``None`` for a stable absence."""
    _reject_pending(directory)
    path = _manifest_path(directory)
    try:
        first = _read_file_bytes(path)
    except FileNotFoundError:
        _reject_pending(directory)
        if allow_missing:
            if os.path.exists(path):
                _fail(f"cannot read stable {path}: manifest appeared while reading")
            _reject_pending(directory)
            return None
        _fail(f"{MANIFEST_NAME} not found in {directory}")
    except OSError as exc:
        _fail(f"cannot read {path}: {exc}")
    _reject_pending(directory)
    try:
        second = _read_file_bytes(path)
    except OSError as exc:
        _fail(f"cannot read {path}: {exc}")
    _reject_pending(directory)
    if first != second:
        _fail(f"cannot read stable {path}: manifest changed while reading")
    return first


def load_export_manifest_snapshot(
        directory: str, *, allow_missing: bool = False,
        ) -> tuple[dict | None, bytes | None]:
    """Load one directory manifest and its opaque raw-byte stability token."""
    payload = _read_stable_manifest_payload(directory, allow_missing=allow_missing)
    if payload is None:
        return None, None
    path = _manifest_path(directory)
    return validate_export_manifest(_decode_manifest(payload, path)), payload


def assert_manifest_stable(directory: str, token: bytes | None) -> None:
    """Fail unless manifest bytes/absence still match a snapshot token."""
    if token is not None and not isinstance(token, bytes):
        raise TypeError("manifest snapshot token must be bytes or None")
    current = _read_stable_manifest_payload(
        directory, allow_missing=token is None)
    if current != token:
        _fail(f"cannot apply source plan: {MANIFEST_NAME} changed after it was read")


def load_export_manifest(directory: str, *, allow_missing: bool = False) -> dict | None:
    document, _token = load_export_manifest_snapshot(
        directory, allow_missing=allow_missing)
    return document


def _read_manifest_direct(path: str, *, allow_missing: bool) -> dict | None:
    try:
        with open(path, "rb") as stream:
            payload = stream.read()
    except FileNotFoundError:
        if allow_missing:
            return None
        _fail(f"{MANIFEST_NAME} not found at {path}")
    except OSError as exc:
        _fail(f"cannot read {path}: {exc}")
    return validate_export_manifest(_decode_manifest(payload, path))


def _row_index(document: dict) -> dict[str, dict]:
    return {row["uid"]: deepcopy(row) for row in document["resources"]}


def _validate_recovery(stable: dict | None, pending: dict,
                       incoming_uid: str) -> None:
    stable_rows = _row_index(stable) if stable is not None else {}
    pending_rows = _row_index(pending)
    changed = {
        uid for uid in stable_rows.keys() | pending_rows.keys()
        if stable_rows.get(uid) != pending_rows.get(uid)
    }
    if changed != {incoming_uid}:
        _fail(
            "interrupted export marker must differ from stable manifest by "
            f"exactly incoming UID {incoming_uid}; changed={sorted(changed)}")
    if stable is None and set(pending_rows) != {incoming_uid}:
        _fail("first-export recovery marker may contain only the incoming UID")
    stable_without_version = deepcopy(stable) if stable is not None else None
    pending_without_version = deepcopy(pending)
    if stable_without_version is not None:
        stable_without_version["exporter_version"] = pending_without_version[
            "exporter_version"]
        stable_without_version["resources"] = [
            row for row in stable_without_version["resources"]
            if row["uid"] != incoming_uid]
        pending_without_version["resources"] = [
            row for row in pending_without_version["resources"]
            if row["uid"] != incoming_uid]
        if stable_without_version != pending_without_version:
            _fail("interrupted export marker changed unrelated manifest data")


def _verify_global_owner(directory: str, source_root: str, uid: str,
                         *, ignore_own_pending: bool) -> None:
    """Prove global owner uniqueness using the authority scan, not registry."""
    # Lazy import keeps the manifest codec usable without introducing an import
    # cycle while core.source_resolver itself consumes this codec.
    from ..core.source_resolver import _scan_source_root

    root = os.path.normpath(os.path.abspath(source_root))
    output = os.path.normpath(os.path.abspath(directory))
    try:
        output_is_internal = os.path.normcase(os.path.commonpath([root, output])) \
            == os.path.normcase(root)
    except ValueError:
        output_is_internal = False
    if not output_is_internal:
        _fail(
            f"owning manifest directory {output} is outside source_root {root}",
            "MH_E_INVALID_RESOURCE_SOURCE")
    real_root = os.path.realpath(root)
    real_output = os.path.realpath(output)
    try:
        real_output_is_internal = os.path.normcase(os.path.commonpath([
            real_root, real_output])) == os.path.normcase(real_root)
    except ValueError:
        real_output_is_internal = False
    if not real_output_is_internal:
        _fail(
            f"owning manifest directory {output} resolves outside "
            f"source_root {root}",
            "MH_E_INVALID_RESOURCE_SOURCE")
    pending = _pending_path(directory)
    ignored = [pending] if ignore_own_pending else []
    snapshot = _scan_source_root(
        source_root, ignored_pending_paths=ignored)
    entries = snapshot.owners.get(uid, ())
    if len(entries) > 1:
        manifests = ", ".join(path for path, _row in entries)
        _fail(
            f"resource {uid} has multiple owning manifests: {manifests}",
            "MH_E_AMBIGUOUS_RESOURCE_OWNER")
    if entries:
        expected = os.path.normcase(os.path.normpath(_manifest_path(directory)))
        actual = os.path.normcase(os.path.normpath(entries[0][0]))
        if actual != expected:
            _fail(
                f"resource {uid} is already owned by {entries[0][0]}; "
                "existing location wins",
                "MH_E_AMBIGUOUS_RESOURCE_OWNER")
    for existing_uid in snapshot.owners:
        if existing_uid != uid and existing_uid[:8] == uid[:8]:
            _fail(
                f"UIDs {existing_uid} and {uid} share uid8 {uid[:8]}",
                "MH_E_UID8_COLLISION")


def prepare_manifest_update(
        directory: str, *, resources=(), exporter_version: str,
        blend_file: str | None = None,
        source_root: str | None = None) -> dict:
    """Prepare one resource-row upsert while preserving unrelated rows.

    ``blend_file`` remains an ignored call-site compatibility parameter; Source
    Schema v1 never serializes Blender provenance. A single call represents
    exactly one standalone payload transaction.
    """
    del blend_file
    incoming = list(resources)
    if len(incoming) != 1:
        _fail("one standalone export must upsert exactly one resource row")
    if source_root is None:
        _fail("source_root is required for global owner proof")
    incoming_doc = validate_export_manifest({
        **_empty_manifest(exporter_version),
        "resources": incoming,
    })
    incoming_row = incoming_doc["resources"][0]
    incoming_uid = incoming_row["uid"]

    stable_path = _manifest_path(directory)
    pending_path = _pending_path(directory)
    stable = _read_manifest_direct(stable_path, allow_missing=True)
    pending = None
    pending_payload = None
    if os.path.exists(pending_path):
        pending_payload = _read_file_bytes(pending_path)
        pending = validate_export_manifest(
            _decode_manifest(pending_payload, pending_path))
        _validate_recovery(stable, pending, incoming_uid)

    _verify_global_owner(
        directory, source_root, incoming_uid,
        ignore_own_pending=pending is not None)

    base = stable or _empty_manifest(exporter_version)
    index = _row_index(base)
    previous = index.get(incoming_uid)
    if previous is not None:
        if previous["kind"] != incoming_row["kind"]:
            _fail(
                f"resource {incoming_uid} cannot change kind from "
                f"{previous['kind']!r} to {incoming_row['kind']!r}")
        if previous["source"] != incoming_row["source"]:
            _fail(
                f"resource {incoming_uid} existing source {previous['source']!r} "
                "wins; automatic payload move/rename is forbidden")
    if pending is not None:
        interrupted = _row_index(pending).get(incoming_uid)
        if interrupted is not None:
            if interrupted["kind"] != incoming_row["kind"]:
                _fail(
                    f"resource {incoming_uid} cannot change kind from interrupted "
                    f"{interrupted['kind']!r} to {incoming_row['kind']!r}")
            if interrupted["source"] != incoming_row["source"]:
                _fail(
                    f"resource {incoming_uid} recovery source must remain "
                    f"{interrupted['source']!r}")
            if previous is not None and interrupted["source"] != previous["source"]:
                _fail("interrupted marker changed an existing resource source")

    index[incoming_uid] = incoming_row
    result = _empty_manifest(exporter_version)
    result["resources"] = [index[uid] for uid in sorted(index)]
    return PreparedManifest(
        validate_export_manifest(result),
        directory=directory,
        previous_marker=pending_payload,
        source_root=source_root,
        incoming_uid=incoming_uid,
    )


def stage_manifest(directory: str, document: dict, *,
                   snapshot_guard=None) -> str:
    """Stage a prepared marker under the writer locks.

    When supplied, ``snapshot_guard`` is called after owner/marker rechecks and
    immediately before the marker write while both the source-root and owning-
    directory writer locks are held. The callback must raise when its caller's
    dependency snapshot is stale. Keeping this as an injected callback avoids a
    manifest-codec/resolver import cycle.
    """
    if not isinstance(document, PreparedManifest):
        _fail("stage_manifest requires prepare_manifest_update output")
    if snapshot_guard is not None and not callable(snapshot_guard):
        _fail("snapshot_guard must be callable")
    os.makedirs(directory, exist_ok=True)
    normalized = validate_export_manifest(document)
    path = _pending_path(directory)
    payload = _json_bytes(normalized)
    directory_key = _directory_key(directory)
    expected = document._previous_marker
    prepared_directory = document._directory_key
    if prepared_directory != directory_key:
        _fail("prepared manifest cannot be staged in another directory")
    root_lock = _acquire_file_lock(
        os.path.join(document._source_root, _ROOT_LOCK_NAME),
        "source-root writer lock")
    directory_lock = None
    try:
        directory_lock = _acquire_file_lock(
            os.path.join(directory, _DIRECTORY_LOCK_NAME),
            "owning-manifest writer lock")
        # Preparation's scan is advisory until stage. Re-prove under the
        # source-root lock so different destination directories cannot both
        # reserve owners from the same stale root state.
        _verify_global_owner(
            directory, document._source_root, document._incoming_uid,
            ignore_own_pending=expected is not None)
        if os.path.exists(path):
            if expected is None:
                _fail(f"another writer already owns {PENDING_MANIFEST_NAME}")
            current = _read_file_bytes(path)
            if current != expected:
                _fail("pending marker changed after recovery preparation")
            if snapshot_guard is not None:
                snapshot_guard()
            _write_bytes_atomic(path, payload)
        else:
            if expected is not None:
                _fail("pending marker disappeared after recovery preparation")
            if snapshot_guard is not None:
                snapshot_guard()
            _write_bytes_exclusive(path, payload)
        _STAGED_TOKENS[directory_key] = payload
        _STAGED_LOCKS[directory_key] = directory_lock
        directory_lock = None
    finally:
        _release_file_lock(directory_lock)
        _release_file_lock(root_lock)
    return path


def commit_staged_manifest(directory: str) -> str:
    """Promote only the exact marker staged by this writer process."""
    pending = _pending_path(directory)
    directory_key = _directory_key(directory)
    try:
        if not os.path.exists(pending):
            _fail(f"no staged {PENDING_MANIFEST_NAME} to commit")
        token = _STAGED_TOKENS.get(directory_key)
        if token is None or directory_key not in _STAGED_LOCKS:
            _fail("staged marker has no writer token; explicit recovery is required")
        current = _read_file_bytes(pending)
        if current != token:
            _fail("staged marker changed before commit")
        validate_export_manifest(_decode_manifest(current, pending))
        final = _manifest_path(directory)
        _replace_atomic(pending, final)
        return final
    finally:
        _release_staged_lock(directory)


def abandon_staged_manifest(directory: str) -> None:
    """Release this process' writer lock after payload failure.

    The pending marker deliberately remains fail-closed. A later explicit
    same-UID retry must prove and replace it through normal recovery.
    """
    _release_staged_lock(directory)


def manifest_resource_index(document: dict) -> dict[str, dict]:
    """Return the resources-only UID index for a validated v1 manifest."""
    normalized = validate_export_manifest(document)
    return {row["uid"]: row for row in normalized["resources"]}
