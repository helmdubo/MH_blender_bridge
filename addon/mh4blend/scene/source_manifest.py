"""Incremental ``export_manifest.json`` index for standalone source exports.

The manifest is a directory-local UID catalogue, not an export job or a
bundle ownership boundary.  FBX and composite operators upsert only the
resources they explicitly export and preserve unrelated entries.

Writers use ``export_manifest.json.tmp`` as a fail-closed marker.  The marker
is prepared before a payload is replaced and promoted only after that payload
is complete.  Readers reject a directory while the marker exists.
"""

from __future__ import annotations

from copy import deepcopy
import json
import ntpath
import os
import posixpath
import unicodedata

from ..core.materials import MaterialValueError, material_disk_payload


MANIFEST_NAME = "export_manifest.json"
PENDING_MANIFEST_NAME = f"{MANIFEST_NAME}.tmp"
MANIFEST_SCHEMA = "mh.export_manifest"
MANIFEST_SCHEMA_VERSION = 1


class ManifestError(ValueError):
    """Invalid, ambiguous, or currently-being-written source manifest."""


def _manifest_path(directory: str) -> str:
    return os.path.join(directory, MANIFEST_NAME)


def _pending_path(directory: str) -> str:
    return os.path.join(directory, PENDING_MANIFEST_NAME)


def _write_json_atomic(path: str, document: dict) -> None:
    staging = f"{path}.writing"
    with open(staging, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(document, stream, indent=2, ensure_ascii=False)
        stream.write("\n")
    os.replace(staging, path)


def _read_file_bytes(path: str) -> bytes:
    """Small test seam for deterministic reader/writer race coverage."""
    with open(path, "rb") as stream:
        return stream.read()


def _reject_pending(directory: str) -> None:
    if os.path.exists(_pending_path(directory)):
        raise ManifestError(
            f"export in progress or interrupted: {PENDING_MANIFEST_NAME} exists")


def _decode_manifest(payload: bytes, path: str) -> dict:
    try:
        return json.loads(payload.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise ManifestError(f"cannot read {path}: {exc}") from exc


def _canonical_relative_source(source: str, uid: str) -> str:
    """Canonical manifest spelling for a directory-owned payload path.

    Storage remains case-preserving so the path is still usable on
    case-sensitive hosts. Ownership comparison is separately case-folded,
    because the source directory is also consumed on Windows.
    """
    if not isinstance(source, str) or not source:
        raise ManifestError(f"export manifest resource {uid} has no source")
    source = unicodedata.normalize("NFC", source).replace("\\", "/")
    drive, _tail = ntpath.splitdrive(source)
    if (drive or source.startswith("/") or "\x00" in source):
        raise ManifestError(
            f"export manifest resource {uid} source must be relative")
    canonical = posixpath.normpath(source)
    if (canonical in {"", ".", ".."}
            or canonical.startswith("../")):
        raise ManifestError(
            f"export manifest resource {uid} source must be relative")
    return canonical


def _source_owner_key(source: str) -> str:
    """Windows-safe ownership identity for an already canonical path."""
    return source.casefold()


def _rows_by_uid(rows, field: str) -> dict[str, dict]:
    if not isinstance(rows, list):
        raise ManifestError(f"export manifest '{field}' must be an array")
    indexed = {}
    for row in rows:
        if not isinstance(row, dict):
            raise ManifestError(f"export manifest '{field}' rows must be objects")
        uid = row.get("uid")
        if not isinstance(uid, str) or not uid:
            raise ManifestError(f"export manifest '{field}' row has no uid")
        if uid in indexed:
            raise ManifestError(
                f"export manifest '{field}' contains duplicate uid {uid}")
        indexed[uid] = deepcopy(row)
    return indexed


def _required_string(row: dict, field: str, owner: str) -> str:
    value = row.get(field)
    if not isinstance(value, str) or not value:
        raise ManifestError(
            f"export manifest {owner} has invalid required {field}")
    return value


def _normalize_material_slots(uid: str, value) -> list[dict]:
    if not isinstance(value, list):
        raise ManifestError(
            f"export manifest static_mesh {uid} material_slots must be an array")
    normalized = []
    slot_owners = {}
    for index, slot in enumerate(value):
        owner = f"static_mesh {uid} material_slots[{index}]"
        if not isinstance(slot, dict):
            raise ManifestError(f"export manifest {owner} must be an object")
        slot_name = unicodedata.normalize(
            "NFC", _required_string(slot, "slot_name", owner))
        material_uid = _required_string(slot, "material_uid", owner)
        if slot_name in slot_owners:
            raise ManifestError(
                f"export manifest static_mesh {uid} contains duplicate "
                f"material slot_name {slot_name!r}")
        slot_owners[slot_name] = material_uid
        normalized.append({
            "slot_name": slot_name,
            "material_uid": material_uid,
        })
    return normalized


def _normalize_resource_row(uid: str, row: dict) -> dict:
    owner = f"resource {uid}"
    # UID was indexed already, but validate the same required writer shape in
    # one place so future callers cannot bypass it with a custom mapping.
    _required_string(row, "uid", owner)
    kind = _required_string(row, "kind", owner)
    if kind not in {"static_mesh", "composite"}:
        raise ManifestError(
            f"export manifest resource {uid} has unsupported kind {kind!r}")
    row["name"] = unicodedata.normalize(
        "NFC", _required_string(row, "name", owner))
    row["source"] = _canonical_relative_source(
        _required_string(row, "source", owner), uid)
    _required_string(row, "content_hash", owner)
    if "properties" in row and not isinstance(row["properties"], dict):
        raise ManifestError(
            f"export manifest resource {uid} properties must be an object")
    if kind == "static_mesh":
        if "material_slots" in row:
            row["material_slots"] = _normalize_material_slots(
                uid, row["material_slots"])
    elif "material_slots" in row:
        raise ManifestError(
            f"export manifest composite {uid} cannot contain material_slots")
    return row


def _normalize_material_row(uid: str, row: dict) -> dict:
    owner = f"material {uid}"
    _required_string(row, "uid", owner)
    if _required_string(row, "kind", owner) != "material":
        raise ManifestError(
            f"export manifest material {uid} has unsupported kind "
            f"{row.get('kind')!r}")
    row["name"] = unicodedata.normalize(
        "NFC", _required_string(row, "name", owner))
    _required_string(row, "content_hash", owner)
    shader_class = _required_string(row, "shader_class", owner)
    if not isinstance(row.get("params"), dict):
        raise ManifestError(
            f"export manifest material {uid} params must be an object")
    textures = row.get("textures")
    if not isinstance(textures, dict):
        raise ManifestError(
            f"export manifest material {uid} textures must be an object")
    for slot, path in textures.items():
        if (not isinstance(slot, str) or not slot
                or not isinstance(path, str) or not path):
            raise ManifestError(
                f"export manifest material {uid} texture slots and paths "
                "must be non-empty strings")
    try:
        payload = material_disk_payload(
            shader_class, row["params"], textures)
    except MaterialValueError as exc:
        raise ManifestError(
            f"export manifest material {uid} has invalid payload: {exc}") from exc
    row["shader_class"] = payload["shader_class"]
    row["params"] = payload["params"]
    row["textures"] = payload["textures"]
    return row


def _empty_manifest(exporter_version: str, blend_file: str) -> dict:
    return {
        "schema": MANIFEST_SCHEMA,
        "schema_version": MANIFEST_SCHEMA_VERSION,
        "exporter_version": exporter_version,
        "source": {"blend_file": blend_file},
        "resources": [],
        "materials": [],
    }


def _upgrade_legacy(document: dict) -> dict:
    """Read the old coupled exporter output without retaining bundle fields."""
    if document.get("schema") != "mh.bundle_manifest" \
            or document.get("schema_version") != 2:
        return document
    return {
        "schema": MANIFEST_SCHEMA,
        "schema_version": MANIFEST_SCHEMA_VERSION,
        "exporter_version": document.get("exporter_version", ""),
        "source": deepcopy(document.get("source", {})),
        "resources": deepcopy(document.get("resources", [])),
        "materials": deepcopy(document.get("materials", [])),
    }


def validate_export_manifest(
        document: dict, *, strict_references: bool = False) -> dict:
    """Validate and return a normalized copy of a source manifest.

    Shape/type validation is always strict. Reader mode deliberately permits a
    mesh slot to reference a material row that is not in this directory index:
    the composite importer represents it as unresolved and emits
    ``MH_W_MATERIAL_NOT_FOUND``. Writers pass ``strict_references=True`` so
    they can never stage a newly dangling material reference.
    """
    if not isinstance(strict_references, bool):
        raise TypeError("strict_references must be a bool")
    if not isinstance(document, dict):
        raise ManifestError("export manifest must be a JSON object")
    document = _upgrade_legacy(deepcopy(document))
    if document.get("schema") != MANIFEST_SCHEMA:
        raise ManifestError(
            f"unsupported export manifest schema {document.get('schema')!r}")
    version = document.get("schema_version")
    if (not isinstance(version, int) or isinstance(version, bool)
            or version != MANIFEST_SCHEMA_VERSION):
        raise ManifestError(
            "unsupported export manifest schema_version "
            f"{version!r}")
    resources = _rows_by_uid(document.get("resources", []), "resources")
    materials = _rows_by_uid(document.get("materials", []), "materials")
    shared_uids = resources.keys() & materials.keys()
    if shared_uids:
        raise ManifestError(
            "export manifest UID appears as resource and material: "
            + ", ".join(sorted(shared_uids)))
    source_owners = {}
    for uid, row in resources.items():
        row = _normalize_resource_row(uid, row)
        resources[uid] = row
        source = row["source"]
        owner_key = _source_owner_key(source)
        previous = source_owners.get(owner_key)
        if previous is not None and previous != uid:
            raise ManifestError(
                f"export manifest source {source!r} is shared by {previous} "
                f"and {uid}")
        source_owners[owner_key] = uid
    for uid, row in materials.items():
        materials[uid] = _normalize_material_row(uid, row)
    for uid, row in resources.items():
        if row["kind"] != "static_mesh":
            continue
        for slot in row.get("material_slots", []):
            material_uid = slot["material_uid"]
            if strict_references and material_uid not in materials:
                raise ManifestError(
                    f"export manifest static_mesh {uid} material slot "
                    f"{slot['slot_name']!r} references missing material "
                    f"{material_uid}")
    source = document.get("source", {})
    if not isinstance(source, dict):
        raise ManifestError("export manifest 'source' must be an object")
    return {
        "schema": MANIFEST_SCHEMA,
        "schema_version": MANIFEST_SCHEMA_VERSION,
        "exporter_version": str(document.get("exporter_version", "")),
        "source": deepcopy(source),
        "resources": [resources[uid] for uid in sorted(resources)],
        "materials": [materials[uid] for uid in sorted(materials)],
    }


def _read_stable_manifest_payload(
        directory: str, *, allow_missing: bool) -> bytes | None:
    """Return one stable raw snapshot, or ``None`` for a stable absence.

    The marker is checked before, between, and after two byte snapshots. This
    closes the pre-check/read race and also rejects a non-conforming writer
    that replaces the stable manifest without leaving its marker observable.
    """
    _reject_pending(directory)
    path = _manifest_path(directory)
    try:
        first = _read_file_bytes(path)
    except FileNotFoundError as exc:
        _reject_pending(directory)
        if allow_missing:
            # The first standalone export may have committed between the
            # failed open and the marker check. Treat appearance as a race,
            # never as a stable "missing" snapshot.
            if os.path.exists(path):
                raise ManifestError(
                    f"cannot read stable {path}: manifest appeared while reading")
            _reject_pending(directory)
            return None
        raise ManifestError(f"{MANIFEST_NAME} not found in {directory}") from exc
    except OSError as exc:
        raise ManifestError(f"cannot read {path}: {exc}") from exc
    _reject_pending(directory)
    try:
        second = _read_file_bytes(path)
    except OSError as exc:
        raise ManifestError(f"cannot read stable {path}: {exc}") from exc
    _reject_pending(directory)
    if first != second:
        raise ManifestError(
            f"cannot read stable {path}: manifest changed while reading")
    return first


def load_export_manifest_snapshot(
        directory: str, *, allow_missing: bool = False
        ) -> tuple[dict | None, bytes | None]:
    """Load a stable document plus an opaque raw-byte stability token.

    Consumers that read payloads named by the manifest must retain the token
    and call :func:`assert_manifest_stable` after all payload reads,
    immediately before applying the plan. This extends the fail-closed marker
    protocol across the whole consumer transaction, not only JSON parsing.
    ``None`` is the token for a manifest that was stably absent.
    """
    payload = _read_stable_manifest_payload(
        directory, allow_missing=allow_missing)
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
        raise ManifestError(
            f"cannot apply source plan: {MANIFEST_NAME} changed after it was read")


def load_export_manifest(directory: str, *, allow_missing: bool = False) -> dict | None:
    """Backward-compatible document-only stable manifest reader."""
    document, _token = load_export_manifest_snapshot(
        directory, allow_missing=allow_missing)
    return document


def _reconciled_source(base: dict, blend_file: str,
                       *, had_stable_manifest: bool) -> dict:
    """Keep ``blend_file`` only while the directory is provably single-source.

    A directory manifest is an incremental index. Once exports from different
    blend files are merged, a top-level owner would be misleading; absence of
    the optional key is the schema-compatible representation of mixed/unknown
    provenance. It intentionally stays absent on later updates.
    """
    if not had_stable_manifest:
        return {"blend_file": blend_file}
    source = deepcopy(base.get("source", {}))
    previous = source.get("blend_file")
    if previous is None:
        return source
    if (not isinstance(previous, str)
            or previous.casefold() != str(blend_file).casefold()):
        source.pop("blend_file", None)
    return source


def prepare_manifest_update(
        directory: str, *, resources=(), materials=(),
        exporter_version: str, blend_file: str) -> dict:
    """Merge explicit UID rows while preserving every unrelated entry.

    A stale pending state may be recovered only by an export that explicitly
    rewrites every UID changed by that pending transaction.  This prevents a
    later, unrelated standalone export from committing a manifest hash for a
    payload whose earlier replace may or may not have completed.
    """
    stable_path = _manifest_path(directory)
    had_stable_manifest = os.path.exists(stable_path)
    if had_stable_manifest:
        try:
            with open(stable_path, encoding="utf-8") as stream:
                base = validate_export_manifest(
                    json.load(stream), strict_references=True)
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            raise ManifestError(f"cannot read {stable_path}: {exc}") from exc
    else:
        base = _empty_manifest(exporter_version, blend_file)

    incoming_resources = _rows_by_uid(list(resources), "resources")
    incoming_materials = _rows_by_uid(list(materials), "materials")
    pending_path = _pending_path(directory)
    pending_resources = {}
    if os.path.exists(pending_path):
        try:
            with open(pending_path, encoding="utf-8") as stream:
                pending = validate_export_manifest(
                    json.load(stream), strict_references=True)
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            raise ManifestError(f"cannot recover {pending_path}: {exc}") from exc
        stable_resources = _rows_by_uid(base["resources"], "resources")
        pending_resources = _rows_by_uid(pending["resources"], "resources")
        changed_resources = {
            uid for uid in stable_resources.keys() | pending_resources.keys()
            if stable_resources.get(uid) != pending_resources.get(uid)}
        missing_resources = changed_resources - incoming_resources.keys()
        # Material rows are inline manifest metadata and have no independently
        # replaced payload. They can be safely recomputed or discarded by the
        # next explicit export. Only changed resource rows can describe a
        # payload whose atomic replace may already have happened.
        if missing_resources:
            missing = sorted(missing_resources)
            raise ManifestError(
                "interrupted export must be recovered by re-exporting UID(s): "
                + ", ".join(missing))

    resource_index = _rows_by_uid(base["resources"], "resources")
    material_index = _rows_by_uid(base["materials"], "materials")
    for uid, row in incoming_resources.items():
        previous = resource_index.get(uid)
        if previous is not None and previous.get("kind") != row.get("kind"):
            raise ManifestError(
                f"resource {uid} cannot change kind from "
                f"{previous.get('kind')!r} to {row.get('kind')!r}")
        interrupted = pending_resources.get(uid)
        if (interrupted is not None
                and interrupted.get("kind") != row.get("kind")):
            raise ManifestError(
                f"resource {uid} cannot change kind from interrupted "
                f"{interrupted.get('kind')!r} to {row.get('kind')!r}")
        resource_index[uid] = row
    for uid, row in incoming_materials.items():
        material_index[uid] = row

    result = _empty_manifest(exporter_version, blend_file)
    result["source"] = _reconciled_source(
        base, blend_file, had_stable_manifest=had_stable_manifest)
    result["resources"] = [
        resource_index[uid] for uid in sorted(resource_index)]
    result["materials"] = [
        material_index[uid] for uid in sorted(material_index)]
    # Return the same canonical document stage_manifest will persist. Besides
    # making aliases deterministic for callers, this catches source ownership
    # collisions before a pending transaction is staged.
    return validate_export_manifest(result, strict_references=True)


def stage_manifest(directory: str, document: dict) -> str:
    """Create/replace the fail-closed pending manifest and return its path."""
    os.makedirs(directory, exist_ok=True)
    normalized = validate_export_manifest(document, strict_references=True)
    path = _pending_path(directory)
    _write_json_atomic(path, normalized)
    return path


def commit_staged_manifest(directory: str) -> str:
    """Promote a previously staged manifest after payload completion."""
    pending = _pending_path(directory)
    if not os.path.exists(pending):
        raise ManifestError(f"no staged {PENDING_MANIFEST_NAME} to commit")
    final = _manifest_path(directory)
    os.replace(pending, final)
    return final


def manifest_resource_index(document: dict) -> dict[str, dict]:
    """UID lookup used by dependency-aware composite import."""
    normalized = validate_export_manifest(document)
    return {row["uid"]: row for row in normalized["resources"]}
