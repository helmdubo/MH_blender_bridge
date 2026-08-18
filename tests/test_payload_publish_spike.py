import json
import multiprocessing
from pathlib import Path
import time
import uuid

import pytest

from tools.spikes.passport_index import (
    PayloadObservation,
    canonical_path,
    rebuild_index,
    serialize_index,
)
from tools.spikes.payload_publish import atomic_publish_bytes, lock_path_for


def _passport(resource_uid):
    return json.dumps(
        {
            "schema": "mh.fbx_passport",
            "schema_version": 1,
            "resource_uid": str(resource_uid),
            "kind": "static_mesh",
            "name": "crash_probe",
            "lod_levels": [0, 1],
            "lod_policy": "authored",
            "geometry_hash": "xxh3:0123456789abcdef",
            "material_slots": [],
            "properties": {},
            "exporter": "G4 test",
        },
        sort_keys=True,
        separators=(",", ":"),
    )


def _publish_worker(target, lock_root, payload, crash_at=None, hold=0.0):
    atomic_publish_bytes(
        target,
        payload,
        lock_root=lock_root,
        crash_at=crash_at,
        hold_lock_seconds=hold,
    )


def _contending_worker(target, lock_root, payload, ready_queue, start_event):
    ready_queue.put("ready")
    if not start_event.wait(10):
        raise TimeoutError("parent did not release G4 writer barrier")
    atomic_publish_bytes(
        target, payload, lock_root=lock_root, hold_lock_seconds=0.2
    )


def _spawn(target, lock_root, payload, crash_at=None, hold=0.0):
    context = multiprocessing.get_context("spawn")
    process = context.Process(
        target=_publish_worker,
        args=(str(target), str(lock_root), payload, crash_at, hold),
    )
    process.start()
    process.join(15)
    if process.is_alive():
        process.kill()
        process.join()
        raise AssertionError("G4 worker hung")
    return process


def test_atomic_publish_uses_sibling_temp_and_external_per_path_lock(tmp_path):
    source = tmp_path / "source"
    target = source / "asset.fbx"
    lock_root = tmp_path / "local-state" / "locks"

    receipt = atomic_publish_bytes(
        target,
        b"new-payload",
        lock_root=lock_root,
        source_root=source,
    )

    assert target.read_bytes() == b"new-payload"
    assert receipt["lock_outside_source_tree"] is True
    assert Path(receipt["lock_path"]).parent == lock_root.resolve()
    assert list(source.glob(".*.mh-tmp-*")) == []
    assert lock_path_for(target, lock_root) == Path(receipt["lock_path"])
    with pytest.raises(ValueError, match="outside the source tree"):
        atomic_publish_bytes(
            target,
            b"must-not-write",
            lock_root=source / ".mh-locks",
            source_root=source,
        )


def test_real_process_crash_before_replace_preserves_old_payload(tmp_path):
    target = tmp_path / "source" / "asset.fbx"
    target.parent.mkdir()
    target.write_bytes(b"old")
    process = _spawn(
        target, tmp_path / "locks", b"new", crash_at="before_replace"
    )

    assert process.exitcode == 91
    assert target.read_bytes() == b"old"
    # A real hard crash may leave a harmless sibling temp; it is never authority.
    assert all(path.read_bytes() == b"new" for path in target.parent.glob(".*.mh-tmp-*"))
    atomic_publish_bytes(target, b"recovered", lock_root=tmp_path / "locks")
    assert target.read_bytes() == b"recovered"


def test_real_process_crash_after_replace_leaves_complete_new_payload(tmp_path):
    target = tmp_path / "source" / "asset.fbx"
    target.parent.mkdir()
    target.write_bytes(b"old")
    process = _spawn(
        target, tmp_path / "locks", b"new-complete", crash_at="after_replace"
    )

    assert process.exitcode == 92
    assert target.read_bytes() == b"new-complete"


def test_two_real_process_writers_never_create_torn_payload(tmp_path):
    target = tmp_path / "source" / "asset.fbx"
    lock_root = tmp_path / "locks"
    first = b"A" * (2 * 1024 * 1024)
    second = b"B" * (3 * 1024 * 1024)
    context = multiprocessing.get_context("spawn")
    ready_queue = context.Queue()
    start_event = context.Event()
    processes = [
        context.Process(
            target=_contending_worker,
            args=(str(target), str(lock_root), first, ready_queue, start_event),
        ),
        context.Process(
            target=_contending_worker,
            args=(str(target), str(lock_root), second, ready_queue, start_event),
        ),
    ]
    for process in processes:
        process.start()
    assert [ready_queue.get(timeout=10), ready_queue.get(timeout=10)] == [
        "ready", "ready"
    ]
    started = time.monotonic()
    start_event.set()
    for process in processes:
        process.join(20)

    assert [process.exitcode for process in processes] == [0, 0]
    assert time.monotonic() - started >= 0.35
    assert target.read_bytes() in {first, second}
    assert list(target.parent.glob(".*.mh-tmp-*")) == []


def test_stale_and_deleted_index_rebuild_from_payload_truth(tmp_path):
    uid = uuid.uuid4()
    passport = _passport(uid)
    target = tmp_path / "source" / "asset.fbx"
    lock_root = tmp_path / "locks"
    atomic_publish_bytes(target, b"revision-one", lock_root=lock_root)
    observation = PayloadObservation.from_text(target, passport)
    first = rebuild_index([observation])
    index_path = tmp_path / "local-state" / "index.json"
    atomic_publish_bytes(index_path, serialize_index(first), lock_root=lock_root)

    time.sleep(0.002)
    atomic_publish_bytes(target, b"revision-two-longer", lock_root=lock_root)
    rebuilt_from_stale = rebuild_index([observation], previous_index=first)
    path_row = rebuilt_from_stale["paths"][canonical_path(target)]
    assert path_row["payload_fingerprint"] != first["paths"][canonical_path(target)][
        "payload_fingerprint"
    ]
    assert "MH_W_PAYLOAD_EXTERNAL_MODIFIED" in path_row["diagnostics"]

    index_path.unlink()
    rebuilt_after_delete = rebuild_index([observation])
    assert rebuilt_after_delete["paths"][canonical_path(target)][
        "payload_fingerprint"
    ] == path_row["payload_fingerprint"]
    assert rebuilt_after_delete["uids"][str(uid)]["resolution_status"] == "adopt_new"
