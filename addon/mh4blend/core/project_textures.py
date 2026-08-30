"""Safe filesystem projection for importing external Dagor textures."""

from __future__ import annotations

from contextlib import ExitStack
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import tempfile

from .dagor_names import project_dagor_resource_name
from .materials import MATERIAL_TEXTURE_EXTENSIONS
from .payload_publish_v2 import payload_lock

__all__ = [
    "ProjectTextureError",
    "TextureCopyPlan",
    "atomic_copy_texture_plans",
    "plan_project_texture",
    "validate_texture_plans",
]


class ProjectTextureError(ValueError):
    """One authored texture path cannot be projected without ambiguity."""

    def __init__(self, code: str, path: str, message: str):
        self.code = code
        self.path = path
        self.message = message
        super().__init__(f"{code}: {path}: {message}")


@dataclass(frozen=True)
class TextureCopyPlan:
    source: Path
    destination: Path


def _path_key(path: Path) -> str:
    return os.path.normcase(str(path.resolve(strict=False))).casefold()


def _inside(root: Path, path: Path) -> bool:
    try:
        return os.path.commonpath([
            os.path.normcase(str(root)), os.path.normcase(str(path)),
        ]) == os.path.normcase(str(root))
    except ValueError:
        return False


def _path_from_transport(value) -> Path:
    """Parse Blender-authored paths with either platform's separators."""
    return Path(os.fspath(value).replace("\\", "/"))


def plan_project_texture(authored_path, project_root) -> TextureCopyPlan:
    """Map ``.../assets/<tail>`` to ``<project_root>/assets/<tail>``."""
    if not isinstance(authored_path, (str, os.PathLike)):
        raise ProjectTextureError(
            "MH_E_INVALID_RESOURCE_SOURCE", repr(authored_path),
            "texture path must be a filesystem path")
    raw_path = os.fspath(authored_path)
    if not raw_path.strip():
        raise ProjectTextureError(
            "MH_E_INVALID_RESOURCE_SOURCE", raw_path,
            "texture path is empty")

    root = _path_from_transport(project_root).resolve(strict=False)
    if not root.is_dir():
        raise ProjectTextureError(
            "MH_E_INVALID_RESOURCE_SOURCE", str(root),
            "Project Source Root does not exist")
    source = _path_from_transport(raw_path).resolve(strict=False)
    suffix = source.suffix.lower()
    if suffix not in MATERIAL_TEXTURE_EXTENSIONS:
        raise ProjectTextureError(
            "MH_E_NONCANONICAL_RESOURCE_NAME", str(source),
            "texture filename must use a supported image extension")
    try:
        projected_stem = project_dagor_resource_name(source.stem)
    except ValueError as exc:
        raise ProjectTextureError(
            "MH_E_NONCANONICAL_RESOURCE_NAME", str(source),
            "texture filename stem must contain only ASCII letters, digits, "
            "underscore and projectable whitespace") from exc

    assets_indices = [
        index for index, part in enumerate(source.parts)
        if part.casefold() == "assets"
    ]
    if len(assets_indices) != 1:
        raise ProjectTextureError(
            "MH_E_INVALID_RESOURCE_SOURCE", str(source),
            "texture path must contain exactly one 'assets' folder segment")
    assets_index = assets_indices[0]
    tail = source.parts[assets_index + 1:]
    if not tail:
        raise ProjectTextureError(
            "MH_E_INVALID_RESOURCE_SOURCE", str(source),
            "texture path must contain a file below the assets folder")

    projected_tail = (*tail[:-1], projected_stem + suffix)
    destination = (
        root / "assets" / Path(*projected_tail)).resolve(strict=False)
    if not _inside(root, destination):
        raise ProjectTextureError(
            "MH_E_INVALID_RESOURCE_SOURCE", str(source),
            "projected texture path escapes Project Source Root")
    return TextureCopyPlan(source=source, destination=destination)


def validate_texture_plans(
        plans, *, require_sources: bool, require_destinations: bool = False
        ) -> list[TextureCopyPlan]:
    """Preflight and deduplicate plans without mutating the filesystem."""
    unique: dict[str, TextureCopyPlan] = {}
    for plan in plans:
        if not isinstance(plan, TextureCopyPlan):
            raise TypeError("plans must contain TextureCopyPlan values")
        destination_key = _path_key(plan.destination)
        previous = unique.get(destination_key)
        if previous is not None:
            if _path_key(previous.source) != _path_key(plan.source):
                same_payload = (
                    previous.source.is_file()
                    and plan.source.is_file()
                    and previous.source.stat().st_size == plan.source.stat().st_size
                    and _sha256_file(previous.source) == _sha256_file(plan.source)
                )
                if not same_payload:
                    raise ProjectTextureError(
                        "MH_E_AMBIGUOUS_RESOURCE_NAME", str(plan.destination),
                        "different external textures project to the same "
                        f"path: {previous.source}, {plan.source}")
                # After Remap, one carrier already points at the project
                # destination while another may still name the original CDK
                # file.  Equal bytes are one resource; retain the self-plan so
                # a repeated Copy All is a verified no-op rather than a write.
                if _path_key(plan.source) == destination_key:
                    unique[destination_key] = plan
            continue
        unique[destination_key] = plan

    ordered = sorted(unique.values(), key=lambda row: _path_key(row.destination))
    for plan in ordered:
        if require_sources and not plan.source.is_file():
            raise ProjectTextureError(
                "MH_E_INVALID_RESOURCE_SOURCE", str(plan.source),
                "external texture file does not exist")
        if plan.destination.exists() and plan.destination.is_dir():
            raise ProjectTextureError(
                "MH_E_INVALID_RESOURCE_SOURCE", str(plan.destination),
                "project texture destination is a directory")
        if require_destinations and not plan.destination.is_file():
            raise ProjectTextureError(
                "MH_E_UNRESOLVED_TEXTURE_REFERENCE", str(plan.destination),
                "copy textures to the project before remapping paths")
    return ordered


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _file_snapshot(path: Path):
    if not path.exists():
        return None
    stat = path.stat()
    return (stat.st_size, stat.st_mtime_ns, _sha256_file(path))


def _snapshot_content(snapshot):
    return None if snapshot is None else (snapshot[0], snapshot[2])


def _copy_verified(source: Path, destination: Path) -> str:
    digest = hashlib.sha256()
    with source.open("rb") as reader, destination.open("wb") as writer:
        while chunk := reader.read(1024 * 1024):
            digest.update(chunk)
            writer.write(chunk)
        writer.flush()
        os.fsync(writer.fileno())
    source_digest = digest.hexdigest()
    if _sha256_file(destination) != source_digest:
        raise OSError(f"staged texture read-back differs: {source}")
    return source_digest


def _sibling_temp(destination: Path, marker: str) -> Path:
    descriptor, temp_name = tempfile.mkstemp(
        prefix=f".{destination.name}.{marker}-", dir=destination.parent)
    os.close(descriptor)
    return Path(temp_name)


def atomic_copy_texture_plans(plans, *, source_root=None) -> dict:
    """Transactionally stage, verify and replace every projected texture."""
    ordered = validate_texture_plans(plans, require_sources=True)
    actionable = [
        plan for plan in ordered
        if _path_key(plan.source) != _path_key(plan.destination)
    ]
    staged: list[tuple[TextureCopyPlan, Path, str]] = []
    backups: dict[str, Path | None] = {}
    destination_snapshots: dict[str, tuple | None] = {}
    temporary_paths: set[Path] = set()
    preserved_backups: set[Path] = set()
    skipped = []
    replaced = []
    for plan in ordered:
        if _path_key(plan.source) == _path_key(plan.destination):
            skipped.append(str(plan.destination))
    for plan in actionable:
        plan.destination.parent.mkdir(parents=True, exist_ok=True)

    with ExitStack() as locks:
        for plan in actionable:
            locks.enter_context(payload_lock(
                plan.destination, source_root=source_root))
        try:
            for plan in actionable:
                destination_key = _path_key(plan.destination)
                destination_snapshots[destination_key] = _file_snapshot(
                    plan.destination)
                backup = None
                if plan.destination.exists():
                    backup = _sibling_temp(plan.destination, "mh-backup")
                    temporary_paths.add(backup)
                    backups[destination_key] = backup
                    _copy_verified(plan.destination, backup)
                else:
                    backups[destination_key] = None

                source_snapshot = _file_snapshot(plan.source)
                if source_snapshot is None:
                    raise ProjectTextureError(
                        "MH_E_UNRESOLVED_TEXTURE_REFERENCE", str(plan.source),
                        "external texture disappeared during copy")
                temp = _sibling_temp(plan.destination, "mh-tmp")
                temporary_paths.add(temp)
                source_digest = _copy_verified(plan.source, temp)
                if _file_snapshot(plan.source) != source_snapshot:
                    raise ProjectTextureError(
                        "MH_E_INVALID_RESOURCE_SOURCE", str(plan.source),
                        "external texture changed while it was being staged")
                staged.append((plan, temp, source_digest))

            for plan, _temp, source_digest in staged:
                if _sha256_file(plan.source) != source_digest:
                    raise ProjectTextureError(
                        "MH_E_INVALID_RESOURCE_SOURCE", str(plan.source),
                        "external texture changed before project commit")
                destination_key = _path_key(plan.destination)
                if (_file_snapshot(plan.destination)
                        != destination_snapshots[destination_key]):
                    raise ProjectTextureError(
                        "MH_E_INVALID_RESOURCE_SOURCE", str(plan.destination),
                        "project texture changed concurrently")

            committed = []
            try:
                for plan, temp, source_digest in staged:
                    destination_key = _path_key(plan.destination)
                    if (_file_snapshot(plan.destination)
                            != destination_snapshots[destination_key]):
                        raise ProjectTextureError(
                            "MH_E_INVALID_RESOURCE_SOURCE",
                            str(plan.destination),
                            "project texture changed immediately before "
                            "commit")
                    os.replace(temp, plan.destination)
                    committed.append((plan, source_digest))
                    replaced.append(str(plan.destination))
            except Exception as commit_error:
                rollback_errors = []
                for plan, source_digest in reversed(committed):
                    backup = backups[_path_key(plan.destination)]
                    try:
                        current = _file_snapshot(plan.destination)
                        if current is None or current[2] != source_digest:
                            if backup is not None and backup.exists():
                                preserved_backups.add(backup)
                            raise RuntimeError(
                                "destination changed after transaction "
                                "commit; external bytes were preserved")
                        if backup is None:
                            plan.destination.unlink(missing_ok=True)
                        else:
                            restore = _sibling_temp(
                                plan.destination, "mh-restore")
                            temporary_paths.add(restore)
                            _copy_verified(backup, restore)
                            os.replace(restore, plan.destination)
                        expected = destination_snapshots[
                            _path_key(plan.destination)]
                        if (_snapshot_content(_file_snapshot(plan.destination))
                                != _snapshot_content(expected)):
                            raise RuntimeError("rollback read-back differs")
                    except Exception as rollback_error:
                        backup_note = ""
                        if backup is not None and backup.exists():
                            preserved_backups.add(backup)
                            backup_note = f"; backup preserved at {backup}"
                        rollback_errors.append(
                            f"{plan.destination}: {rollback_error}"
                            f"{backup_note}")
                if rollback_errors:
                    raise RuntimeError(
                        f"texture copy commit failed: {commit_error}; "
                        "rollback also failed: " + "; ".join(rollback_errors)
                    ) from commit_error
                raise
        finally:
            for path in temporary_paths:
                if path.exists() and path not in preserved_backups:
                    path.unlink()

    return {
        "ok": True,
        "copied": len(replaced),
        "skipped": len(skipped),
        "destinations": replaced,
        "already_project_paths": skipped,
    }
