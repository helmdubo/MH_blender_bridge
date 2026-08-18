"""Pure G4 spike for atomic payload publication and per-path local locks."""

from __future__ import annotations

from contextlib import contextmanager
import hashlib
import os
from pathlib import Path
import sys
import time
import uuid


def canonical_payload_path(path: str | Path) -> str:
    return os.path.normcase(str(Path(path).resolve(strict=False)))


def default_lock_root(
    *, local_appdata: str | Path | None = None, home: str | Path | None = None
) -> Path:
    user_home = Path(home) if home is not None else Path.home()
    if sys.platform.startswith("win"):
        base = Path(
            local_appdata
            if local_appdata is not None
            else os.environ.get("LOCALAPPDATA", user_home / "AppData" / "Local")
        )
    elif sys.platform == "darwin":
        base = user_home / "Library" / "Caches"
    else:
        base = Path(os.environ.get("XDG_CACHE_HOME", user_home / ".cache"))
    return base / "MimirHead" / "MHBridge" / "payload-locks"


def lock_path_for(payload_path: str | Path, lock_root: str | Path) -> Path:
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


@contextmanager
def payload_lock(
    payload_path: str | Path,
    *,
    lock_root: str | Path | None = None,
    timeout_seconds: float = 10.0,
    poll_seconds: float = 0.01,
):
    """Take an OS-released lock for exactly one canonical payload path."""

    root = Path(lock_root) if lock_root is not None else default_lock_root()
    lock_path = lock_path_for(payload_path, root)
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    stream = lock_path.open("a+b")
    try:
        if lock_path.stat().st_size == 0:
            stream.write(b"\0")
            stream.flush()
        deadline = time.monotonic() + timeout_seconds
        while not _try_lock(stream):
            if time.monotonic() >= deadline:
                raise TimeoutError(f"timed out waiting for payload lock: {lock_path}")
            time.sleep(poll_seconds)
        try:
            yield lock_path
        finally:
            _unlock(stream)
    finally:
        stream.close()


def _fsync_parent_directory(path: Path) -> bool:
    """Fsync a directory where supported; Windows replace durability is external."""

    if os.name == "nt":
        return False
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    return True


def atomic_publish_bytes(
    target: str | Path,
    payload: bytes,
    *,
    lock_root: str | Path | None = None,
    source_root: str | Path | None = None,
    timeout_seconds: float = 10.0,
    crash_at: str | None = None,
    hold_lock_seconds: float = 0.0,
) -> dict:
    """Publish bytes using sibling temp + flush + fsync + ``os.replace``.

    ``crash_at`` and ``hold_lock_seconds`` are explicit spike hooks used only by
    real subprocess tests.  ``os._exit`` models a process disappearing without
    Python cleanup; the OS must release the per-file lock.
    """

    if crash_at not in {None, "before_replace", "after_replace"}:
        raise ValueError("crash_at must be before_replace, after_replace, or None")
    if not isinstance(payload, bytes):
        raise TypeError("payload must be bytes")

    destination = Path(target).resolve(strict=False)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temp = destination.parent / (
        f".{destination.name}.mh-tmp-{os.getpid()}-{uuid.uuid4().hex}"
    )
    replaced = False
    started = time.monotonic()
    effective_lock_root = (
        Path(lock_root).resolve(strict=False)
        if lock_root is not None else default_lock_root().resolve(strict=False)
    )
    if source_root is not None:
        resolved_source_root = Path(source_root).resolve(strict=False)
        try:
            effective_lock_root.relative_to(resolved_source_root)
        except ValueError:
            pass
        else:
            raise ValueError("payload lock root must be outside the source tree")
    with payload_lock(
        destination, lock_root=effective_lock_root, timeout_seconds=timeout_seconds
    ) as acquired_lock:
        if hold_lock_seconds:
            time.sleep(hold_lock_seconds)
        try:
            with temp.open("xb") as stream:
                stream.write(payload)
                stream.flush()
                os.fsync(stream.fileno())
            if crash_at == "before_replace":
                os._exit(91)
            os.replace(temp, destination)
            replaced = True
            parent_fsynced = _fsync_parent_directory(destination.parent)
            if crash_at == "after_replace":
                os._exit(92)
        finally:
            if not replaced and temp.exists():
                temp.unlink()

    return {
        "target": canonical_payload_path(destination),
        "bytes": len(payload),
        "lock_path": str(acquired_lock),
        "lock_outside_source_tree": (
            None
            if source_root is None
            else not Path(acquired_lock).resolve(strict=False).is_relative_to(
                    Path(source_root).resolve(strict=False)
                )
        ),
        "parent_directory_fsynced": parent_fsynced,
        "elapsed_ms": round((time.monotonic() - started) * 1000.0, 3),
    }
