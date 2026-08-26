import multiprocessing
from pathlib import Path
import sys

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.batch_publish import (  # noqa: E402
    BatchPartialPublishError,
    BatchPublishItem,
    publish_ordered_batch,
)
from mh4blend.core import payload_publish_v2  # noqa: E402


class InjectedFailure(RuntimeError):
    pass


def _items(root: Path):
    identities = ("material:leaf", "static_mesh:body", "composite:root")
    suffixes = (".material", ".mesh.fbx", ".composite")
    rows = []
    for identity, suffix in zip(identities, suffixes):
        target = root / f"{identity.split(':')[1]}{suffix}"
        target.write_bytes(b"old")
        rows.append(BatchPublishItem(identity, target, identity.encode("ascii")))
    return tuple(rows)


@pytest.mark.parametrize("boundary", ("before_replace", "after_replace"))
@pytest.mark.parametrize("failure_index", (0, 1, 2))
def test_failure_at_every_replace_boundary_reports_exact_sets(
        tmp_path, boundary, failure_index):
    source = tmp_path / "source"
    source.mkdir()
    items = _items(source)

    def hook(event, item, published):
        if event == boundary and item == items[failure_index]:
            raise InjectedFailure(f"{event}:{item.identity}:{published}")

    if boundary == "before_replace" and failure_index == 0:
        with pytest.raises(InjectedFailure):
            publish_ordered_batch(
                items,
                source_root=source,
                lock_root=tmp_path / "locks",
                pre_replace_guard=lambda _published: None,
                _boundary_hook=hook,
            )
        expected_published = ()
    else:
        with pytest.raises(BatchPartialPublishError) as caught:
            publish_ordered_batch(
                items,
                source_root=source,
                lock_root=tmp_path / "locks",
                pre_replace_guard=lambda _published: None,
                _boundary_hook=hook,
            )
        expected_count = failure_index + (boundary == "after_replace")
        expected_published = tuple(
            item.identity for item in items[:expected_count])
        assert caught.value.code == "MH_E_PARTIAL_PUBLISH"
        assert caught.value.published == expected_published
        assert caught.value.unpublished == tuple(
            item.identity for item in items[expected_count:])

    for index, item in enumerate(items):
        expected = item.payload if index < len(expected_published) else b"old"
        assert item.target.read_bytes() == expected


def test_directory_fsync_failure_after_replace_counts_resource_as_published(
        tmp_path, monkeypatch):
    source = tmp_path / "source"
    source.mkdir()
    items = _items(source)[:1]

    def fail_fsync(_directory):
        raise OSError("injected directory fsync failure")

    monkeypatch.setattr(payload_publish_v2, "_fsync_parent_directory", fail_fsync)
    with pytest.raises(BatchPartialPublishError) as caught:
        publish_ordered_batch(
            items,
            source_root=source,
            lock_root=tmp_path / "locks",
            pre_replace_guard=lambda _published: None,
        )
    assert caught.value.published == (items[0].identity,)
    assert caught.value.unpublished == ()
    assert items[0].target.read_bytes() == items[0].payload


def _crash_worker(source, lock_root, targets, identities, crash_identity, crash_at):
    items = tuple(BatchPublishItem(
        identity,
        Path(target),
        identity.encode("ascii"),
    ) for target, identity in zip(targets, identities))
    publish_ordered_batch(
        items,
        source_root=source,
        lock_root=lock_root,
        pre_replace_guard=lambda _published: None,
        _crash_identity=crash_identity,
        _crash_at=crash_at,
    )


@pytest.mark.parametrize("boundary", ("before_replace", "after_replace"))
@pytest.mark.parametrize("crash_index", (0, 1, 2))
def test_process_crash_at_every_boundary_leaves_complete_closed_prefix(
        tmp_path, boundary, crash_index):
    source = tmp_path / f"source-{boundary}-{crash_index}"
    source.mkdir()
    items = _items(source)
    context = multiprocessing.get_context("spawn")
    process = context.Process(
        target=_crash_worker,
        args=(
            str(source),
            str(tmp_path / "locks"),
            tuple(str(item.target) for item in items),
            tuple(item.identity for item in items),
            items[crash_index].identity,
            boundary,
        ),
    )
    process.start()
    process.join(20)
    assert process.exitcode == (91 if boundary == "before_replace" else 92)

    published_count = crash_index + (boundary == "after_replace")
    for index, item in enumerate(items):
        expected = item.payload if index < published_count else b"old"
        assert item.target.read_bytes() == expected
