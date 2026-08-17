"""Strict, Blender-free ``mh.material`` Source Schema v1 codec.

The material payload is independent from the owning ``export_manifest.json``.
This module prepares both documents' material-specific pieces, but deliberately
does not resolve owners or update manifests: the global source resolver owns
that decision.  A caller either supplies the resolved payload/manifest pair or
supplies a directory for first creation.
"""

from __future__ import annotations

from dataclasses import dataclass
import json
import ntpath
import os
import posixpath
import re
import uuid
from typing import Any, Iterable

from .canonical import nfc, resource_filename, validate_resource_name
from .materials import (
    MaterialValueError,
    material_content_hash,
    material_disk_payload,
)

__all__ = [
    "MATERIAL_SCHEMA",
    "MATERIAL_SCHEMA_VERSION",
    "MaterialDiagnostic",
    "MaterialSourceError",
    "PreparedMaterialExport",
    "build_material_document",
    "canonicalize_texture_path",
    "commit_material_payload",
    "first_create_material_path",
    "material_payload_bytes",
    "material_payload_matches",
    "material_resource_row",
    "prepare_material_export",
    "stage_material_payload",
    "validate_material_document",
    "validate_material_resource_row",
    "write_material_payload_atomic",
]


MATERIAL_SCHEMA = "mh.material"
MATERIAL_SCHEMA_VERSION = 1
MANIFEST_NAME = "export_manifest.json"
MATERIAL_SUFFIX = ".material"
TEXTURE_POLICIES = frozenset({"transitional", "strict"})
_MATERIAL_FIELDS = frozenset({
    "schema", "schema_version", "uid", "name", "shader_class", "params",
    "textures",
})
_TEXTURE_SLOT = re.compile(r"tex(?:[0-9]|1[0-5])\Z")
_WINDOWS_DRIVE_ABSOLUTE = re.compile(r"^[A-Za-z]:[\\/]")


class MaterialSourceError(ValueError):
    """A material source value violates frozen Source Schema v1."""

    def __init__(self, code: str, path: str, message: str):
        self.code = code
        self.path = path
        super().__init__(f"{code}: {path}: {message}")


@dataclass(frozen=True)
class MaterialDiagnostic:
    code: str
    path: str
    message: str
    subjects: tuple[str, ...] = ()

    def disk_dict(self) -> dict:
        return {
            "code": self.code,
            "subjects": list(self.subjects),
            "message": f"{self.path}: {self.message}",
        }

    def for_subject(self, uid: str) -> "MaterialDiagnostic":
        return MaterialDiagnostic(
            self.code, self.path, self.message, (uid,))


@dataclass(frozen=True)
class PreparedMaterialExport:
    """Validated material payload plus its owning-manifest row.

    ``owning_manifest_path`` is explicit even for first creation.  This keeps
    the transaction seam honest: a higher-level writer stages that manifest,
    then atomically replaces ``payload_path``, then promotes the marker.
    """

    document: dict
    resource_row: dict
    payload_path: str
    owning_manifest_path: str
    diagnostics: tuple[MaterialDiagnostic, ...]

    @property
    def content_hash(self) -> str:
        return self.resource_row["content_hash"]


def _error(path: str, message: str, code: str = "MH_E_INVALID_MATERIAL_VALUE"):
    raise MaterialSourceError(code, path, message)


def _validate_uid(value: Any, path: str = "uid") -> str:
    if not isinstance(value, str):
        _error(path, "must be a lowercase UUID string")
    try:
        parsed = uuid.UUID(value)
    except (ValueError, AttributeError) as exc:
        raise MaterialSourceError(
            "MH_E_INVALID_MATERIAL_VALUE", path,
            "must be a lowercase UUID string") from exc
    canonical = str(parsed)
    if value != canonical:
        _error(path, "must use lowercase canonical UUID spelling with dashes")
    return canonical


def _validate_name(value: Any) -> str:
    if not isinstance(value, str):
        _error("name", "must be a string")
    normalized = nfc(value)
    try:
        validate_resource_name(normalized)
    except (TypeError, ValueError) as exc:
        raise MaterialSourceError(
            "MH_E_NON_ASCII_RESOURCE_NAME", "name", str(exc)) from exc
    return normalized


def _validate_policy(texture_policy: str) -> str:
    if texture_policy not in TEXTURE_POLICIES:
        raise ValueError(
            "texture_policy must be 'transitional' or 'strict'")
    return texture_policy


def _is_windows_absolute(path: str) -> bool:
    return bool(_WINDOWS_DRIVE_ABSOLUTE.match(path)) or path.startswith(("//", "\\\\"))


def _is_posix_absolute(path: str) -> bool:
    return path.startswith("/") and not path.startswith("//")


def _path_flavour(path: str) -> str | None:
    if _is_windows_absolute(path):
        return "windows"
    if _is_posix_absolute(path):
        return "posix"
    return None


def _normalize_absolute(path: str, flavour: str) -> str:
    if flavour == "windows":
        native = ntpath.normpath(path.replace("/", "\\"))
        drive, tail = ntpath.splitdrive(native)
        if len(drive) == 2 and drive[1] == ":":
            drive = drive[0].upper() + ":"
            native = drive + tail
        return native.replace("\\", "/")
    return posixpath.normpath(path.replace("\\", "/"))


def _absolute_inside_root(path: str, path_flavour: str,
                          root: str, root_flavour: str) -> bool:
    if path_flavour != root_flavour:
        return False
    try:
        if root_flavour == "windows":
            path_native = path.replace("/", "\\").casefold()
            root_native = root.replace("/", "\\").casefold()
            return ntpath.commonpath([path_native, root_native]) == root_native
        return posixpath.commonpath([path, root]) == root
    except ValueError:
        return False


def _relative_to_root(path: str, root: str, flavour: str) -> str:
    if flavour == "windows":
        value = ntpath.relpath(
            path.replace("/", "\\"), root.replace("/", "\\"))
        return value.replace("\\", "/")
    return posixpath.relpath(path, root)


def _normalized_source_root(source_root: str | os.PathLike) -> tuple[str, str]:
    if not isinstance(source_root, (str, os.PathLike)):
        raise TypeError("source_root must be an absolute path")
    raw = os.fspath(source_root).strip()
    flavour = _path_flavour(raw)
    if flavour is None:
        _error("source_root", "must be an absolute path")
    return _normalize_absolute(raw, flavour), flavour


def _external_diagnostic(path: str, texture_policy: str,
                         diagnostics: list[MaterialDiagnostic]) -> None:
    message = "texture path is outside source_root"
    if texture_policy == "strict":
        _error(path, message, "MH_E_TEXTURE_OUTSIDE_ROOT")
    diagnostics.append(MaterialDiagnostic(
        "MH_W_TEXTURE_OUTSIDE_ROOT", path, message))


def canonicalize_texture_path(
        authored_path: str, source_root: str | os.PathLike, *,
        texture_policy: str = "transitional",
        diagnostic_path: str = "textures") -> tuple[str, tuple[MaterialDiagnostic, ...]]:
    """Normalize one already-expanded authored texture reference.

    Blender ``//`` paths must be expanded by the host adapter before this pure
    function is called; a leading ``//`` here is therefore an authored UNC.
    Files are never probed or copied.
    """
    _validate_policy(texture_policy)
    if not isinstance(authored_path, str):
        _error(diagnostic_path, "texture path must be a string")
    value = nfc(authored_path.strip())
    if not value:
        _error(diagnostic_path, "texture path must be non-empty")
    root, root_flavour = _normalized_source_root(source_root)
    flavour = _path_flavour(value)
    if flavour is None:
        if root_flavour == "windows":
            joined = ntpath.join(
                root.replace("/", "\\"), value.replace("/", "\\"))
        else:
            joined = posixpath.join(root, value.replace("\\", "/"))
        absolute = _normalize_absolute(joined, root_flavour)
        flavour = root_flavour
    else:
        absolute = _normalize_absolute(value, flavour)

    if _absolute_inside_root(absolute, flavour, root, root_flavour):
        relative = _relative_to_root(absolute, root, root_flavour)
        if relative in {"", "."}:
            _error(diagnostic_path, "texture path cannot name source_root itself")
        return relative, ()

    diagnostics: list[MaterialDiagnostic] = []
    _external_diagnostic(diagnostic_path, texture_policy, diagnostics)
    return absolute, tuple(diagnostics)


def _validate_disk_texture_path(
        value: str, source_root: str | os.PathLike, *, texture_policy: str,
        diagnostic_path: str) -> tuple[str, tuple[MaterialDiagnostic, ...]]:
    if not isinstance(value, str) or not value:
        _error(diagnostic_path, "texture path must be a non-empty string")
    if value != nfc(value):
        _error(diagnostic_path, "texture path must be NFC-normalized")
    root, root_flavour = _normalized_source_root(source_root)
    flavour = _path_flavour(value)
    if flavour is None:
        if "\\" in value or value.startswith("/"):
            _error(diagnostic_path, "relative texture path must use forward slashes")
        normalized = posixpath.normpath(value)
        if (normalized != value or normalized in {"", ".", ".."}
                or normalized.startswith("../")):
            _error(diagnostic_path, "relative texture path is not normalized")
        # A normalized relative path is internal by definition and resolves
        # beneath root because dot/dot-dot segments are forbidden.
        return value, ()

    normalized = _normalize_absolute(value, flavour)
    if normalized != value:
        _error(diagnostic_path, "absolute texture path is not normalized")
    if _absolute_inside_root(normalized, flavour, root, root_flavour):
        _error(diagnostic_path,
               "absolute texture path inside source_root must be relative")
    diagnostics: list[MaterialDiagnostic] = []
    _external_diagnostic(diagnostic_path, texture_policy, diagnostics)
    return normalized, tuple(diagnostics)


def _normalize_texture_slots(
        textures: Any, source_root: str | os.PathLike, *,
        texture_policy: str, on_disk: bool) -> tuple[dict, tuple[MaterialDiagnostic, ...]]:
    if not isinstance(textures, dict):
        _error("textures", "must be an object")
    normalized: dict[str, str] = {}
    diagnostics: list[MaterialDiagnostic] = []
    source_keys: dict[str, str] = {}
    for raw_slot, raw_path in textures.items():
        if not isinstance(raw_slot, str):
            _error("textures", "slot names must be strings")
        slot = nfc(raw_slot)
        if slot in normalized:
            _error("textures", "slot names collide after NFC normalization")
        if not _TEXTURE_SLOT.fullmatch(slot):
            _error(f"textures.{slot}", "slot must be tex0 through tex15")
        source_keys[slot] = raw_slot
        if not on_disk and isinstance(raw_path, str) and not raw_path.strip():
            # Writer omits empty dagormat slots.
            continue
        if on_disk:
            path, rows = _validate_disk_texture_path(
                raw_path, source_root, texture_policy=texture_policy,
                diagnostic_path=f"textures.{slot}")
        else:
            path, rows = canonicalize_texture_path(
                raw_path, source_root, texture_policy=texture_policy,
                diagnostic_path=f"textures.{slot}")
        normalized[slot] = path
        diagnostics.extend(rows)
    ordered = {
        slot: normalized[slot]
        for slot in sorted(normalized, key=lambda item: int(item[3:]))
    }
    return ordered, tuple(diagnostics)


def build_material_document(
        *, uid: str, name: str, shader_class: str, params: dict,
        textures: dict, source_root: str | os.PathLike,
        texture_policy: str = "transitional") -> tuple[dict, tuple[MaterialDiagnostic, ...]]:
    """Build a normalized writer document from authored material values."""
    uid = _validate_uid(uid)
    name = _validate_name(name)
    _validate_policy(texture_policy)
    normalized_textures, diagnostics = _normalize_texture_slots(
        textures, source_root, texture_policy=texture_policy, on_disk=False)
    try:
        payload = material_disk_payload(
            shader_class, params, normalized_textures)
    except MaterialValueError as exc:
        raise MaterialSourceError(exc.code, exc.path, str(exc)) from exc
    if not payload["shader_class"]:
        _error("shader_class", "must be a non-empty string")
    document = {
        "schema": MATERIAL_SCHEMA,
        "schema_version": MATERIAL_SCHEMA_VERSION,
        "uid": uid,
        "name": name,
        "shader_class": payload["shader_class"],
        "params": payload["params"],
        "textures": payload["textures"],
    }
    return document, tuple(row.for_subject(uid) for row in diagnostics)


def validate_material_document(
        document: Any, *, source_root: str | os.PathLike,
        texture_policy: str = "transitional") -> tuple[dict, tuple[MaterialDiagnostic, ...]]:
    """Strictly validate and normalize one on-disk ``mh.material`` v1."""
    _validate_policy(texture_policy)
    if not isinstance(document, dict):
        _error("material", "document must be an object")
    unknown = set(document) - _MATERIAL_FIELDS
    missing = _MATERIAL_FIELDS - set(document)
    if unknown:
        _error("material", "unknown field(s): " + ", ".join(sorted(unknown)))
    if missing:
        _error("material", "missing field(s): " + ", ".join(sorted(missing)))
    if document["schema"] != MATERIAL_SCHEMA:
        _error("schema", f"must be {MATERIAL_SCHEMA!r}")
    version = document["schema_version"]
    if (not isinstance(version, int) or isinstance(version, bool)
            or version != MATERIAL_SCHEMA_VERSION):
        _error("schema_version", "must be integer 1")
    uid = _validate_uid(document["uid"])
    name = _validate_name(document["name"])
    textures, diagnostics = _normalize_texture_slots(
        document["textures"], source_root,
        texture_policy=texture_policy, on_disk=True)
    try:
        payload = material_disk_payload(
            document["shader_class"], document["params"], textures)
    except MaterialValueError as exc:
        raise MaterialSourceError(exc.code, exc.path, str(exc)) from exc
    if not payload["shader_class"]:
        _error("shader_class", "must be a non-empty string")
    normalized = {
        "schema": MATERIAL_SCHEMA,
        "schema_version": MATERIAL_SCHEMA_VERSION,
        "uid": uid,
        "name": name,
        "shader_class": payload["shader_class"],
        "params": payload["params"],
        "textures": payload["textures"],
    }
    return normalized, diagnostics


def _validate_relative_source(source: Any, uid: str) -> str:
    if not isinstance(source, str) or not source:
        _error("source", "must be a non-empty relative path")
    if "\\" in source or _path_flavour(source) is not None:
        _error("source", "must be relative and use forward slashes")
    normalized = posixpath.normpath(source)
    if (normalized != source or normalized in {".", ".."}
            or normalized.startswith("../")):
        _error("source", "must be a normalized path without dot segments")
    expected = f"__{uid[:8]}{MATERIAL_SUFFIX}"
    if not posixpath.basename(source).endswith(expected):
        _error("source", f"basename must end with {expected!r}")
    return source


def material_resource_row(document: dict, source: str) -> dict:
    """Build the exact manifest row for a validated material document."""
    uid = _validate_uid(document.get("uid"))
    name = _validate_name(document.get("name"))
    source = _validate_relative_source(source, uid)
    return {
        "uid": uid,
        "kind": "material",
        "name": name,
        "source": source,
        "content_hash": material_content_hash(
            document["shader_class"], document["params"],
            document["textures"]),
    }


def validate_material_resource_row(row: Any) -> dict:
    fields = {"uid", "kind", "name", "source", "content_hash"}
    if not isinstance(row, dict):
        _error("resource", "material row must be an object")
    if set(row) != fields:
        _error("resource", "material row must contain exactly "
               + ", ".join(sorted(fields)))
    uid = _validate_uid(row["uid"], "resource.uid")
    if row["kind"] != "material":
        _error("resource.kind", "must be 'material'")
    name = _validate_name(row["name"])
    source = _validate_relative_source(row["source"], uid)
    content_hash = row["content_hash"]
    if (not isinstance(content_hash, str)
            or not re.fullmatch(r"xxh3:[0-9a-f]{16}", content_hash)):
        _error("resource.content_hash", "must be xxh3 plus 16 lowercase hex digits")
    return {
        "uid": uid, "kind": "material", "name": name,
        "source": source, "content_hash": content_hash,
    }


def first_create_material_path(
        output_dir: str | os.PathLike, name: str, uid: str) -> str:
    uid = _validate_uid(uid)
    name = _validate_name(name)
    return os.path.abspath(os.path.join(
        os.fspath(output_dir), resource_filename(name, uid, MATERIAL_SUFFIX)))


def _assert_under_source_root(path: str, source_root: str | os.PathLike,
                              field: str) -> str:
    absolute = os.path.abspath(path)
    root = os.path.abspath(os.fspath(source_root))
    try:
        inside = os.path.commonpath([os.path.normcase(absolute),
                                     os.path.normcase(root)]) == os.path.normcase(root)
    except ValueError:
        inside = False
    if not inside:
        _error(field, "must be inside source_root")
    return absolute


def prepare_material_export(
        *, uid: str, name: str, shader_class: str, params: dict,
        textures: dict, source_root: str | os.PathLike,
        texture_policy: str = "transitional",
        output_dir: str | os.PathLike | None = None,
        target_payload_path: str | os.PathLike | None = None,
        owning_manifest_path: str | os.PathLike | None = None,
        existing_source: str | None = None) -> PreparedMaterialExport:
    """Prepare first-create or resolved-owner material output.

    For an existing UID, the global resolver supplies ``target_payload_path``
    and ``owning_manifest_path`` (and may supply the manifest row's
    ``existing_source`` for an exact ownership check).  Without a target this
    function performs first-create naming in ``output_dir``.
    """
    document, diagnostics = build_material_document(
        uid=uid, name=name, shader_class=shader_class, params=params,
        textures=textures, source_root=source_root,
        texture_policy=texture_policy)
    uid = document["uid"]
    if target_payload_path is None:
        if output_dir is None:
            raise ValueError(
                "output_dir is required for first material export")
        payload_path = first_create_material_path(output_dir, name, uid)
        manifest_path = (os.path.join(os.fspath(output_dir), MANIFEST_NAME)
                         if owning_manifest_path is None
                         else os.fspath(owning_manifest_path))
    else:
        payload_path = os.path.abspath(os.fspath(target_payload_path))
        if owning_manifest_path is None:
            raise ValueError(
                "owning_manifest_path is required for an existing material")
        manifest_path = os.fspath(owning_manifest_path)

    payload_path = _assert_under_source_root(
        payload_path, source_root, "payload_path")
    manifest_path = _assert_under_source_root(
        manifest_path, source_root, "owning_manifest_path")
    if os.path.basename(manifest_path) != MANIFEST_NAME:
        _error("owning_manifest_path", f"must name {MANIFEST_NAME}")
    manifest_dir = os.path.dirname(manifest_path)
    try:
        source = os.path.relpath(payload_path, manifest_dir).replace("\\", "/")
    except ValueError as exc:
        raise MaterialSourceError(
            "MH_E_INVALID_RESOURCE_SOURCE", "source", str(exc)) from exc
    source = _validate_relative_source(source, uid)
    if existing_source is not None and source != existing_source:
        _error("source", "resolved payload path does not match owning manifest row",
               "MH_E_INVALID_RESOURCE_SOURCE")
    row = material_resource_row(document, source)
    return PreparedMaterialExport(
        document=document, resource_row=row, payload_path=payload_path,
        owning_manifest_path=manifest_path, diagnostics=diagnostics)


def material_payload_bytes(document: dict) -> bytes:
    return (json.dumps(
        document, indent=2, ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")


def material_payload_matches(
        path: str | os.PathLike, document: dict, *,
        source_root: str | os.PathLike,
        texture_policy: str = "transitional") -> bool:
    try:
        with open(path, "rb") as stream:
            existing = json.loads(stream.read().decode("utf-8"))
        normalized, _diagnostics = validate_material_document(
            existing, source_root=source_root, texture_policy=texture_policy)
    except (OSError, UnicodeError, json.JSONDecodeError, MaterialSourceError):
        return False
    return normalized == document


def stage_material_payload(
        prepared: PreparedMaterialExport, *, temp_path: str | None = None) -> str:
    """Write a complete sibling temp payload; do not replace stable output."""
    path = temp_path or prepared.payload_path + ".tmp"
    os.makedirs(os.path.dirname(prepared.payload_path), exist_ok=True)
    try:
        with open(path, "wb") as stream:
            stream.write(material_payload_bytes(prepared.document))
            stream.flush()
            os.fsync(stream.fileno())
    except Exception:
        try:
            os.remove(path)
        except OSError:
            pass
        raise
    return path


def commit_material_payload(
        prepared: PreparedMaterialExport, staged_path: str | os.PathLike) -> str:
    """Atomically replace the stable payload from a completely written temp."""
    os.replace(os.fspath(staged_path), prepared.payload_path)
    return prepared.payload_path


def write_material_payload_atomic(
        prepared: PreparedMaterialExport, *, force: bool = False,
        source_root: str | os.PathLike,
        texture_policy: str = "transitional") -> bool:
    """Replace one payload atomically; caller owns manifest marker ordering.

    Returns ``True`` when bytes were replaced and ``False`` for a semantic +
    metadata hash-skip.  Recovery callers pass ``force=True``.
    """
    if not force and material_payload_matches(
            prepared.payload_path, prepared.document,
            source_root=source_root, texture_policy=texture_policy):
        return False
    staged = stage_material_payload(prepared)
    try:
        commit_material_payload(prepared, staged)
    finally:
        try:
            os.remove(staged)
        except OSError:
            pass
    return True
