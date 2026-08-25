"""Filesystem gates for the Misc Dagor texture project workflow."""

from pathlib import Path
import sys

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

import mh4blend.core.project_textures as project_textures_module
from mh4blend.core.project_textures import (
    ProjectTextureError,
    atomic_copy_texture_plans,
    plan_project_texture,
    validate_texture_plans,
)


def _external_texture(tmp_path, branch="external", name="car_glass_c_tex_d.tif"):
    path = (
        tmp_path / branch / "develop" / "assets" / "gameproj"
        / "manmade_common" / "textures" / "tile_textures" / name)
    path.parent.mkdir(parents=True)
    path.write_bytes((branch + ":" + name).encode("utf-8"))
    return path


def test_plan_recreates_tree_starting_at_assets(tmp_path):
    source = _external_texture(tmp_path)
    project = tmp_path / "project"
    project.mkdir()

    plan = plan_project_texture(source, project)

    assert plan.source == source
    assert plan.destination == (
        project / "assets" / "gameproj" / "manmade_common" / "textures"
        / "tile_textures" / "car_glass_c_tex_d.tif")


def test_plan_accepts_mixed_windows_path_separators(tmp_path):
    source = _external_texture(tmp_path)
    project = tmp_path / "project"
    project.mkdir()
    mixed = str(source.parent).replace("\\", "/") + "\\" + source.name

    plan = plan_project_texture(mixed, project)

    assert plan.source == source
    assert plan.destination.name == "car_glass_c_tex_d.tif"


def test_plan_accepts_windows_separators_for_source_and_project_root(tmp_path):
    source = _external_texture(tmp_path)
    project = tmp_path / "project"
    project.mkdir()

    plan = plan_project_texture(
        str(source).replace("/", "\\"),
        str(project).replace("/", "\\"),
    )

    assert plan.source == source
    assert plan.destination == (
        project / "assets" / "gameproj" / "manmade_common" / "textures"
        / "tile_textures" / "car_glass_c_tex_d.tif")


def test_atomic_copy_replaces_complete_file_and_cleans_temps(tmp_path):
    source = _external_texture(tmp_path)
    project = tmp_path / "project"
    project.mkdir()
    plan = plan_project_texture(source, project)
    plan.destination.parent.mkdir(parents=True)
    plan.destination.write_bytes(b"old project bytes")

    report = atomic_copy_texture_plans([plan])

    assert report["copied"] == 1 and report["skipped"] == 0
    assert plan.destination.read_bytes() == source.read_bytes()
    assert not list(plan.destination.parent.glob(".*.mh-tmp-*"))


def test_copy_deduplicates_one_source_referenced_many_times(tmp_path):
    source = _external_texture(tmp_path)
    project = tmp_path / "project"
    project.mkdir()
    plan = plan_project_texture(source, project)

    report = atomic_copy_texture_plans([plan, plan, plan])

    assert report["copied"] == 1
    assert plan.destination.is_file()


def test_different_sources_cannot_claim_same_project_path(tmp_path):
    first = _external_texture(tmp_path, "first")
    second = _external_texture(tmp_path, "second")
    project = tmp_path / "project"
    project.mkdir()

    plans = [
        plan_project_texture(first, project),
        plan_project_texture(second, project),
    ]
    with pytest.raises(ProjectTextureError) as excinfo:
        validate_texture_plans(plans, require_sources=True)
    assert excinfo.value.code == "MH_E_AMBIGUOUS_RESOURCE_NAME"


def test_multi_file_commit_failure_restores_every_previous_destination(
        tmp_path, monkeypatch):
    first = _external_texture(tmp_path, "first", "first_d.tif")
    second = _external_texture(tmp_path, "second", "second_d.tif")
    project = tmp_path / "project"
    project.mkdir()
    plans = [
        plan_project_texture(first, project),
        plan_project_texture(second, project),
    ]
    for index, plan in enumerate(plans):
        plan.destination.parent.mkdir(parents=True, exist_ok=True)
        plan.destination.write_bytes(f"old-{index}".encode("utf-8"))

    real_replace = project_textures_module.os.replace
    texture_commits = 0

    def fail_second_texture_commit(source, destination):
        nonlocal texture_commits
        if ".mh-tmp-" in Path(source).name:
            texture_commits += 1
            if texture_commits == 2:
                raise OSError("injected second commit failure")
        return real_replace(source, destination)

    monkeypatch.setattr(
        project_textures_module.os, "replace", fail_second_texture_commit)
    with pytest.raises(OSError, match="injected second commit failure"):
        atomic_copy_texture_plans(plans)

    assert plans[0].destination.read_bytes() == b"old-0"
    assert plans[1].destination.read_bytes() == b"old-1"
    assert not list(project.rglob(".*.mh-tmp-*"))
    assert not list(project.rglob(".*.mh-backup-*"))


def test_commit_failure_removes_an_earlier_new_destination(tmp_path, monkeypatch):
    first = _external_texture(tmp_path, "first", "first_d.tif")
    second = _external_texture(tmp_path, "second", "second_d.tif")
    project = tmp_path / "project"
    project.mkdir()
    plans = [
        plan_project_texture(first, project),
        plan_project_texture(second, project),
    ]
    real_replace = project_textures_module.os.replace
    texture_commits = 0

    def fail_second_texture_commit(source, destination):
        nonlocal texture_commits
        if ".mh-tmp-" in Path(source).name:
            texture_commits += 1
            if texture_commits == 2:
                raise OSError("injected second commit failure")
        return real_replace(source, destination)

    monkeypatch.setattr(
        project_textures_module.os, "replace", fail_second_texture_commit)
    with pytest.raises(OSError, match="injected second commit failure"):
        atomic_copy_texture_plans(plans)

    assert not plans[0].destination.exists()
    assert not plans[1].destination.exists()


def test_destination_change_immediately_before_commit_is_not_overwritten(
        tmp_path, monkeypatch):
    first = _external_texture(tmp_path, "first", "first_d.tif")
    second = _external_texture(tmp_path, "second", "second_d.tif")
    project = tmp_path / "project"
    project.mkdir()
    plans = [
        plan_project_texture(first, project),
        plan_project_texture(second, project),
    ]
    for index, plan in enumerate(plans):
        plan.destination.parent.mkdir(parents=True, exist_ok=True)
        plan.destination.write_bytes(f"old-{index}".encode("utf-8"))

    real_replace = project_textures_module.os.replace
    texture_commits = 0

    def change_second_after_first_commit(source, destination):
        nonlocal texture_commits
        if ".mh-tmp-" in Path(source).name:
            texture_commits += 1
            result = real_replace(source, destination)
            if texture_commits == 1:
                plans[1].destination.write_bytes(b"external-change")
            return result
        return real_replace(source, destination)

    monkeypatch.setattr(
        project_textures_module.os, "replace",
        change_second_after_first_commit)
    with pytest.raises(ProjectTextureError) as excinfo:
        atomic_copy_texture_plans(plans)

    assert excinfo.value.code == "MH_E_INVALID_RESOURCE_SOURCE"
    assert plans[0].destination.read_bytes() == b"old-0"
    assert plans[1].destination.read_bytes() == b"external-change"


def test_failed_rollback_preserves_recoverable_backup(tmp_path, monkeypatch):
    first = _external_texture(tmp_path, "first", "first_d.tif")
    second = _external_texture(tmp_path, "second", "second_d.tif")
    project = tmp_path / "project"
    project.mkdir()
    plans = [
        plan_project_texture(first, project),
        plan_project_texture(second, project),
    ]
    for index, plan in enumerate(plans):
        plan.destination.parent.mkdir(parents=True, exist_ok=True)
        plan.destination.write_bytes(f"old-{index}".encode("utf-8"))

    real_replace = project_textures_module.os.replace
    texture_commits = 0

    def fail_commit_and_restore(source, destination):
        nonlocal texture_commits
        source_name = Path(source).name
        if ".mh-tmp-" in source_name:
            texture_commits += 1
            if texture_commits == 2:
                raise OSError("injected second commit failure")
        if ".mh-restore-" in source_name:
            raise OSError("injected rollback failure")
        return real_replace(source, destination)

    monkeypatch.setattr(
        project_textures_module.os, "replace", fail_commit_and_restore)
    with pytest.raises(RuntimeError, match="backup preserved at"):
        atomic_copy_texture_plans(plans)

    backups = list(plans[0].destination.parent.glob(".*.mh-backup-*"))
    assert len(backups) == 1
    assert backups[0].read_bytes() == b"old-0"
    assert plans[0].destination.read_bytes() == first.read_bytes()
    assert plans[1].destination.read_bytes() == b"old-1"


def test_late_missing_source_preflight_writes_nothing(tmp_path):
    first = _external_texture(tmp_path, "first", "first_d.tif")
    missing = (
        tmp_path / "second" / "develop" / "assets" / "gameproj"
        / "textures" / "missing_d.tif")
    project = tmp_path / "project"
    project.mkdir()
    plans = [
        plan_project_texture(first, project),
        plan_project_texture(missing, project),
    ]

    with pytest.raises(ProjectTextureError) as excinfo:
        atomic_copy_texture_plans(plans)

    assert excinfo.value.code == "MH_E_INVALID_RESOURCE_SOURCE"
    assert all(not plan.destination.exists() for plan in plans)


@pytest.mark.parametrize(
    "relative",
    [
        Path("outside") / "textures" / "wall_d.tif",
        Path("assets") / "nested" / "assets" / "wall_d.tif",
    ],
)
def test_plan_requires_exactly_one_assets_segment(tmp_path, relative):
    source = tmp_path / relative
    source.parent.mkdir(parents=True)
    source.write_bytes(b"texture")
    project = tmp_path / "project"
    project.mkdir()

    with pytest.raises(ProjectTextureError) as excinfo:
        plan_project_texture(source, project)
    assert excinfo.value.code == "MH_E_INVALID_RESOURCE_SOURCE"


@pytest.mark.parametrize("name", ["Wall_D.tif", "wall.d.tif", "wall_d.TIF"])
def test_plan_rejects_files_that_project_index_cannot_canonicalize(
        tmp_path, name):
    source = _external_texture(tmp_path, name=name)
    project = tmp_path / "project"
    project.mkdir()

    with pytest.raises(ProjectTextureError) as excinfo:
        plan_project_texture(source, project)
    assert excinfo.value.code == "MH_E_NONCANONICAL_RESOURCE_NAME"


def test_already_projected_texture_is_a_safe_noop(tmp_path):
    project = tmp_path / "project"
    project.mkdir()
    source = (
        project / "assets" / "gameproj" / "textures" / "wall_d.tif")
    source.parent.mkdir(parents=True)
    source.write_bytes(b"texture")
    plan = plan_project_texture(source, project)

    report = atomic_copy_texture_plans([plan])

    assert report["copied"] == 0 and report["skipped"] == 1
    assert source.read_bytes() == b"texture"
