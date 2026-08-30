"""Ordered, crash-observable publication for one preflighted source batch.

The caller owns closure construction, staging and final-edge validation.  This
module owns only the irreversible prefix: every item is atomically replaced in
the supplied dependency order, and a soft failure after the first replace is
reported with exact published/unpublished identities.  There is deliberately
no transaction manifest or cross-process watcher token.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
import time

from . import payload_publish_v2
from .payload_publish_v2 import atomic_publish_bytes

__all__ = [
    "BatchPartialPublishError",
    "BatchPublishItem",
    "publish_ordered_batch",
]


@dataclass(frozen=True)
class BatchPublishItem:
    """One already-staged payload in irreversible publication order."""

    identity: str
    target: Path
    payload: bytes

    def __post_init__(self) -> None:
        if not isinstance(self.identity, str) or not self.identity:
            raise ValueError("batch item identity must be a non-empty string")
        if not isinstance(self.target, Path):
            raise TypeError("batch item target must be pathlib.Path")
        if not isinstance(self.payload, bytes):
            raise TypeError("batch item payload must be bytes")


class BatchPartialPublishError(RuntimeError):
    """A soft failure after an irreversible, dependency-closed prefix."""

    code = "MH_E_PARTIAL_PUBLISH"

    def __init__(
        self,
        *,
        published: tuple[str, ...],
        unpublished: tuple[str, ...],
        cause: BaseException,
    ) -> None:
        if not published:
            raise ValueError("partial publish requires a non-empty published set")
        self.published = published
        self.unpublished = unpublished
        self.cause = cause
        super().__init__(
            f"{self.code}: published={list(published)!r}; "
            f"unpublished={list(unpublished)!r}; cause={cause}")


def publish_ordered_batch(
    items,
    *,
    source_root,
    pre_replace_guard: Callable[[tuple[str, ...]], None],
    replace_observer: Callable[[BatchPublishItem], None] | None = None,
    lock_root=None,
    _boundary_hook: Callable[[str, BatchPublishItem, tuple[str, ...]], None]
    | None = None,
    _crash_identity: str | None = None,
    _crash_at: str | None = None,
) -> tuple[dict, ...]:
    """Replace ``items`` in order and expose every replace boundary to tests.

    ``_boundary_hook`` and the crash arguments are private verification seams;
    product callers leave them unset.  A real process crash cannot emit a
    diagnostic, so crash acceptance observes the complete prefix on disk.
    """

    rows = tuple(items)
    if any(not isinstance(row, BatchPublishItem) for row in rows):
        raise TypeError("items must contain only BatchPublishItem values")
    identities = tuple(row.identity for row in rows)
    if len(set(identities)) != len(identities):
        raise ValueError("batch item identities must be unique")
    if not callable(pre_replace_guard):
        raise TypeError("pre_replace_guard must be callable")
    if replace_observer is not None and not callable(replace_observer):
        raise TypeError("replace_observer must be callable")
    if _boundary_hook is not None and not callable(_boundary_hook):
        raise TypeError("_boundary_hook must be callable")
    if _crash_at not in {None, "before_replace", "after_replace"}:
        raise ValueError("_crash_at must be before_replace or after_replace")
    if (_crash_identity is None) != (_crash_at is None):
        raise ValueError("crash identity and boundary must be supplied together")
    if _crash_identity is not None and _crash_identity not in identities:
        raise ValueError("crash identity is not a member of this batch")

    published: list[str] = []
    receipts = []
    for row in rows:
        try:
            if _boundary_hook is not None:
                _boundary_hook("before_replace", row, tuple(published))
            crash_at = _crash_at if row.identity == _crash_identity else None
            def replaced():
                published.append(row.identity)
                if replace_observer is not None:
                    replace_observer(row)

            receipt = atomic_publish_bytes(
                row.target,
                row.payload,
                source_root=source_root,
                lock_root=lock_root,
                pre_replace_guard=lambda: pre_replace_guard(tuple(published)),
                replace_observer=replaced,
                fsync_parent=False,
                _crash_at=crash_at,
            )
            receipts.append(receipt)
            if _boundary_hook is not None:
                _boundary_hook("after_replace", row, tuple(published))
        except Exception as exc:
            if not published:
                raise
            unpublished = tuple(
                identity for identity in identities if identity not in published)
            raise BatchPartialPublishError(
                published=tuple(published),
                unpublished=unpublished,
                cause=exc,
            ) from exc
    if receipts:
        try:
            started = time.monotonic()
            fsynced = False
            directories = tuple(dict.fromkeys(
                row.target.resolve(strict=False).parent for row in rows))
            for directory in directories:
                fsynced = (
                    payload_publish_v2._fsync_parent_directory(directory)
                    or fsynced)
            elapsed_ms = round((time.monotonic() - started) * 1000.0, 3)
            receipts[-1]["parent_directory_fsynced"] = fsynced
            receipts[-1]["parent_fsync_directories"] = len(directories)
            receipts[-1]["timings_ms"]["parent_fsync"] = elapsed_ms
            receipts[-1]["elapsed_ms"] = round(
                receipts[-1]["elapsed_ms"] + elapsed_ms, 3)
        except Exception as exc:
            raise BatchPartialPublishError(
                published=tuple(published),
                unpublished=(),
                cause=exc,
            ) from exc
    return tuple(receipts)
