"""Blender-hosted gates for the V5-S4 write-free closure planner."""

import inspect
from dataclasses import replace
import os
from pathlib import Path
import sys

import pytest

bpy = pytest.importorskip("bpy")
REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.composites import (  # noqa: E402
    CompositeValueError,
    composite_json_bytes,
)
from mh4blend.core.model import Composite, Node, PlacementProfile  # noqa: E402
from mh4blend.core.placements import placement_json_bytes  # noqa: E402
from mh4blend.core.source_closure import ResourceKey  # noqa: E402
from mh4blend.core.validate import MHValidationError  # noqa: E402
from mh4blend.scene.export_closure import (  # noqa: E402
    CLOSURE_MODE_COMPOSITES,
    CLOSURE_MODE_INCLUDE_ALL,
    prepare_composite_closure_export,
    revalidate_composite_closure_export,
    stage_composite_closure_export,
)
from mh4blend.scene.export_composite import (  # noqa: E402
    NODE_RESOURCE_KEY,
    UNRESOLVED_PLACEMENT_KEY,
)
from mh4blend.scene.resource_markers import stamp_resource_collection  # noqa: E402
from mh4blend.ui import composite_authoring, ops  # noqa: E402


@pytest.fixture(autouse=True)
def registered_addon_properties():
    owned_material = not hasattr(bpy.types.Material, "mh4blend")
    owned_object = not hasattr(bpy.types.Object, "mh4blend")
    if owned_material:
        ops.register()
    if owned_object:
        composite_authoring.register()
    try:
        yield
    finally:
        if owned_object:
            composite_authoring.unregister()
        if owned_material:
            ops.unregister()


def _collection(name):
    collection = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(collection)
    return collection


def _write_composite(root: Path, resource: Composite):
    path = root / f"{resource.name}.composite"
    path.write_bytes(composite_json_bytes(resource))
    return path


def _unresolved_object(collection, name, kind, resource, *, parent=None):
    obj = bpy.data.objects.new(name, None)
    collection.objects.link(obj)
    obj.parent = parent
    obj.mh4blend.kind = kind
    obj[NODE_RESOURCE_KEY] = resource
    obj[UNRESOLVED_PLACEMENT_KEY] = True
    return obj


def _root_with_two_random_options(source_root: Path):
    root = _collection("closure_root")
    random = bpy.data.objects.new("all_variants", None)
    root.objects.link(random)
    random.mh4blend.kind = "random"
    random.mh4blend.profile = "scatter"
    for index, resource in enumerate(("variant_a", "variant_b")):
        option = _unresolved_object(
            root, f"option_{index}", "composite", resource, parent=random)
        option.mh4blend.option_index = index
        option.mh4blend.weight = 0.0 if index == 1 else 1.0
    _write_composite(source_root, Composite("variant_a", [
        Node("actor", resource="actor_a"),
    ]))
    _write_composite(source_root, Composite("variant_b", [
        Node("actor", resource="actor_b"),
    ]))
    (source_root / "scatter.placement").write_bytes(
        placement_json_bytes(PlacementProfile("scatter")))
    return root


def test_planner_includes_zero_weight_option_profiles_and_root_last(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    source = tmp_path / "source"
    source.mkdir()
    staging = tmp_path / "staging"
    staging.mkdir()
    root = _root_with_two_random_options(source)

    plan = prepare_composite_closure_export(
        root, source, source_root=source,
        mode=CLOSURE_MODE_COMPOSITES)

    assert plan.closure.composites_postorder == (
        ResourceKey("composite", "variant_a"),
        ResourceKey("composite", "variant_b"),
        ResourceKey("composite", "closure_root"),
    )
    assert [row.key for row in plan.payloads] == [
        ResourceKey("placement_profile", "scatter"),
        ResourceKey("composite", "variant_a"),
        ResourceKey("composite", "variant_b"),
        ResourceKey("composite", "closure_root"),
    ]
    assert [row.action for row in plan.payloads] == [
        "reuse", "reuse", "reuse", "publish"]
    assert not (source / "closure_root.composite").exists()

    staged = stage_composite_closure_export(plan, staging_dir=staging)
    assert [row.planned.key for row in staged] == [
        row.key for row in plan.payloads]
    assert all(row.staged_path.read_bytes() == row.payload for row in staged)
    revalidate_composite_closure_export(plan, staged)
    assert not (source / "closure_root.composite").exists()

    # A source race after stage is caught at the final edge.
    (source / "variant_a.composite").write_bytes(
        composite_json_bytes(Composite("variant_a", [])))
    with pytest.raises(MHValidationError, match="changed after preflight"):
        revalidate_composite_closure_export(plan, staged)


def test_cycle_in_zero_weight_option_blocks_without_writes(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root = _root_with_two_random_options(tmp_path)
    _write_composite(tmp_path, Composite("variant_b", [
        Node("composite", resource="closure_root"),
    ]))

    with pytest.raises(CompositeValueError, match="MH_E_COMPOSITE_CYCLE"):
        prepare_composite_closure_export(
            root, tmp_path, source_root=tmp_path,
            mode=CLOSURE_MODE_COMPOSITES)

    assert not (tmp_path / "closure_root.composite").exists()


def test_stage_failure_cleans_every_member_and_never_touches_source(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    source = tmp_path / "source"
    source.mkdir()
    staging = tmp_path / "staging"
    staging.mkdir()
    root = _root_with_two_random_options(source)
    plan = prepare_composite_closure_export(
        root, source, source_root=source,
        mode=CLOSURE_MODE_COMPOSITES)
    broken_rows = list(plan.payloads)
    broken_rows[1] = replace(broken_rows[1], payload=b"{}\n")
    broken = replace(plan, payloads=tuple(broken_rows))

    with pytest.raises(CompositeValueError):
        stage_composite_closure_export(broken, staging_dir=staging)

    assert list(staging.iterdir()) == []
    assert not (source / "closure_root.composite").exists()


def test_final_edge_rejects_staged_symlink_substitution(tmp_path):
    if os.name == "nt":
        # File symlink creation is not reliably available on Windows build
        # hosts; the same boundary is exercised by Linux review.
        return
    bpy.ops.wm.read_factory_settings(use_empty=True)
    source = tmp_path / "source"
    source.mkdir()
    staging = tmp_path / "staging"
    staging.mkdir()
    root = _root_with_two_random_options(source)
    plan = prepare_composite_closure_export(
        root, source, source_root=source,
        mode=CLOSURE_MODE_COMPOSITES)
    staged = stage_composite_closure_export(plan, staging_dir=staging)
    victim = staged[0]
    replacement = tmp_path / "same_bytes.placement"
    replacement.write_bytes(victim.payload)
    victim.staged_path.unlink()
    victim.staged_path.symlink_to(replacement)

    with pytest.raises(MHValidationError, match="staged closure payload changed"):
        revalidate_composite_closure_export(plan, staged)


def test_direct_unmanaged_loaded_dependency_blocks_even_when_source_exists(
        tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    _write_composite(tmp_path, Composite("nested", []))
    unmanaged = _collection("nested.composite")
    root = _collection("unmanaged_root")
    placement = bpy.data.objects.new("nested_placement", None)
    root.objects.link(placement)
    placement.mh4blend.kind = "composite"
    placement[NODE_RESOURCE_KEY] = "nested"
    placement.instance_type = "COLLECTION"
    placement.instance_collection = unmanaged

    with pytest.raises(MHValidationError) as caught:
        prepare_composite_closure_export(
            root, tmp_path, source_root=tmp_path,
            mode=CLOSURE_MODE_COMPOSITES)
    assert caught.value.code == "MH_E_INVALID_RESOURCE_SOURCE"
    assert not (tmp_path / "unmanaged_root.composite").exists()


def test_composite_only_loaded_mesh_requires_existing_source(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mesh_collection = _collection("loaded_mesh")
    mesh = bpy.data.meshes.new("loaded_mesh_geometry")
    mesh.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
    mesh_object = bpy.data.objects.new("loaded_mesh", mesh)
    mesh_collection.objects.link(mesh_object)
    material = bpy.data.materials.new("body_mat")
    material.mh4blend.mode = "CLASS"
    material.mh4blend.material_class = "painted"
    mesh.materials.append(material)
    stamp_resource_collection(mesh_collection, "mesh", "loaded_mesh")
    root = _collection("mesh_root")
    placement = bpy.data.objects.new("mesh_placement", None)
    root.objects.link(placement)
    placement.mh4blend.kind = "mesh"
    placement[NODE_RESOURCE_KEY] = "loaded_mesh"
    placement.instance_type = "COLLECTION"
    placement.instance_collection = mesh_collection

    with pytest.raises(MHValidationError) as caught:
        prepare_composite_closure_export(
            root, tmp_path, source_root=tmp_path,
            mode=CLOSURE_MODE_COMPOSITES)
    assert caught.value.code == "MH_E_RESOURCE_NOT_FOUND"

    plan = prepare_composite_closure_export(
        root, tmp_path, source_root=tmp_path,
        mode=CLOSURE_MODE_INCLUDE_ALL)
    assert [row.key for row in plan.to_publish] == [
        ResourceKey("material", "body_mat"),
        ResourceKey("static_mesh", "loaded_mesh"),
        ResourceKey("composite", "mesh_root"),
    ]


def test_texture_toggle_and_public_api_have_no_seed_surface(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root = _collection("root")
    signature = inspect.signature(prepare_composite_closure_export)
    assert "seed" not in {name.casefold() for name in signature.parameters}

    with pytest.raises(RuntimeError, match="OPEN-V5-12 STOP"):
        prepare_composite_closure_export(
            root, tmp_path, source_root=tmp_path,
            mode=CLOSURE_MODE_INCLUDE_ALL, include_textures=True)
