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
from mh4blend.core.batch_publish import BatchPartialPublishError  # noqa: E402
from mh4blend.core.materials import material_json_bytes  # noqa: E402
from mh4blend.core.model import (  # noqa: E402
    Composite,
    MaterialResource,
    Node,
    PlacementProfile,
)
from mh4blend.core.placements import placement_json_bytes  # noqa: E402
from mh4blend.core.source_closure import ResourceKey  # noqa: E402
from mh4blend.core.validate import MHValidationError  # noqa: E402
from mh4blend.scene.export_closure import (  # noqa: E402
    CLOSURE_MODE_COMPOSITES,
    CLOSURE_MODE_INCLUDE_ALL,
    export_composite_closure_collection,
    prepare_composite_closure_export,
    publish_composite_closure_export,
    revalidate_composite_closure_export,
    stage_composite_closure_export,
)
from mh4blend.scene.export_composite import (  # noqa: E402
    NODE_RESOURCE_KEY,
    UNRESOLVED_PLACEMENT_KEY,
)
from mh4blend.scene.resource_markers import (  # noqa: E402
    is_managed_resource_collection,
    stamp_resource_collection,
)
from mh4blend.scene.export_fbx import export_fbx_collection  # noqa: E402
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


def test_publication_reuses_unloaded_sources_and_replaces_root_last(tmp_path):
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
    reused_before = {
        row.key: (row.target.read_bytes(), row.target.stat().st_mtime_ns)
        for row in plan.reused
    }
    boundaries = []

    report = publish_composite_closure_export(
        plan,
        staged,
        lock_root=tmp_path / "locks",
        _boundary_hook=lambda event, item, published: boundaries.append(
            (event, item.identity, published)),
    )

    assert report["published"] == ["composite:closure_root"]
    assert boundaries == [
        ("before_replace", "composite:closure_root", ()),
        ("after_replace", "composite:closure_root",
         ("composite:closure_root",)),
    ]
    assert (source / "closure_root.composite").is_file()
    for row in plan.reused:
        assert (row.target.read_bytes(), row.target.stat().st_mtime_ns) == (
            reused_before[row.key])


def test_partial_failure_after_loaded_leaf_reports_exact_prefix_and_no_root(
        tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    source = tmp_path / "source"
    source.mkdir()
    staging = tmp_path / "staging"
    staging.mkdir()
    nested = _collection("nested")
    stamp_resource_collection(nested, "composite", "nested")
    root = _collection("root")
    placement = bpy.data.objects.new("nested_placement", None)
    root.objects.link(placement)
    placement.mh4blend.kind = "composite"
    placement[NODE_RESOURCE_KEY] = "nested"
    placement.instance_type = "COLLECTION"
    placement.instance_collection = nested
    plan = prepare_composite_closure_export(
        root, source, source_root=source,
        mode=CLOSURE_MODE_COMPOSITES)
    staged = stage_composite_closure_export(plan, staging_dir=staging)

    def fail_after_leaf(event, item, _published):
        if event == "after_replace" and item.identity == "composite:nested":
            raise RuntimeError("injected leaf boundary failure")

    with pytest.raises(BatchPartialPublishError) as caught:
        publish_composite_closure_export(
            plan,
            staged,
            lock_root=tmp_path / "locks",
            _boundary_hook=fail_after_leaf,
        )
    assert caught.value.published == ("composite:nested",)
    assert caught.value.unpublished == ("composite:root",)
    assert (source / "nested.composite").is_file()
    assert not (source / "root.composite").exists()


def test_high_level_closure_export_finalizes_blender_identity(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    source = tmp_path / "source"
    source.mkdir()
    root = _root_with_two_random_options(source)

    report = export_composite_closure_collection(
        root,
        source,
        source_root=source,
        mode=CLOSURE_MODE_COMPOSITES,
        lock_root=tmp_path / "locks",
    )

    assert report["root"] == "composite:closure_root"
    assert report["published"] == ["composite:closure_root"]
    assert is_managed_resource_collection(root, "composite", "closure_root")


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


def test_nested_unmanaged_binding_wins_before_missing_source_diagnostic(
        tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    unmanaged_leaf = _collection("unmanaged_leaf")
    nested = _collection("nested")
    stamp_resource_collection(nested, "composite", "nested")
    bad = bpy.data.objects.new("bad_leaf", None)
    nested.objects.link(bad)
    bad.mh4blend.kind = "composite"
    bad[NODE_RESOURCE_KEY] = "missing_leaf"
    bad.instance_type = "COLLECTION"
    bad.instance_collection = unmanaged_leaf
    root = _collection("root")
    placement = bpy.data.objects.new("nested_placement", None)
    root.objects.link(placement)
    placement.mh4blend.kind = "composite"
    placement[NODE_RESOURCE_KEY] = "nested"
    placement.instance_type = "COLLECTION"
    placement.instance_collection = nested

    with pytest.raises(MHValidationError) as caught:
        prepare_composite_closure_export(
            root, tmp_path, source_root=tmp_path,
            mode=CLOSURE_MODE_COMPOSITES)
    assert caught.value.code == "MH_E_INVALID_RESOURCE_SOURCE"
    assert not (tmp_path / "root.composite").exists()


def test_composite_only_loaded_mesh_requires_existing_source(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    source = tmp_path / "source"
    source.mkdir()
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
    placement.mh4blend.profile = "scatter"
    placement[NODE_RESOURCE_KEY] = "loaded_mesh"
    placement.instance_type = "COLLECTION"
    placement.instance_collection = mesh_collection
    (source / "scatter.placement").write_bytes(
        placement_json_bytes(PlacementProfile("scatter")))

    with pytest.raises(MHValidationError) as caught:
        prepare_composite_closure_export(
            root, source, source_root=source,
            mode=CLOSURE_MODE_COMPOSITES)
    assert caught.value.code == "MH_E_RESOURCE_NOT_FOUND"
    assert "static_mesh:loaded_mesh" in caught.value.subjects
    assert "composite:mesh_root" in caught.value.subjects
    assert "Export Composite Include All Stuff" in caught.value.subjects

    plan = prepare_composite_closure_export(
        root, source, source_root=source,
        mode=CLOSURE_MODE_INCLUDE_ALL)
    assert [row.key for row in plan.to_publish] == [
        ResourceKey("material", "body_mat"),
        ResourceKey("static_mesh", "loaded_mesh"),
        ResourceKey("composite", "mesh_root"),
    ]
    stage_all = tmp_path / "stage_all"
    stage_all.mkdir()
    staged = stage_composite_closure_export(plan, staging_dir=stage_all)
    publish_composite_closure_export(
        plan, staged, lock_root=tmp_path / "locks")

    composite_only = prepare_composite_closure_export(
        root, source, source_root=source,
        mode=CLOSURE_MODE_COMPOSITES)
    assert [row.key for row in composite_only.payloads] == [
        ResourceKey("placement_profile", "scatter"),
        ResourceKey("material", "body_mat"),
        ResourceKey("static_mesh", "loaded_mesh"),
        ResourceKey("composite", "mesh_root"),
    ]
    assert [row.action for row in composite_only.payloads] == [
        "reuse", "reuse", "reuse", "publish"]
    stage_composites = tmp_path / "stage_composites"
    stage_composites.mkdir()
    staged = stage_composite_closure_export(
        composite_only, staging_dir=stage_composites)
    assert [row.planned.key for row in staged] == [
        row.key for row in composite_only.payloads]

    missing_image = bpy.data.images.new("missing_albedo", 1, 1)
    missing_image.filepath = str(source / "missing_albedo.png")
    texture_row = material.mh4blend.textures.add()
    texture_row.slot = 0
    texture_row.image = missing_image
    with pytest.raises(MHValidationError) as loaded_texture_error:
        prepare_composite_closure_export(
            root, source, source_root=source,
            mode=CLOSURE_MODE_INCLUDE_ALL)
    assert loaded_texture_error.value.code == (
        "MH_E_UNRESOLVED_TEXTURE_REFERENCE")
    assert "texture:missing_albedo" in loaded_texture_error.value.subjects
    assert "composite:mesh_root" in loaded_texture_error.value.subjects
    assert (
        "Copy All Textures to Project, then Remap All Texture Paths"
        in loaded_texture_error.value.subjects)
    material.mh4blend.textures.clear()

    (source / "body_mat.material").write_bytes(material_json_bytes(
        MaterialResource(
            "body_mat", material_class="painted",
            textures={"tex0": "missing_albedo"})))
    with pytest.raises(MHValidationError) as texture_error:
        prepare_composite_closure_export(
            root, source, source_root=source,
            mode=CLOSURE_MODE_COMPOSITES)
    assert texture_error.value.code == "MH_E_UNRESOLVED_TEXTURE_REFERENCE"
    assert "texture:missing_albedo" in texture_error.value.subjects
    assert "composite:mesh_root" in texture_error.value.subjects
    assert (
        "Copy All Textures to Project, then Remap All Texture Paths"
        in texture_error.value.subjects)
    (source / "missing_albedo.png").write_bytes(b"texture")
    with_texture = prepare_composite_closure_export(
        root, source, source_root=source,
        mode=CLOSURE_MODE_COMPOSITES)
    assert ResourceKey("texture", "missing_albedo") in (
        with_texture.full_closure_keys)
    assert ResourceKey("texture", "missing_albedo") not in {
        row.key for row in with_texture.payloads}
    if os.name != "nt":
        texture = source / "missing_albedo.png"
        outside = tmp_path / "outside_albedo.png"
        outside.write_bytes(b"outside")
        texture.unlink()
        texture.symlink_to(outside)
        with pytest.raises(MHValidationError) as outside_error:
            prepare_composite_closure_export(
                root, source, source_root=source,
                mode=CLOSURE_MODE_COMPOSITES)
        assert outside_error.value.code == "MH_E_TEXTURE_OUTSIDE_ROOT"
        assert "texture:missing_albedo" in outside_error.value.subjects
        assert "composite:mesh_root" in outside_error.value.subjects


def test_include_all_missing_mesh_reports_owner_without_command(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root = _collection("include_all_root")
    _unresolved_object(root, "missing_mesh", "mesh", "missing_mesh")

    with pytest.raises(MHValidationError) as caught:
        prepare_composite_closure_export(
            root, tmp_path, source_root=tmp_path,
            mode=CLOSURE_MODE_INCLUDE_ALL)
    assert caught.value.code == "MH_E_RESOURCE_NOT_FOUND"
    assert caught.value.subjects == [
        "composite:include_all_root", "static_mesh:missing_mesh"]
    assert "Export Composite Include All Stuff" not in str(caught.value)
    assert list(tmp_path.iterdir()) == []


def test_include_all_missing_material_reports_owner_without_command(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mesh_collection = _collection("existing_mesh")
    mesh = bpy.data.meshes.new("existing_mesh_geometry")
    mesh.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
    mesh_collection.objects.link(bpy.data.objects.new("existing_mesh", mesh))
    material = bpy.data.materials.new("missing_mat")
    mesh.materials.append(material)
    exported = export_fbx_collection(
        mesh_collection, tmp_path, source_root=tmp_path,
        export_materials=False)
    source_path = Path(exported["filepath"])
    source_bytes = source_path.read_bytes()

    bpy.ops.wm.read_factory_settings(use_empty=True)
    root = _collection("include_all_root")
    _unresolved_object(root, "existing_mesh", "mesh", "existing_mesh")
    with pytest.raises(MHValidationError) as caught:
        prepare_composite_closure_export(
            root, tmp_path, source_root=tmp_path,
            mode=CLOSURE_MODE_INCLUDE_ALL)
    assert caught.value.code == "MH_E_RESOURCE_NOT_FOUND"
    assert caught.value.subjects == [
        "composite:include_all_root", "material:missing_mat"]
    assert "Export Composite Include All Stuff" not in str(caught.value)
    assert source_path.read_bytes() == source_bytes
    assert sorted(path.name for path in tmp_path.iterdir()) == [
        "existing_mesh.mesh.fbx"]


def test_public_api_has_neither_seed_nor_texture_publish_toggle(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root = _collection("root")
    signature = inspect.signature(prepare_composite_closure_export)
    names = {name.casefold() for name in signature.parameters}
    assert "seed" not in names
    assert "include_textures" not in names


def test_unmanaged_dag4blend_dependency_names_the_converter_and_managed_twin():
    """The owner's second wall: say WHY it is unmanaged and what fixes it."""

    bpy.ops.wm.read_factory_settings(use_empty=True)
    root = bpy.data.collections.new("gaz53_b_random_cmp")
    stamp_resource_collection(root, "composite", "gaz53_b_random_cmp")

    # A dag4blend definition left in the file by dt.cmp_import.
    legacy = bpy.data.collections.new("gaz53_b_body_cmp")
    legacy["type"] = "composit"
    legacy["name"] = "gaz53_b_body_cmp"

    placement = bpy.data.objects.new("gaz53_b_body_cmp", None)
    root.objects.link(placement)
    placement.instance_type = "COLLECTION"
    placement.instance_collection = legacy

    with pytest.raises(MHValidationError) as caught:
        prepare_composite_closure_export(
            root, Path(bpy.app.tempdir), source_root=Path(bpy.app.tempdir),
            mode=CLOSURE_MODE_COMPOSITES)
    message = caught.value.message
    assert caught.value.code == "MH_E_INVALID_RESOURCE_SOURCE"
    assert "is a dag4blend definition" in message
    assert "mh.convert_dag4blend_composite" in message
    assert "mh.import_dagor_composite" in message
    assert "already exists in this file" not in message

    # Once the converted definition exists, the error must point straight at it.
    managed = bpy.data.collections.new("gaz53_b_body_cmp.composite")
    stamp_resource_collection(managed, "composite", "gaz53_b_body_cmp")
    with pytest.raises(MHValidationError) as rebound:
        prepare_composite_closure_export(
            root, Path(bpy.app.tempdir), source_root=Path(bpy.app.tempdir),
            mode=CLOSURE_MODE_COMPOSITES)
    assert "already exists in this file" in rebound.value.message
    assert "'gaz53_b_body_cmp.composite'" in rebound.value.message
    assert "repoint this placement" in rebound.value.message
