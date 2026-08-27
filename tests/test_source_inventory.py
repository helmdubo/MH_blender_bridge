"""Pure filesystem tests for the V5 physical source inventory."""

from pathlib import Path
import sys

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.source_closure import ResourceKey  # noqa: E402
from mh4blend.core.source_inventory import scan_source_inventory  # noqa: E402
from mh4blend.core.validate import MHValidationError  # noqa: E402


def test_inventory_classifies_all_resource_kinds_and_keeps_kinds_separate(
        tmp_path):
    payloads = (
        "shared.composite",
        "shared.placement",
        "shared.material",
        "shared.mesh.fbx",
        "shared.tga",
    )
    for filename in payloads:
        (tmp_path / filename).write_bytes(filename.encode("ascii"))

    inventory = scan_source_inventory(tmp_path)

    assert inventory.resolve(ResourceKey("composite", "shared")).path.name == (
        "shared.composite")
    assert inventory.resolve(
        ResourceKey("placement_profile", "shared")).path.name == (
            "shared.placement")
    assert inventory.resolve(ResourceKey("material", "shared")).path.name == (
        "shared.material")
    assert inventory.resolve(
        ResourceKey("static_mesh", "shared")).path.name == "shared.mesh.fbx"
    assert inventory.resolve(ResourceKey("texture", "shared")).path.name == (
        "shared.tga")


def test_inventory_reports_ambiguous_physical_candidates(tmp_path):
    left = tmp_path / "left"
    right = tmp_path / "right"
    left.mkdir()
    right.mkdir()
    (left / "same.composite").write_bytes(b"left")
    (right / "same.composite").write_bytes(b"right")
    inventory = scan_source_inventory(tmp_path)

    with pytest.raises(MHValidationError) as caught:
        inventory.resolve(ResourceKey("composite", "same"))
    assert caught.value.code == "MH_E_AMBIGUOUS_RESOURCE_NAME"
    assert str(left / "same.composite") in caught.value.subjects
    assert str(right / "same.composite") in caught.value.subjects


@pytest.mark.parametrize("filename,key", [
    ("Bad.composite", ResourceKey("composite", "bad")),
    ("good.Composite", ResourceKey("composite", "good")),
    ("good.MESH.FBX", ResourceKey("static_mesh", "good")),
    ("good.PNG", ResourceKey("texture", "good")),
])
def test_inventory_keeps_noncanonical_candidate_out_of_resolution(
        tmp_path, filename, key):
    (tmp_path / filename).write_bytes(b"x")
    (tmp_path / "unrelated.composite").write_bytes(b"good")
    inventory = scan_source_inventory(tmp_path)

    assert inventory.resolve(
        ResourceKey("composite", "unrelated")).path.name == (
            "unrelated.composite")
    with pytest.raises(MHValidationError) as caught:
        inventory.resolve(key)
    assert caught.value.code == "MH_E_NONCANONICAL_RESOURCE_NAME"
    assert str(tmp_path / filename) in caught.value.subjects


def test_inventory_missing_can_be_explicitly_admitted(tmp_path):
    inventory = scan_source_inventory(tmp_path)
    key = ResourceKey("material", "missing")
    assert inventory.resolve(key, allow_missing=True) is None
    with pytest.raises(MHValidationError) as caught:
        inventory.resolve(key)
    assert caught.value.code == "MH_E_RESOURCE_NOT_FOUND"


def test_physical_alias_of_one_file_is_not_a_duplicate(tmp_path):
    target = tmp_path / "same.composite"
    target.write_bytes(b"payload")
    alias = tmp_path / "alias.composite"
    try:
        alias.symlink_to(target)
    except OSError:
        pytest.skip("host does not permit file symlinks")

    inventory = scan_source_inventory(tmp_path)
    rows = inventory.candidates_for(ResourceKey("composite", "same"))
    assert len(rows) == 1
    assert rows[0].path == target.resolve()
    assert inventory.candidates_for(ResourceKey("composite", "alias")) == ()


def test_alias_outside_root_is_rejected_before_identity(tmp_path):
    root = tmp_path / "root"
    root.mkdir()
    outside = tmp_path / "outside.composite"
    outside.write_bytes(b"payload")
    alias = root / "inside.composite"
    try:
        alias.symlink_to(outside)
    except OSError:
        pytest.skip("host does not permit file symlinks")

    with pytest.raises(MHValidationError) as caught:
        scan_source_inventory(root)
    assert caught.value.code == "MH_E_INVALID_RESOURCE_SOURCE"
    assert str(alias) in caught.value.subjects
    assert str(outside) in caught.value.subjects
