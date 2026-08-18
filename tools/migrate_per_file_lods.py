"""One-shot migration preflight for deprecated per-file LOD manifest rows.

The production manifest reader intentionally remains strict and rejects every
``lods`` field.  This external tool creates an exact, non-scanned backup and
removes only the selected legacy owner row.  The artist must then immediately
re-export the matching Blender ``.lods`` collection with the same ResourceUID;
payload files are inspected but never changed or deleted here.
"""

from __future__ import annotations

import argparse
from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import uuid


TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(TOOLS_DIR), "addon"))

from mh4blend.scene.source_manifest import (  # noqa: E402
    MANIFEST_NAME,
    PENDING_MANIFEST_NAME,
    ManifestError,
    validate_export_manifest,
    validate_relative_path,
)


RECEIPT_SCHEMA = "mh.per_file_lod_migration_receipt"
RECEIPT_SCHEMA_VERSION = 1
NEXT_ACTION = "re-export selected .lods collection with same UID"
_DIRECTORY_LOCK_NAME = ".mh_manifest_writer.lock"
_CONTENT_HASH_RE = re.compile(r"^xxh3:[0-9a-f]{16}$")
_TOP_LEVEL_FIELDS = frozenset({
    "schema", "schema_version", "exporter_version", "resources",
})
_LEGACY_STATIC_MESH_FIELDS = frozenset({
    "uid", "kind", "name", "source", "content_hash", "material_slots",
    "properties", "lod_policy", "lods",
})


class MigrationError(RuntimeError):
    """The one-shot migration cannot prove a safe exact transition."""


def _error(message: str) -> None:
    raise MigrationError(message)


def _canonical_uid(value: str) -> str:
    try:
        parsed = str(uuid.UUID(value))
    except (ValueError, AttributeError) as exc:
        _error(f"ResourceUID must be a canonical lowercase UUID: {exc}")
    if parsed != value:
        _error("ResourceUID must be a canonical lowercase UUID")
    return value


def _manifest_path(value: str | os.PathLike[str]) -> Path:
    path = Path(value).absolute()
    if path.name != MANIFEST_NAME:
        _error(f"manifest path must end with exact basename {MANIFEST_NAME!r}")
    if path.is_symlink() or not path.is_file():
        _error(f"manifest must be a regular non-symlink file: {path}")
    return path


def backup_path_for(
        manifest_path: str | os.PathLike[str], resource_uid: str) -> Path:
    """Return the deterministic non-scanned exact-backup path."""
    uid = _canonical_uid(resource_uid)
    manifest = Path(manifest_path).absolute()
    return manifest.with_name(f"{MANIFEST_NAME}.pre-combined-lod.{uid}.bak")


def _pairs_without_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON object key {key!r}")
        result[key] = value
    return result


def _decode_exact(payload: bytes, owner: str) -> dict:
    try:
        document = json.loads(
            payload.decode("utf-8"), object_pairs_hook=_pairs_without_duplicates)
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        _error(f"cannot parse exact UTF-8 JSON from {owner}: {exc}")
    if not isinstance(document, dict):
        _error(f"{owner} must contain one JSON object")
    if set(document) != _TOP_LEVEL_FIELDS:
        _error(
            f"{owner} has invalid top-level fields; "
            f"missing={sorted(_TOP_LEVEL_FIELDS - set(document))}, "
            f"unknown={sorted(set(document) - _TOP_LEVEL_FIELDS)}")
    return document


def _skip_json_whitespace(text: str, index: int) -> int:
    while index < len(text) and text[index] in " \t\r\n":
        index += 1
    return index


def _resource_element_layout(text: str) -> tuple[list[tuple[int, int]], list[int]]:
    """Locate resources[] element/comma spans without re-encoding other rows."""
    decoder = json.JSONDecoder()
    index = _skip_json_whitespace(text, 0)
    if index >= len(text) or text[index] != "{":
        _error("manifest JSON must begin with an object")
    index += 1
    while True:
        index = _skip_json_whitespace(text, index)
        if index >= len(text) or text[index] == "}":
            break
        try:
            key, index = decoder.raw_decode(text, index)
        except json.JSONDecodeError as exc:
            _error(f"cannot locate manifest resources array: {exc}")
        if not isinstance(key, str):
            _error("manifest top-level key is not a string")
        index = _skip_json_whitespace(text, index)
        if index >= len(text) or text[index] != ":":
            _error(f"manifest top-level key {key!r} has no colon")
        index = _skip_json_whitespace(text, index + 1)
        if key != "resources":
            try:
                _value, index = decoder.raw_decode(text, index)
            except json.JSONDecodeError as exc:
                _error(f"cannot locate manifest resources array: {exc}")
        else:
            if index >= len(text) or text[index] != "[":
                _error("manifest resources must be an array")
            index += 1
            spans: list[tuple[int, int]] = []
            commas: list[int] = []
            index = _skip_json_whitespace(text, index)
            if index < len(text) and text[index] == "]":
                return spans, commas
            while True:
                start = _skip_json_whitespace(text, index)
                try:
                    _value, end = decoder.raw_decode(text, start)
                except json.JSONDecodeError as exc:
                    _error(f"cannot locate manifest resource element: {exc}")
                spans.append((start, end))
                index = _skip_json_whitespace(text, end)
                if index < len(text) and text[index] == ",":
                    commas.append(index)
                    index += 1
                    continue
                if index < len(text) and text[index] == "]":
                    return spans, commas
                _error("manifest resources array has an invalid delimiter")
        index = _skip_json_whitespace(text, index)
        if index < len(text) and text[index] == ",":
            index += 1
            continue
        if index < len(text) and text[index] == "}":
            break
        _error("manifest top-level object has an invalid delimiter")
    _error("manifest has no resources array")


def _remove_resource_bytes(
        payload: bytes, selected_index: int, expected_prepared: dict) -> bytes:
    text = payload.decode("utf-8")
    spans, commas = _resource_element_layout(text)
    if len(spans) != len(expected_prepared["resources"]) + 1:
        _error("resources byte layout disagrees with decoded manifest")
    start, end = spans[selected_index]
    if len(spans) == 1:
        prepared_text = text[:start] + text[end:]
    elif selected_index < len(spans) - 1:
        prepared_text = text[:start] + text[commas[selected_index] + 1:]
    else:
        prepared_text = text[:commas[selected_index - 1]] + text[end:]
    prepared = prepared_text.encode("utf-8")
    decoded = _decode_exact(prepared, "prepared migration manifest")
    if decoded != expected_prepared:
        _error("surgical resource removal changed unrelated manifest data")
    return prepared


def _read_stable(path: Path) -> bytes:
    try:
        first = path.read_bytes()
        second = path.read_bytes()
    except OSError as exc:
        _error(f"cannot read stable file {path}: {exc}")
    if first != second:
        _error(f"file changed while reading: {path}")
    return first


def _pending_path(manifest: Path) -> Path:
    return manifest.with_name(PENDING_MANIFEST_NAME)


def _reject_pending(manifest: Path) -> None:
    pending = _pending_path(manifest)
    if pending.exists() or pending.is_symlink():
        _error(f"pending export/recovery marker blocks migration: {pending}")


def _source_root(manifest: Path, value: str | os.PathLike[str] | None) -> Path:
    root = Path(value).absolute() if value is not None else manifest.parent
    if not root.is_dir():
        _error(f"source_root must be an existing directory: {root}")
    try:
        manifest.parent.resolve(strict=True).relative_to(root.resolve(strict=True))
    except (OSError, ValueError) as exc:
        _error(f"owning manifest directory is outside source_root {root}: {exc}")
    return root


def _payload_path(owner: Path, root: Path, source: object, label: str) -> Path:
    try:
        relative = validate_relative_path(source, label)
    except ManifestError as exc:
        _error(str(exc))
    path = owner.joinpath(*relative.split("/"))
    try:
        real_owner = owner.resolve(strict=True)
        real_root = root.resolve(strict=True)
        real_path = path.resolve(strict=True)
        real_path.relative_to(real_owner)
        real_path.relative_to(real_root)
    except (OSError, ValueError) as exc:
        _error(f"{label} escapes owner/source_root or is missing: {path}: {exc}")
    if path.is_symlink() or not path.is_file():
        _error(f"{label} must be a regular non-symlink file: {path}")
    return path.absolute()


def _validate_lod_rows(row: dict, uid: str) -> list[dict]:
    value = row["lods"]
    if not isinstance(value, list):
        _error(f"legacy static_mesh {uid} lods must be an array")
    result = []
    levels = set()
    for index, item in enumerate(value):
        owner = f"legacy static_mesh {uid} lods[{index}]"
        if not isinstance(item, dict) or set(item) != {
                "level", "source", "content_hash"}:
            _error(f"{owner} must contain exactly level, source and content_hash")
        level = item["level"]
        if not isinstance(level, int) or isinstance(level, bool) or level < 1:
            _error(f"{owner}.level must be an integer >= 1")
        if level in levels:
            _error(f"legacy static_mesh {uid} repeats LOD level {level}")
        levels.add(level)
        try:
            source = validate_relative_path(item["source"], f"{owner}.source")
        except ManifestError as exc:
            _error(str(exc))
        required = f"__{uid[:8]}.lod{level}.mesh.fbx"
        if not source.rsplit("/", 1)[-1].endswith(required):
            _error(f"{owner}.source must end with {required!r}")
        content_hash = item["content_hash"]
        if not isinstance(content_hash, str) or not _CONTENT_HASH_RE.fullmatch(
                content_hash):
            _error(f"{owner}.content_hash is invalid")
        result.append({
            "level": level,
            "source": source,
            "content_hash": content_hash,
        })
    return result


def _plan(payload: bytes, uid: str, owner: str) -> tuple[dict, dict, bytes, list[dict]]:
    document = _decode_exact(payload, owner)
    rows = document.get("resources")
    if not isinstance(rows, list):
        _error(f"{owner} resources must be an array")
    matching_indices = [
        index for index, row in enumerate(rows)
        if isinstance(row, dict) and row.get("uid") == uid]
    matches = [rows[index] for index in matching_indices]
    if len(matches) > 1:
        _error(f"ResourceUID {uid} is ambiguous in {owner}")
    if not matches:
        _error(f"ResourceUID {uid} is not owned by {owner}")
    selected = matches[0]
    if selected.get("kind") != "static_mesh":
        _error(f"ResourceUID {uid} is not a static_mesh")
    if "lods" not in selected:
        _error(f"ResourceUID {uid} has no deprecated lods field")
    if set(selected) - _LEGACY_STATIC_MESH_FIELDS:
        _error(
            f"legacy static_mesh {uid} has unknown fields: "
            f"{sorted(set(selected) - _LEGACY_STATIC_MESH_FIELDS)}")
    lods = _validate_lod_rows(selected, uid)

    # Validate the exact legacy manifest as though only the deprecated field
    # were absent. This proves row order, UID uniqueness and all current fields.
    sanitized = deepcopy(document)
    sanitized_selected = sanitized["resources"][matching_indices[0]]
    sanitized_selected.pop("lods")
    try:
        validate_export_manifest(sanitized)
    except ManifestError as exc:
        _error(f"legacy manifest is invalid after removing lods: {exc}")

    prepared = deepcopy(document)
    prepared["resources"] = [
        row for row in prepared["resources"]
        if not isinstance(row, dict) or row.get("uid") != uid]
    # This explicit second validation is the authority proof for every
    # unrelated row and for the exact owner inventory after migration.
    try:
        validate_export_manifest(prepared)
    except ManifestError as exc:
        _error(f"unrelated manifest rows are not current-strict: {exc}")
    prepared_bytes = _remove_resource_bytes(
        payload, matching_indices[0], prepared)
    return document, selected, prepared_bytes, lods


def _acquire_directory_lock(directory: Path):
    path = directory / _DIRECTORY_LOCK_NAME
    try:
        stream = open(path, "a+b")
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
        try:
            stream.close()
        except UnboundLocalError:
            pass
        _error(f"another writer holds owning-manifest lock: {exc}")


def _release_directory_lock(stream) -> None:
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


def _write_temp(path: Path, payload: bytes) -> Path:
    staging = path.with_name(
        f".{path.name}.writing.{os.getpid()}.{uuid.uuid4().hex}")
    try:
        with open(staging, "xb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
    except OSError as exc:
        _error(f"cannot prepare atomic write for {path}: {exc}")
    return staging


def _install_backup_exclusive(path: Path, payload: bytes) -> None:
    if path.exists() or path.is_symlink():
        _error(f"migration backup already exists: {path}")
    staging = _write_temp(path, payload)
    try:
        try:
            os.link(staging, path)
        except FileExistsError:
            _error(f"migration backup already exists: {path}")
        except OSError as exc:
            _error(f"filesystem cannot atomically install exact backup {path}: {exc}")
    finally:
        try:
            staging.unlink()
        except FileNotFoundError:
            pass


def _replace_atomic(path: Path, payload: bytes) -> None:
    staging = _write_temp(path, payload)
    try:
        os.replace(staging, path)
    except OSError as exc:
        _error(f"cannot atomically replace {path}: {exc}")
    finally:
        try:
            staging.unlink()
        except FileNotFoundError:
            pass


def migrate_manifest(
        manifest_path: str | os.PathLike[str], resource_uid: str, *,
        source_root: str | os.PathLike[str] | None = None) -> dict:
    """Back up exact legacy bytes and remove only the selected owner row."""
    uid = _canonical_uid(resource_uid)
    manifest = _manifest_path(manifest_path)
    root = _source_root(manifest, source_root)
    backup = backup_path_for(manifest, uid)

    # Read-only preflight comes before lock-file creation: every ordinary
    # malformed/pending/missing/collision outcome is guaranteed write-free.
    _reject_pending(manifest)
    original = _read_stable(manifest)
    _document, selected, prepared, lods = _plan(
        original, uid, str(manifest))
    if backup.exists() or backup.is_symlink():
        _error(f"migration backup already exists: {backup}")
    primary = _payload_path(
        manifest.parent, root, selected["source"],
        f"static_mesh {uid} primary source")
    lod_payloads = []
    for lod in lods:
        path = _payload_path(
            manifest.parent, root, lod["source"],
            f"static_mesh {uid} LOD {lod['level']} source")
        lod_payloads.append({"level": lod["level"], "path": str(path)})

    lock = _acquire_directory_lock(manifest.parent)
    try:
        _reject_pending(manifest)
        if _read_stable(manifest) != original:
            _error("export_manifest.json changed after migration preflight")
        if backup.exists() or backup.is_symlink():
            _error(f"migration backup already exists: {backup}")
        # Repeat path proofs under the same owning-manifest lock used by the
        # addon writer; a cooperating export cannot invalidate the preflight.
        _payload_path(
            manifest.parent, root, selected["source"],
            f"static_mesh {uid} primary source")
        for lod in lods:
            _payload_path(
                manifest.parent, root, lod["source"],
                f"static_mesh {uid} LOD {lod['level']} source")

        _reject_pending(manifest)
        _install_backup_exclusive(backup, original)
        _reject_pending(manifest)
        if _read_stable(manifest) != original:
            try:
                backup.unlink()
            finally:
                _error("export_manifest.json changed before migration commit")
        _replace_atomic(manifest, prepared)
    except BaseException:
        # Never remove a successfully committed exact backup. It is the
        # recovery authority if replacement raised after entering os.replace.
        raise
    finally:
        _release_directory_lock(lock)

    return {
        "schema": RECEIPT_SCHEMA,
        "schema_version": RECEIPT_SCHEMA_VERSION,
        "operation": "prepare_combined_lod_reexport",
        "resource_uid": uid,
        "manifest_path": str(manifest),
        "backup_path": str(backup),
        "primary_payload": str(primary),
        "lod_payloads": lod_payloads,
        "prepared_manifest_sha256": hashlib.sha256(prepared).hexdigest(),
        "payloads_deleted": False,
        "next_action": NEXT_ACTION,
    }


def restore_manifest(
        manifest_path: str | os.PathLike[str], resource_uid: str) -> dict:
    """Atomically consume the exact backup when active bytes are untouched."""
    uid = _canonical_uid(resource_uid)
    manifest = _manifest_path(manifest_path)
    backup = backup_path_for(manifest, uid)

    # Restore refusal is also read-only until every expected byte is proven.
    _reject_pending(manifest)
    if backup.is_symlink() or not backup.is_file():
        _error(f"exact migration backup is missing or not regular: {backup}")
    original = _read_stable(backup)
    _document, _selected, prepared, _lods = _plan(
        original, uid, str(backup))
    active = _read_stable(manifest)
    if active != prepared:
        _error(
            "active manifest changed since migration; exact restore refused")

    lock = _acquire_directory_lock(manifest.parent)
    try:
        _reject_pending(manifest)
        if backup.is_symlink() or not backup.is_file():
            _error(f"exact migration backup is missing or not regular: {backup}")
        if (_read_stable(backup) != original
                or _read_stable(manifest) != prepared):
            _error("active manifest changed during restore preflight")
        try:
            os.replace(backup, manifest)
        except OSError as exc:
            _error(f"cannot atomically restore exact backup: {exc}")
    finally:
        _release_directory_lock(lock)
    return {
        "schema": RECEIPT_SCHEMA,
        "schema_version": RECEIPT_SCHEMA_VERSION,
        "operation": "restore_legacy_manifest",
        "resource_uid": uid,
        "manifest_path": str(manifest),
        "restored_sha256": hashlib.sha256(original).hexdigest(),
        "backup_consumed": True,
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Remove one deprecated per-file LOD owner row before immediate "
            "Combined-LOD re-export. Payloads are never deleted."))
    parser.add_argument("manifest", help="exact path to owning export_manifest.json")
    parser.add_argument("resource_uid", help="static_mesh ResourceUID to migrate")
    parser.add_argument(
        "--source-root",
        help="optional project source_root; defaults to owning manifest directory")
    parser.add_argument(
        "--restore", action="store_true",
        help="restore exact backup if active prepared manifest is unchanged")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.restore:
            receipt = restore_manifest(args.manifest, args.resource_uid)
        else:
            receipt = migrate_manifest(
                args.manifest, args.resource_uid, source_root=args.source_root)
    except MigrationError as exc:
        print(f"MH_E_LOD_MIGRATION_FAILED: {exc}", file=sys.stderr)
        return 2
    json.dump(receipt, sys.stdout, indent=2, ensure_ascii=False)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
