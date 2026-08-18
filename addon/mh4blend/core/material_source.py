"""Strict, Blender-free self-contained ``mh.material`` codec.

MH Source Protocol v2 stores all material identity and metadata in the payload
itself.  Export directories contain only human-named ``*.material`` files;
there is no owning manifest or directory-local inventory.
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

from .canonical import nfc, sanitize_name, validate_resource_name
from .materials import (
    MaterialValueError,
    material_content_hash,
    material_disk_payload,
)
from .payload_publish_v2 import atomic_publish_bytes

__all__ = [
    "MATERIAL_SCHEMA",
    "MATERIAL_SCHEMA_VERSION",
    "MaterialDiagnostic",
    "MaterialSourceError",
    "PreparedMaterialExport",
    "build_material_document",
    "canonicalize_texture_path",
    "first_create_material_path",
    "material_payload_bytes",
    "prepare_material_export",
    "validate_material_document",
    "write_material_payload_atomic",
]


MATERIAL_SCHEMA = "mh.material"
MATERIAL_SCHEMA_VERSION = 1
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
    """Validated self-contained material payload and its destination."""

    document: dict
    payload_path: str
    diagnostics: tuple[MaterialDiagnostic, ...]

    @property
    def content_hash(self) -> str:
        return material_content_hash(
            self.document["shader_class"], self.document["params"],
            self.document["textures"])


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


def first_create_material_path(
        output_dir: str | os.PathLike, name: str, uid: str) -> str:
    uid = _validate_uid(uid)
    name = _validate_name(name)
    return os.path.abspath(os.path.join(
        os.fspath(output_dir), sanitize_name(name) + MATERIAL_SUFFIX))


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


def _assert_material_target_owner(
        payload_path: str, expected_uid: str, *,
        source_root: str | os.PathLike, texture_policy: str) -> None:
    """Reject an occupied clean path that does not prove expected identity."""
    if not os.path.exists(payload_path):
        return
    try:
        with open(payload_path, "rb") as stream:
            existing_raw = json.loads(stream.read().decode("utf-8"))
        existing, _ = validate_material_document(
            existing_raw, source_root=source_root,
            texture_policy=texture_policy)
    except (OSError, UnicodeError, json.JSONDecodeError,
            MaterialSourceError) as exc:
        raise MaterialSourceError(
            "MH_E_PASSPORT_INVALID", "payload_path",
            "occupied material path has no valid embedded identity") from exc
    if existing["uid"] != expected_uid:
        raise MaterialSourceError(
            "MH_E_NAME_COLLISION_DIFFERENT_UID", "payload_path",
            f"clean filename is owned by {existing['uid']}")


def prepare_material_export(
        *, uid: str, name: str, shader_class: str, params: dict,
        textures: dict, source_root: str | os.PathLike,
        texture_policy: str = "transitional",
        output_dir: str | os.PathLike | None = None,
        target_payload_path: str | os.PathLike | None = None) \
        -> PreparedMaterialExport:
    """Prepare a clean-name first export or a UID-resolved in-place update."""
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
    else:
        payload_path = os.path.abspath(os.fspath(target_payload_path))

    payload_path = _assert_under_source_root(
        payload_path, source_root, "payload_path")
    _assert_material_target_owner(
        payload_path, uid, source_root=source_root,
        texture_policy=texture_policy)
    return PreparedMaterialExport(
        document=document, payload_path=payload_path,
        diagnostics=diagnostics)


def material_payload_bytes(document: dict) -> bytes:
    return (json.dumps(
        document, indent=2, ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")


def write_material_payload_atomic(
        prepared: PreparedMaterialExport, *, force: bool = False,
        source_root: str | os.PathLike,
        texture_policy: str = "transitional",
        pre_replace_guard=None) -> bool:
    """Replace one self-contained payload atomically.

    An explicit artist export always writes. ``force`` remains a compatibility
    keyword for callers from the pre-v2 prototype and has no effect.
    """
    def guarded_owner_check():
        _assert_material_target_owner(
            prepared.payload_path, prepared.document["uid"],
            source_root=source_root, texture_policy=texture_policy)
        if pre_replace_guard is not None:
            pre_replace_guard()

    atomic_publish_bytes(
        prepared.payload_path, material_payload_bytes(prepared.document),
        source_root=source_root,
        pre_replace_guard=guarded_owner_check)
    return True
