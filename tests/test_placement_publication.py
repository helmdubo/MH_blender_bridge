from pathlib import Path
import sys
import tempfile

import pytest


REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.model import PlacementProfile, PlacementRange  # noqa: E402
from mh4blend.core.placement_publication import (  # noqa: E402
    PlacementPublicationRequest,
    plan_placement_publications,
    publish_placement_publications,
    revalidate_placement_publications,
    stage_placement_publications,
)
from mh4blend.core.placements import placement_json_bytes  # noqa: E402


def _payload(value: float) -> bytes:
    return placement_json_bytes(PlacementProfile(
        name="",
        uniform_scale=PlacementRange(value, 0.0),
    ))


def _request(name: str, value: float, provenance: Path):
    return PlacementPublicationRequest(name, _payload(value), provenance)


def test_plan_coalesces_identical_requests_and_rejects_divergent_identity(
        tmp_path):
    output = tmp_path / "out"
    output.mkdir()
    first = _request("scatter", 1.0, tmp_path / "first.blk")
    identical = _request("scatter", 1.0, tmp_path / "second.blk")

    plans = plan_placement_publications(
        [first, identical], source_root=tmp_path, output_dir=output)
    assert len(plans) == 1
    assert plans[0].request.provenance == first.provenance
    assert plans[0].should_write is True

    with pytest.raises(
            ValueError, match="MH_E_AMBIGUOUS_RESOURCE_NAME") as caught:
        plan_placement_publications(
            [first, _request("scatter", 2.0, tmp_path / "different.blk")],
            source_root=tmp_path, output_dir=output)
    assert str(first.provenance) in caught.value.subjects
    assert str(tmp_path / "different.blk") in caught.value.subjects


def test_plan_reuses_only_exact_canonical_global_source(tmp_path):
    output = tmp_path / "out"
    elsewhere = tmp_path / "elsewhere"
    output.mkdir()
    elsewhere.mkdir()
    request = _request("scatter", 1.0, tmp_path / "source.blk")
    existing = elsewhere / "scatter.placement"
    existing.write_bytes(request.canonical_bytes)

    plans = plan_placement_publications(
        [request], source_root=tmp_path, output_dir=output)
    assert plans[0].target == existing
    assert plans[0].should_write is False

    existing.write_bytes(b" " + request.canonical_bytes)
    with pytest.raises(
            ValueError, match="MH_E_PLACEMENT_PROFILE_GRAMMAR"):
        plan_placement_publications(
            [request], source_root=tmp_path, output_dir=output)


def test_stage_all_then_publish_and_reuse_without_rewrite(tmp_path):
    output = tmp_path / "out"
    output.mkdir()
    requests = [
        _request("first", 1.0, tmp_path / "first.blk"),
        _request("second", 2.0, tmp_path / "second.blk"),
    ]
    plans = plan_placement_publications(
        requests, source_root=tmp_path, output_dir=output)
    with tempfile.TemporaryDirectory(prefix="mh-placement-test-") as staging:
        staged = stage_placement_publications(
            plans, staging_dir=staging, source_root=tmp_path)
        assert [row.staged_path.name for row in staged] == [
            "first.placement", "second.placement"]
        assert not list(output.iterdir())
        results = publish_placement_publications(
            staged, source_root=tmp_path)
    assert [row.name for row in results] == ["first", "second"]
    assert all(row.written and not row.reused for row in results)
    assert (output / "first.placement").read_bytes() == requests[
        0].canonical_bytes
    assert (output / "second.placement").read_bytes() == requests[
        1].canonical_bytes

    mtimes = {
        path.name: path.stat().st_mtime_ns for path in output.iterdir()
    }
    reuse_plans = plan_placement_publications(
        requests, source_root=tmp_path, output_dir=output)
    with tempfile.TemporaryDirectory(prefix="mh-placement-test-") as staging:
        reuse_staged = stage_placement_publications(
            reuse_plans, staging_dir=staging, source_root=tmp_path)
        assert all(row.staged_path is None for row in reuse_staged)
        reused = publish_placement_publications(
            reuse_staged, source_root=tmp_path)
    assert all(not row.written and row.reused for row in reused)
    assert mtimes == {
        path.name: path.stat().st_mtime_ns for path in output.iterdir()
    }


def test_whole_set_revalidation_happens_before_first_replace(tmp_path):
    output = tmp_path / "out"
    duplicate = tmp_path / "raced"
    output.mkdir()
    duplicate.mkdir()
    requests = [
        _request("first", 1.0, tmp_path / "first.blk"),
        _request("second", 2.0, tmp_path / "second.blk"),
    ]
    plans = plan_placement_publications(
        requests, source_root=tmp_path, output_dir=output)
    with tempfile.TemporaryDirectory(prefix="mh-placement-test-") as staging:
        staged = stage_placement_publications(
            plans, staging_dir=staging, source_root=tmp_path)
        (duplicate / "second.placement").write_bytes(
            requests[1].canonical_bytes)

        with pytest.raises(
                ValueError, match="MH_E_AMBIGUOUS_RESOURCE_NAME"):
            publish_placement_publications(staged, source_root=tmp_path)
    assert not (output / "first.placement").exists()
    assert not (output / "second.placement").exists()


def test_staging_inside_source_root_is_rejected_before_write(tmp_path):
    output = tmp_path / "out"
    staging = tmp_path / "stage"
    output.mkdir()
    staging.mkdir()
    plans = plan_placement_publications(
        [_request("scatter", 1.0, tmp_path / "source.blk")],
        source_root=tmp_path, output_dir=output)

    with pytest.raises(ValueError, match="outside Project Source Root"):
        stage_placement_publications(
            plans, staging_dir=staging, source_root=tmp_path)
    assert not list(staging.iterdir())


def test_revalidation_rejects_reused_source_change(tmp_path):
    output = tmp_path / "out"
    output.mkdir()
    request = _request("scatter", 1.0, tmp_path / "source.blk")
    target = output / "scatter.placement"
    target.write_bytes(request.canonical_bytes)
    plans = plan_placement_publications(
        [request], source_root=tmp_path, output_dir=output)
    target.write_bytes(_payload(2.0))

    with pytest.raises(
            ValueError, match="MH_E_AMBIGUOUS_RESOURCE_NAME"):
        revalidate_placement_publications(plans, source_root=tmp_path)
