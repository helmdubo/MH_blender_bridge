"""Blender-free texture basename resolution and path actualization.

The authored texture path remains the identity.  This module only repairs a
stale path when exactly one file with the same *full* basename exists below
the project Source Root.  Modification times never participate and files are
never copied.
"""

from __future__ import annotations

from dataclasses import dataclass
import ntpath
import os
import posixpath
from typing import Any

from .canonical import nfc
from .material_source import canonicalize_texture_path

__all__ = [
    "TextureActualizeError",
    "TexturePathResult",
    "TextureTreeSnapshot",
    "actualize_material_document",
    "actualize_texture_path",
    "assert_texture_tree_stable",
    "capture_texture_tree",
    "texture_basename_key",
]


class TextureActualizeError(ValueError):
    """The Source Root cannot be scanned without escaping its boundary."""

    def __init__(self, code: str, message: str):
        self.code = code
        super().__init__(f"{code}: {message}")


@dataclass(frozen=True)
class TexturePathResult:
    status: str
    authored_path: str
    basename: str
    resolved_path: str | None = None
    candidates: tuple[str, ...] = ()
    code: str | None = None

    def disk_dict(self) -> dict:
        result = {
            "status": self.status,
            "path": self.authored_path,
            "basename": self.basename,
        }
        if self.resolved_path is not None:
            result["resolved_path"] = self.resolved_path
        if self.candidates:
            result["candidates"] = list(self.candidates)
        if self.code is not None:
            result["code"] = self.code
        return result


@dataclass(frozen=True)
class TextureTreeSnapshot:
    source_root: str
    real_source_root: str
    files: tuple[str, ...]
    basename_index: dict[str, tuple[str, ...]]


def _path_key(path: str) -> str:
    return nfc(os.path.normcase(os.path.normpath(path)))


def _inside(root: str, path: str) -> bool:
    try:
        return os.path.commonpath([
            os.path.normcase(root), os.path.normcase(path)]) == \
            os.path.normcase(root)
    except ValueError:
        return False


def _canonical_root(source_root: str | os.PathLike) -> tuple[str, str]:
    if not isinstance(source_root, (str, os.PathLike)):
        raise TextureActualizeError(
            "MH_E_INVALID_RESOURCE_SOURCE", "source_root must be a path")
    root = os.path.normpath(os.path.abspath(os.fspath(source_root)))
    if not os.path.isabs(root) or not os.path.isdir(root):
        raise TextureActualizeError(
            "MH_E_INVALID_RESOURCE_SOURCE",
            f"source_root is not an absolute directory: {root}")
    real_root = os.path.normpath(os.path.realpath(root))
    return root, real_root


def _inventory(root: str, real_root: str) -> tuple[str, ...]:
    files: dict[str, str] = {}
    for directory, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames.sort(key=lambda item: (nfc(item).casefold(), nfc(item)))
        filenames.sort(key=lambda item: (nfc(item).casefold(), nfc(item)))
        for filename in filenames:
            # Source Schema v1 writer coordination files are not texture
            # candidates and may be created between the snapshot guard and
            # marker staging by this same operation.
            if (filename in {".mh_source_root.lock", ".mh_manifest_writer.lock"}
                    or filename == "export_manifest.json.tmp"
                    or ".mh-tmp-" in filename
                    or ".writing." in filename):
                continue
            path = os.path.normpath(os.path.abspath(os.path.join(
                directory, filename)))
            real_path = os.path.normpath(os.path.realpath(path))
            # A relative Source path resolving through a symlink outside the
            # project is not an internal texture candidate.
            if not _inside(real_root, real_path):
                continue
            if not os.path.isfile(path):
                continue
            key = _path_key(path)
            previous = files.get(key)
            if previous is not None and previous != path:
                raise TextureActualizeError(
                    "MH_E_INVALID_RESOURCE_SOURCE",
                    "source_root contains paths that collide after platform "
                    f"normalization: {previous!r}, {path!r}")
            files[key] = path
    return tuple(files[key] for key in sorted(files))


def texture_basename_key(path: str) -> str:
    """Return NFC + casefold key for a complete filename including extension."""
    if not isinstance(path, str) or not path:
        raise ValueError("texture path must be a non-empty string")
    # On every host understand both Source Protocol forward slashes and old
    # Windows-authored paths.
    basename = ntpath.basename(path.replace("/", "\\"))
    return nfc(basename).casefold()


def capture_texture_tree(
        source_root: str | os.PathLike) -> TextureTreeSnapshot:
    """Capture a stable path-only snapshot and basename index of Source Root."""
    root, real_root = _canonical_root(source_root)
    first = _inventory(root, real_root)
    second = _inventory(root, real_root)
    if first != second:
        raise TextureActualizeError(
            "MH_E_INVALID_RESOURCE_SOURCE",
            "source_root file set changed while indexing textures")
    mutable: dict[str, list[str]] = {}
    for path in first:
        mutable.setdefault(texture_basename_key(path), []).append(path)
    index = {
        key: tuple(sorted(paths, key=lambda item: (
            _path_key(os.path.relpath(item, root)), nfc(item))))
        for key, paths in mutable.items()
    }
    return TextureTreeSnapshot(root, real_root, first, index)


def assert_texture_tree_stable(snapshot: TextureTreeSnapshot) -> None:
    """Fail if any candidate pathname appeared or disappeared since capture."""
    first = _inventory(snapshot.source_root, snapshot.real_source_root)
    second = _inventory(snapshot.source_root, snapshot.real_source_root)
    if first != snapshot.files or second != snapshot.files:
        raise TextureActualizeError(
            "MH_E_INVALID_RESOURCE_SOURCE",
            "source_root file set changed after texture index snapshot")


def _authored_absolute(authored_path: str, root: str) -> str:
    drive, _tail = ntpath.splitdrive(authored_path)
    if drive or authored_path.startswith(("/", "\\")):
        return os.path.normpath(authored_path.replace("/", os.sep))
    parts = authored_path.replace("\\", "/").split("/")
    return os.path.normpath(os.path.abspath(os.path.join(root, *parts)))


def _exact_file(authored_path: str, snapshot: TextureTreeSnapshot) -> bool:
    candidate = _authored_absolute(authored_path, snapshot.source_root)
    if not os.path.isabs(candidate):
        return False
    # Relative paths are required to remain physically inside Source Root.
    drive, _tail = ntpath.splitdrive(authored_path)
    relative = not drive and not authored_path.startswith(("/", "\\"))
    if relative:
        if not _inside(snapshot.source_root, candidate):
            raise TextureActualizeError(
                "MH_E_INVALID_RESOURCE_SOURCE",
                f"texture path escapes source_root: {authored_path}")
        real_candidate = os.path.normpath(os.path.realpath(candidate))
        if not _inside(snapshot.real_source_root, real_candidate):
            raise TextureActualizeError(
                "MH_E_INVALID_RESOURCE_SOURCE",
                f"texture path resolves outside source_root: {authored_path}")
    return os.path.isfile(candidate)


def _relative_candidate(path: str, snapshot: TextureTreeSnapshot) -> str:
    normalized, diagnostics = canonicalize_texture_path(
        path, snapshot.source_root, texture_policy="transitional")
    if diagnostics or posixpath.isabs(normalized) or ntpath.splitdrive(normalized)[0]:
        raise TextureActualizeError(
            "MH_E_INVALID_RESOURCE_SOURCE",
            f"indexed texture candidate is outside source_root: {path}")
    return normalized


def actualize_texture_path(
        authored_path: str, snapshot: TextureTreeSnapshot) -> TexturePathResult:
    """Apply exact -> unique basename -> ambiguous/missing resolution cascade."""
    if not isinstance(authored_path, str) or not authored_path:
        raise ValueError("texture path must be a non-empty string")
    basename = ntpath.basename(authored_path.replace("/", "\\"))
    if _exact_file(authored_path, snapshot):
        return TexturePathResult("exact", authored_path, nfc(basename))
    candidates = snapshot.basename_index.get(texture_basename_key(authored_path), ())
    relative_candidates = tuple(
        _relative_candidate(path, snapshot) for path in candidates)
    if len(relative_candidates) == 1:
        return TexturePathResult(
            "fixed", authored_path, nfc(basename),
            resolved_path=relative_candidates[0], candidates=relative_candidates)
    if len(relative_candidates) > 1:
        return TexturePathResult(
            "ambiguous", authored_path, nfc(basename),
            candidates=relative_candidates,
            code="MH_W_TEXTURE_BASENAME_AMBIGUOUS")
    return TexturePathResult(
        "missing", authored_path, nfc(basename),
        code="MH_W_TEXTURE_NOT_FOUND")


def actualize_material_document(
        document: dict[str, Any], snapshot: TextureTreeSnapshot,
        ) -> tuple[dict[str, Any], tuple[tuple[str, TexturePathResult], ...]]:
    """Return an independent material document with uniquely stale paths fixed."""
    if (not isinstance(document, dict)
            or not isinstance(document.get("textures"), dict)):
        raise ValueError("material document must contain a textures object")
    updated = dict(document)
    updated_textures = dict(document["textures"])
    results = []
    for slot in sorted(updated_textures):
        result = actualize_texture_path(updated_textures[slot], snapshot)
        results.append((slot, result))
        if result.status == "fixed":
            updated_textures[slot] = result.resolved_path
    updated["textures"] = updated_textures
    return updated, tuple(results)
