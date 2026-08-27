"""One physical, immutable scan of Source Protocol payload candidates.

The inventory is deliberately bpy-free.  Every encountered filesystem path is
resolved before containment or identity checks, so symlink/junction aliases
cannot bypass the Project Source Root boundary.  Multiple lexical aliases of
one physical file collapse to one candidate; different physical files that
claim one ResourceKey remain ambiguous.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
from types import MappingProxyType
from typing import Mapping

from .canonical import validate_resource_name
from .materials import MATERIAL_TEXTURE_EXTENSIONS
from .source_closure import ResourceKey
from .validate import MHValidationError

__all__ = [
    "InvalidSourceCandidate",
    "SourceCandidate",
    "SourceInventory",
    "SourceSnapshot",
    "scan_source_inventory",
]


_FIXED_SUFFIXES = (
    (".mesh.fbx", "static_mesh"),
    (".composite", "composite"),
    (".placement", "placement_profile"),
    (".material", "material"),
)


def _physical_key(path: Path) -> str:
    return os.path.normcase(os.path.normpath(str(path)))


def _inside(root: Path, path: Path) -> bool:
    try:
        return os.path.commonpath([
            _physical_key(root), _physical_key(path),
        ]) == _physical_key(root)
    except ValueError:
        return False


def _raise(code: str, subjects, message: str):
    raise MHValidationError(code, subjects, message)


@dataclass(frozen=True)
class InvalidSourceCandidate:
    """Noncanonical source-like file; it owns no ResourceKey."""

    path: Path
    kind: str
    claimed_name: str
    message: str

    def claims(self, key: ResourceKey) -> bool:
        return (
            self.kind == key.kind
            and self.claimed_name.casefold() == key.name.casefold()
        )


def _resource_key(path: Path) -> ResourceKey | InvalidSourceCandidate | None:
    filename = path.name
    folded = filename.casefold()
    for suffix, kind in _FIXED_SUFFIXES:
        if not folded.endswith(suffix):
            continue
        if not filename.endswith(suffix):
            return InvalidSourceCandidate(
                path, kind, filename[:-len(suffix)],
                f"source filename suffix must be exactly {suffix!r}",
            )
        name = filename[:-len(suffix)]
        try:
            validate_resource_name(name)
        except (TypeError, ValueError) as exc:
            return InvalidSourceCandidate(path, kind, name, str(exc))
        return ResourceKey(kind, name)

    suffix = path.suffix
    if suffix.casefold() not in MATERIAL_TEXTURE_EXTENSIONS:
        return None
    if suffix not in MATERIAL_TEXTURE_EXTENSIONS:
        return InvalidSourceCandidate(
            path, "texture", path.stem,
            "texture extension must use its exact lowercase spelling",
        )
    try:
        validate_resource_name(path.stem)
    except (TypeError, ValueError) as exc:
        return InvalidSourceCandidate(path, "texture", path.stem, str(exc))
    return ResourceKey("texture", path.stem)


@dataclass(frozen=True)
class SourceSnapshot:
    """Exact observation used for final-edge batch revalidation."""

    path: Path
    size: int
    sha256: str


@dataclass(frozen=True)
class SourceCandidate:
    """One physical file claiming one ResourceKey."""

    key: ResourceKey
    path: Path

    def read_bytes(self) -> bytes:
        return self.path.read_bytes()

    def snapshot(self) -> SourceSnapshot:
        payload = self.read_bytes()
        return SourceSnapshot(
            path=self.path,
            size=len(payload),
            sha256=hashlib.sha256(payload).hexdigest(),
        )


@dataclass(frozen=True)
class SourceInventory:
    """Immutable result of one physically canonicalized source scan."""

    root: Path
    candidates: Mapping[ResourceKey, tuple[SourceCandidate, ...]]
    invalid_candidates: tuple[InvalidSourceCandidate, ...] = ()

    def candidates_for(self, key: ResourceKey) -> tuple[SourceCandidate, ...]:
        if not isinstance(key, ResourceKey):
            raise TypeError("key must be ResourceKey")
        return self.candidates.get(key, ())

    def resolve(self, key: ResourceKey, *, allow_missing=False) -> SourceCandidate | None:
        rows = self.candidates_for(key)
        if len(rows) > 1:
            _raise(
                "MH_E_AMBIGUOUS_RESOURCE_NAME",
                [str(key), *(row.path for row in rows)],
                "multiple physical source payloads claim one ResourceKey",
            )
        if not rows:
            invalid = tuple(
                row for row in self.invalid_candidates if row.claims(key))
            if invalid:
                _raise(
                    "MH_E_NONCANONICAL_RESOURCE_NAME",
                    [str(key), *(row.path for row in invalid)],
                    "only noncanonical source candidates claim the required "
                    "logical identity: "
                    + "; ".join(row.message for row in invalid),
                )
            if allow_missing:
                return None
            _raise(
                "MH_E_RESOURCE_NOT_FOUND",
                [str(key)],
                "required managed source payload was not found",
            )
        return rows[0]


def scan_source_inventory(source_root: str | os.PathLike) -> SourceInventory:
    """Scan one source root exactly once without following paths outside it."""

    lexical_root = Path(source_root)
    try:
        root = lexical_root.resolve(strict=True)
    except OSError as exc:
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE",
            [lexical_root],
            f"Project Source Root cannot be resolved: {exc}",
        )
    if not root.is_dir():
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE",
            [lexical_root, root],
            "Project Source Root is not a directory",
        )

    pending = [lexical_root]
    visited_directories: set[str] = set()
    physical_files: dict[
        str, SourceCandidate | InvalidSourceCandidate] = {}
    while pending:
        lexical_directory = pending.pop()
        try:
            physical_directory = lexical_directory.resolve(strict=True)
        except OSError as exc:
            _raise(
                "MH_E_INVALID_RESOURCE_SOURCE",
                [lexical_directory],
                f"scanned directory cannot be resolved: {exc}",
            )
        if not _inside(root, physical_directory):
            _raise(
                "MH_E_INVALID_RESOURCE_SOURCE",
                [lexical_directory, physical_directory],
                "scanned directory resolves outside physical source_root",
            )
        directory_key = _physical_key(physical_directory)
        if directory_key in visited_directories:
            continue
        visited_directories.add(directory_key)

        try:
            with os.scandir(lexical_directory) as scanner:
                entries = sorted(scanner, key=lambda row: row.name)
        except OSError as exc:
            _raise(
                "MH_E_INVALID_RESOURCE_SOURCE",
                [lexical_directory, physical_directory],
                f"source directory cannot be scanned: {exc}",
            )
        for entry in entries:
            lexical_path = Path(entry.path)
            try:
                physical = lexical_path.resolve(strict=True)
            except OSError as exc:
                _raise(
                    "MH_E_INVALID_RESOURCE_SOURCE",
                    [lexical_path],
                    f"scanned path cannot be resolved: {exc}",
                )
            if not _inside(root, physical):
                # Texture files are targeted, preflight-only dependencies.
                # Keep an outside-root file symlink out of the managed
                # inventory without globally blocking unrelated closures; the
                # ratified texture resolver will emit the precise
                # MH_E_TEXTURE_OUTSIDE_ROOT if this logical key is referenced.
                if (physical.is_file()
                        and lexical_path.suffix.casefold()
                        in MATERIAL_TEXTURE_EXTENSIONS):
                    physical_files.setdefault(
                        "outside-texture:" + _physical_key(
                            lexical_path.absolute()),
                        InvalidSourceCandidate(
                            lexical_path,
                            "texture",
                            lexical_path.stem,
                            f"texture resolves outside source_root: {physical}",
                        ),
                    )
                    continue
                _raise(
                    "MH_E_INVALID_RESOURCE_SOURCE",
                    [lexical_path, physical],
                    "scanned path resolves outside physical source_root",
                )
            if physical.is_dir():
                pending.append(lexical_path)
                continue
            if not physical.is_file():
                continue
            classified = _resource_key(physical)
            if classified is None:
                continue
            candidate = (
                SourceCandidate(classified, physical)
                if isinstance(classified, ResourceKey) else classified)
            physical_files.setdefault(_physical_key(physical), candidate)

    grouped: dict[ResourceKey, list[SourceCandidate]] = {}
    invalid = []
    for candidate in physical_files.values():
        if isinstance(candidate, SourceCandidate):
            grouped.setdefault(candidate.key, []).append(candidate)
        else:
            invalid.append(candidate)
    frozen = {
        key: tuple(sorted(
            rows, key=lambda row: str(row.path).replace("\\", "/")))
        for key, rows in grouped.items()
    }
    return SourceInventory(
        root,
        MappingProxyType(frozen),
        tuple(sorted(
            invalid, key=lambda row: str(row.path).replace("\\", "/"))),
    )
