"""Crash-safe publication primitives for MH Source Protocol v4.

The source payload is the authority.  This module therefore publishes a whole
file with a sibling temporary file and ``os.replace`` while holding an
OS-released lock for the canonical destination path.  Lock files live in the
host-local cache, never in the project source tree.
"""

from __future__ import annotations

from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unicodedata

__all__ = [
    "PayloadPublishV2Error",
    "atomic_publish_bytes",
    "atomic_publish_json",
    "canonical_payload_path",
    "default_lock_root",
    "lock_path_for",
    "payload_lock",
]


class PayloadPublishV2Error(RuntimeError):
    """A fail-closed publication error with a stable diagnostic code."""

    def __init__(self, code: str, message: str):
        self.code = code
        super().__init__(f"{code}: {message}")


def canonical_payload_path(path: str | os.PathLike) -> str:
    """Return the local canonical spelling used to derive the lock key."""
    raw = os.path.realpath(os.path.abspath(os.fspath(path)))
    return unicodedata.normalize(
        "NFC", os.path.normcase(os.path.normpath(raw)))


def default_lock_root(
        *, local_appdata: str | os.PathLike | None = None,
        home: str | os.PathLike | None = None,
        platform_name: str | None = None) -> Path:
    """Return the host-local lock directory outside project source trees."""
    platform_name = platform_name or sys.platform
    user_home = Path(home) if home is not None else Path.home()
    if platform_name.startswith("win"):
        base = Path(
            local_appdata if local_appdata is not None else
            os.environ.get("LOCALAPPDATA", user_home / "AppData" / "Local"))
    elif platform_name == "darwin":
        base = user_home / "Library" / "Caches"
    else:
        base = Path(os.environ.get(
            "XDG_CACHE_HOME", user_home / ".cache"))
    return base / "MimirHead" / "MHBridge" / "payload-locks"


def lock_path_for(
        payload_path: str | os.PathLike,
        lock_root: str | os.PathLike) -> Path:
    canonical = canonical_payload_path(payload_path)
    key = hashlib.sha256(canonical.encode("utf-8")).hexdigest()
    return Path(lock_root).resolve(strict=False) / f"{key}.lock"


def _try_lock(stream) -> bool:
    stream.seek(0)
    if os.name == "nt":
        import msvcrt
        try:
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            return True
        except OSError:
            return False
    import fcntl
    try:
        fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except BlockingIOError:
        return False


def _unlock(stream) -> None:
    stream.seek(0)
    if os.name == "nt":
        import msvcrt
        msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
    else:
        import fcntl
        fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def _inside(root: str | os.PathLike, path: str | os.PathLike) -> bool:
    root_key = canonical_payload_path(root)
    path_key = canonical_payload_path(path)
    try:
        return os.path.commonpath([root_key, path_key]) == root_key
    except ValueError:
        return False


@contextmanager
def payload_lock(
        payload_path: str | os.PathLike, *,
        lock_root: str | os.PathLike | None = None,
        source_root: str | os.PathLike | None = None,
        timeout_seconds: float = 10.0,
        poll_seconds: float = 0.01):
    """Take the inter-process lock for exactly one canonical payload path."""
    if timeout_seconds < 0.0 or poll_seconds <= 0.0:
        raise ValueError("lock timeout must be >= 0 and poll interval > 0")
    root = Path(lock_root) if lock_root is not None else default_lock_root()
    root = root.resolve(strict=False)
    if source_root is not None and _inside(source_root, root):
        raise PayloadPublishV2Error(
            "MH_E_SOURCE_INDEX_INVALID",
            "payload lock root must be outside source_root")
    path = lock_path_for(payload_path, root)
    path.parent.mkdir(parents=True, exist_ok=True)
    stream = path.open("a+b")
    acquired = False
    try:
        if path.stat().st_size == 0:
            stream.write(b"\0")
            stream.flush()
        deadline = time.monotonic() + timeout_seconds
        while not _try_lock(stream):
            if time.monotonic() >= deadline:
                raise PayloadPublishV2Error(
                    "MH_E_PAYLOAD_LOCK_TIMEOUT",
                    f"timed out waiting for payload lock: {path}")
            time.sleep(poll_seconds)
        acquired = True
        yield path
    finally:
        if acquired:
            _unlock(stream)
        stream.close()


def _fsync_parent_directory(directory: Path) -> bool:
    # Windows has no portable directory-fsync equivalent.  os.replace still
    # guarantees that readers observe the complete old or complete new file.
    if os.name == "nt":
        return False
    descriptor = os.open(directory, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    return True


def _elapsed_ms(started: float) -> float:
    return round((time.monotonic() - started) * 1000.0, 3)


def atomic_publish_bytes(
        target: str | os.PathLike, payload: bytes, *,
        lock_root: str | os.PathLike | None = None,
        source_root: str | os.PathLike | None = None,
        timeout_seconds: float = 10.0,
        read_back_validator=None,
        pre_replace_guard=None,
        replace_observer=None,
        fsync_parent: bool = True,
        _crash_at: str | None = None,
        _hold_lock_seconds: float = 0.0) -> dict:
    """Publish complete bytes via sibling temp, fsync and atomic replace.

    The private ``_crash_at``/``_hold_lock_seconds`` arguments are deterministic
    process-level verification seams.  Product callers must leave them unset.
    A hard exit before replace may leave a non-authoritative ``.mh-tmp-`` file.
    """
    if not isinstance(payload, bytes):
        raise TypeError("payload must be bytes")
    if read_back_validator is not None and not callable(read_back_validator):
        raise TypeError("read_back_validator must be callable")
    if pre_replace_guard is not None and not callable(pre_replace_guard):
        raise TypeError("pre_replace_guard must be callable")
    if replace_observer is not None and not callable(replace_observer):
        raise TypeError("replace_observer must be callable")
    if type(fsync_parent) is not bool:
        raise TypeError("fsync_parent must be a bool")
    if _crash_at not in {None, "before_replace", "after_replace"}:
        raise ValueError("_crash_at must be before_replace or after_replace")
    if _hold_lock_seconds < 0.0:
        raise ValueError("_hold_lock_seconds must be >= 0")

    destination = Path(target).resolve(strict=False)
    if source_root is not None and not _inside(source_root, destination):
        raise PayloadPublishV2Error(
            "MH_E_INVALID_RESOURCE_SOURCE",
            f"payload target resolves outside source_root: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temp_name = tempfile.mkstemp(
        prefix=f".{destination.name}.mh-tmp-", dir=destination.parent)
    os.close(descriptor)
    temp = Path(temp_name)
    effective_lock_root = (
        Path(lock_root).resolve(strict=False) if lock_root is not None
        else default_lock_root().resolve(strict=False))
    if source_root is not None and _inside(source_root, effective_lock_root):
        raise PayloadPublishV2Error(
            "MH_E_SOURCE_INDEX_INVALID",
            "payload lock root must be outside source_root")

    replaced = False
    parent_fsynced = False
    guard_details = None
    timings_ms = {
        "lock": 0.0,
        "write_fsync": 0.0,
        "read_back": 0.0,
        "guard": 0.0,
        "replace": 0.0,
        "parent_fsync": 0.0,
    }
    started = time.monotonic()
    lock_started = time.monotonic()
    with payload_lock(
            destination, lock_root=effective_lock_root,
            source_root=source_root, timeout_seconds=timeout_seconds) as lock:
        timings_ms["lock"] = _elapsed_ms(lock_started)
        if _hold_lock_seconds:
            time.sleep(_hold_lock_seconds)
        try:
            stage_started = time.monotonic()
            with temp.open("wb") as stream:
                stream.write(payload)
                stream.flush()
                os.fsync(stream.fileno())
            timings_ms["write_fsync"] = _elapsed_ms(stage_started)
            stage_started = time.monotonic()
            read_back = temp.read_bytes()
            if read_back != payload:
                raise OSError("staged payload read-back differs from written bytes")
            if read_back_validator is not None:
                read_back_validator(read_back)
            timings_ms["read_back"] = _elapsed_ms(stage_started)
            if _crash_at == "before_replace":
                os._exit(91)
            if pre_replace_guard is not None:
                stage_started = time.monotonic()
                guard_details = pre_replace_guard()
                timings_ms["guard"] = _elapsed_ms(stage_started)
            stage_started = time.monotonic()
            os.replace(temp, destination)
            replaced = True
            if replace_observer is not None:
                replace_observer()
            timings_ms["replace"] = _elapsed_ms(stage_started)
            if fsync_parent:
                stage_started = time.monotonic()
                parent_fsynced = _fsync_parent_directory(destination.parent)
                timings_ms["parent_fsync"] = _elapsed_ms(stage_started)
            if _crash_at == "after_replace":
                os._exit(92)
        finally:
            if not replaced and temp.exists():
                temp.unlink()

    return {
        "target": canonical_payload_path(destination),
        "bytes": len(payload),
        "lock_path": str(lock),
        "parent_directory_fsynced": parent_fsynced,
        "elapsed_ms": _elapsed_ms(started),
        "timings_ms": timings_ms,
        "guard": guard_details,
    }


def atomic_publish_json(
        target: str | os.PathLike, document: dict, **kwargs) -> dict:
    """Publish deterministic, human-readable UTF-8 JSON."""
    if not isinstance(document, dict):
        raise TypeError("document must be a dict")
    payload = (json.dumps(
        document, ensure_ascii=False, allow_nan=False, indent=2,
        sort_keys=True) + "\n").encode("utf-8")
    return atomic_publish_bytes(target, payload, **kwargs)
