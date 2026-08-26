from __future__ import annotations

import json
import multiprocessing
from pathlib import Path
import sys
import time

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.payload_publish_v2 import (  # noqa: E402
    PayloadPublishV2Error,
    atomic_publish_bytes,
    atomic_publish_json,
    canonical_payload_path,
    default_lock_root,
    lock_path_for,
    payload_lock,
)


def _publish_worker(target, lock_root, payload, crash_at=None, hold=0.0):
    atomic_publish_bytes(
        target, payload, lock_root=lock_root,
        _crash_at=crash_at, _hold_lock_seconds=hold)


def _contending_worker(target, lock_root, payload, ready, start):
    ready.put("ready")
    if not start.wait(10):
        raise TimeoutError("test writer barrier was not released")
    atomic_publish_bytes(
        target, payload, lock_root=lock_root, _hold_lock_seconds=0.2)


def _spawn(target, lock_root, payload, crash_at=None, hold=0.0):
    context = multiprocessing.get_context("spawn")
    process = context.Process(
        target=_publish_worker,
        args=(str(target), str(lock_root), payload, crash_at, hold))
    process.start()
    process.join(15)
    if process.is_alive():
        process.kill()
        process.join()
        raise AssertionError("publication worker hung")
    return process


def test_default_lock_root_and_lock_key_are_host_local_and_deterministic(
        tmp_path):
    root = default_lock_root(
        platform_name="win32", local_appdata=tmp_path / "LocalAppData")
    target = tmp_path / "source" / "asset.fbx"

    assert root == (
        tmp_path / "LocalAppData" / "MimirHead" / "MHBridge" /
        "payload-locks")
    assert lock_path_for(target, root) == lock_path_for(
        target.parent / "." / target.name, root)
    assert lock_path_for(target, root).suffix == ".lock"


def test_atomic_publish_uses_external_lock_sibling_temp_and_complete_bytes(
        tmp_path):
    source = tmp_path / "source"
    target = source / "asset.fbx"
    locks = tmp_path / "cache" / "locks"

    receipt = atomic_publish_bytes(
        target, b"complete payload", lock_root=locks, source_root=source)

    assert target.read_bytes() == b"complete payload"
    assert Path(receipt["lock_path"]).parent == locks.resolve()
    assert receipt["target"] == canonical_payload_path(target)
    assert list(source.glob(".*.mh-tmp-*")) == []
    with pytest.raises(PayloadPublishV2Error) as caught:
        atomic_publish_bytes(
            target, b"blocked", lock_root=source / ".locks",
            source_root=source)
    assert caught.value.code == "MH_E_SOURCE_INDEX_INVALID"


def test_read_back_validator_runs_before_replace_and_preserves_old_on_reject(
        tmp_path):
    target = tmp_path / "source" / "wall.material"
    target.parent.mkdir()
    target.write_bytes(b"old authority")
    observed = []

    def reject(payload):
        observed.append(payload)
        raise ValueError("synthetic read-back rejection")

    with pytest.raises(ValueError, match="read-back rejection"):
        atomic_publish_bytes(
            target, b"new complete payload",
            lock_root=tmp_path / "locks",
            read_back_validator=reject)

    assert observed == [b"new complete payload"]
    assert target.read_bytes() == b"old authority"
    assert list(target.parent.glob(".*.mh-tmp-*")) == []


def test_atomic_json_is_deterministic_utf8_and_rejects_nan(tmp_path):
    target = tmp_path / "cache" / "index.json"
    locks = tmp_path / "locks"
    document = {"z": "Гараж", "a": {"value": 1}}

    first = atomic_publish_json(target, document, lock_root=locks)
    payload = target.read_bytes()
    second = atomic_publish_json(target, document, lock_root=locks)

    assert json.loads(payload.decode("utf-8")) == document
    assert payload.startswith(b'{\n  "a"')
    assert payload.endswith(b"\n")
    assert first["bytes"] == second["bytes"] == len(payload)
    with pytest.raises(ValueError):
        atomic_publish_json(
            target, {"invalid": float("nan")}, lock_root=locks)


def test_real_process_crash_before_replace_preserves_old_authority(tmp_path):
    target = tmp_path / "source" / "asset.fbx"
    target.parent.mkdir()
    target.write_bytes(b"old")
    locks = tmp_path / "locks"

    process = _spawn(target, locks, b"new", crash_at="before_replace")

    assert process.exitcode == 91
    assert target.read_bytes() == b"old"
    temps = list(target.parent.glob(".*.mh-tmp-*"))
    assert temps and all(path.read_bytes() == b"new" for path in temps)
    atomic_publish_bytes(target, b"recovered", lock_root=locks)
    assert target.read_bytes() == b"recovered"


def test_real_process_crash_after_replace_leaves_complete_new_authority(
        tmp_path):
    target = tmp_path / "source" / "asset.fbx"
    target.parent.mkdir()
    target.write_bytes(b"old")

    process = _spawn(
        target, tmp_path / "locks", b"new complete",
        crash_at="after_replace")

    assert process.exitcode == 92
    assert target.read_bytes() == b"new complete"


def test_two_real_process_writers_serialize_without_torn_bytes(tmp_path):
    target = tmp_path / "source" / "asset.fbx"
    locks = tmp_path / "locks"
    first = b"A" * (1024 * 1024)
    second = b"B" * (2 * 1024 * 1024)
    context = multiprocessing.get_context("spawn")
    ready = context.Queue()
    start = context.Event()
    processes = [
        context.Process(
            target=_contending_worker,
            args=(str(target), str(locks), payload, ready, start))
        for payload in (first, second)
    ]
    for process in processes:
        process.start()
    assert {ready.get(timeout=10), ready.get(timeout=10)} == {"ready"}
    started = time.monotonic()
    start.set()
    for process in processes:
        process.join(20)

    assert [process.exitcode for process in processes] == [0, 0]
    assert time.monotonic() - started >= 0.35
    assert target.read_bytes() in {first, second}
    assert list(target.parent.glob(".*.mh-tmp-*")) == []


def test_lock_timeout_does_not_mutate_destination(tmp_path):
    target = tmp_path / "source" / "asset.fbx"
    target.parent.mkdir()
    target.write_bytes(b"stable")
    locks = tmp_path / "locks"

    with payload_lock(target, lock_root=locks):
        with pytest.raises(PayloadPublishV2Error) as caught:
            atomic_publish_bytes(
                target, b"blocked", lock_root=locks,
                timeout_seconds=0.02)
    assert caught.value.code == "MH_E_PAYLOAD_LOCK_TIMEOUT"
    assert target.read_bytes() == b"stable"


def test_pre_replace_guard_runs_under_lock_and_failure_preserves_target(
        tmp_path):
    target = tmp_path / "source" / "asset.fbx"
    target.parent.mkdir()
    target.write_bytes(b"stable")
    locks = tmp_path / "locks"
    observed = []

    def guarded_failure():
        with pytest.raises(PayloadPublishV2Error) as caught:
            with payload_lock(
                    target, lock_root=locks, timeout_seconds=0.02):
                pass
        observed.append(caught.value.code)
        raise RuntimeError("resource collision discovered under lock")

    with pytest.raises(
            RuntimeError, match="resource collision discovered under lock"):
        atomic_publish_bytes(
            target, b"must not publish", lock_root=locks,
            pre_replace_guard=guarded_failure)

    assert observed == ["MH_E_PAYLOAD_LOCK_TIMEOUT"]
    assert target.read_bytes() == b"stable"
    assert list(target.parent.glob(".*.mh-tmp-*")) == []


def test_publication_rejects_target_outside_supplied_source_root(tmp_path):
    source = tmp_path / "source"
    source.mkdir()
    outside = tmp_path / "outside.material"
    with pytest.raises(PayloadPublishV2Error) as caught:
        atomic_publish_bytes(
            outside,
            b"payload",
            source_root=source,
            lock_root=tmp_path / "locks",
        )
    assert caught.value.code == "MH_E_INVALID_RESOURCE_SOURCE"
    assert not outside.exists()
