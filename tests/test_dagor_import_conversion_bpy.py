"""Blender-hosted gates for both V5-S3 Dagor conversion inputs."""

import math
from pathlib import Path
import sys

import pytest

bpy = pytest.importorskip("bpy")
from mathutils import Matrix  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.composites import composite_json_bytes  # noqa: E402
from mh4blend.core.dagor_composites import (  # noqa: E402
    parse_dagor_composite,
    parse_dagor_placement_include,
)
from mh4blend.core import (  # noqa: E402
    placement_publication as placement_publication_module,
)
from mh4blend.core.placements import placement_json_bytes  # noqa: E402
from mh4blend.scene import export_composite as export_composite_module  # noqa: E402
from mh4blend.scene.export_composite import export_composite_collection  # noqa: E402
from mh4blend.scene.import_dagor_composite import (  # noqa: E402
    _option_weight,
    convert_dag4blend_collection,
    convert_dag4blend_collection_closure,
    convert_dagor_composite,
    import_dag4blend_composite_collection,
    import_dagor_composite_file,
    load_dagor_composite_bundle,
    load_dagor_composite_documents,
)
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


def _write(path, text):
    path.write_text(text, encoding="utf-8", newline="\n")
    return path


def _dagor_resource_collection(datablock_name, resource_name, resource_type):
    collection = bpy.data.collections.new(datablock_name)
    collection["name"] = resource_name
    collection["type"] = resource_type
    return collection


def _empty(name, collection, *, instance=None, parent=None):
    obj = bpy.data.objects.new(name, None)
    collection.objects.link(obj)
    if instance is not None:
        obj.instance_type = "COLLECTION"
        obj.instance_collection = instance
    if parent is not None:
        obj.parent = parent
        obj.matrix_parent_inverse = Matrix.Identity(4)
    return obj


def _counts():
    return len(bpy.data.objects), len(bpy.data.collections), len(bpy.data.scenes)


def test_direct_dagor_closure_materializes_all_random_options_and_local_tm(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root_path = _write(tmp_path / "truck.composit.blk", '''\
className:t="composit"
node{
  name:t="truck_random:composit"
  tm:m=[[1,0,0] [0,1,0] [0,0,1] [0,0,0]]
}
''')
    _write(tmp_path / "truck_random.composit.blk", '''\
className:t="composit"
node{
  ent{ name:t="crate:PrEfAb"; weight:r=0.25; }
  ent{ name:t="driver:gameObj"; }
  ent{ name:t="truck_variant:composit"; weight:r=0.5; }
  tm:m=[[1,0,0] [0,1,0] [0,0,1] [1,2,3]]
}
''')
    _write(tmp_path / "truck_variant.composit.blk", '''\
className:t="composit"
node{ tm:m=[[1,0,0] [0,1,0] [0,0,1] [0,0,0]] }
''')
    mesh = _dagor_resource_collection("legacy_crate", "crate", "prefab")

    documents = load_dagor_composite_documents(
        root_path, source_root=tmp_path)
    assert list(documents) == ["truck", "truck_random", "truck_variant"]
    assert [option.resource for option in documents["truck_random"].nodes[0].options] \
        == ["crate", "driver", "truck_variant"]
    assert [option.weight for option in documents["truck_random"].nodes[0].options] \
        == [0.25, 1.0, 0.5]

    report = import_dagor_composite_file(
        root_path,
        source_root=tmp_path,
        load_mode="structure-only",
        definition_policy="reuse",
        resource_overrides={
            ("mesh", "crate"): mesh,
        },
    )
    assert report["load_mode"] == "structure-only"
    assert report["definition_policy"] == "reuse"
    assert report["composites"] == ["truck", "truck_random", "truck_variant"]
    random_collection = bpy.data.collections["truck_random.composite"]
    random_node = next(
        obj for obj in random_collection.objects if obj.mh4blend.kind == "random")
    assert tuple(random_node.matrix_local.translation) == pytest.approx((1.0, 3.0, 2.0))
    options = sorted(random_node.children, key=lambda obj: obj.mh4blend.option_index)
    assert [obj.mh4blend.kind for obj in options] == ["mesh", "actor", "composite"]
    assert [obj.mh4blend.weight for obj in options] == pytest.approx([0.25, 1.0, 0.5])
    assert options[0].instance_collection is bpy.data.collections["crate"]
    assert options[0].instance_collection is not mesh
    assert options[1].instance_collection is bpy.data.collections["driver.actor"]
    assert options[2].instance_collection is bpy.data.collections[
        "truck_variant.composite"]

    for name in ("truck_variant", "truck_random", "truck"):
        exported = export_composite_collection(
            bpy.data.collections[f"{name}.composite"],
            tmp_path,
            source_root=tmp_path,
        )
        assert Path(exported["filepath"]).read_bytes() == composite_json_bytes(
            documents[name])


def test_owner_gaz_fixture_materializes_and_exports_real_composite_options(
        tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root = REPO_ROOT / "reference" / "dagor_fixtures" / "gaz53"
    documents = load_dagor_composite_documents(
        root / "gaz53_b_random_cmp.composit.blk",
        source_root=root,
    )
    report = import_dagor_composite_file(
        root / "gaz53_b_random_cmp.composit.blk",
        source_root=root,
    )
    assert report["composites"] == [
        "gaz53_b_random_cmp",
        "gaz53_b_body_cmp",
        "gaz53_body_bc_random_cmp",
    ]
    random_definition = bpy.data.collections[
        "gaz53_body_bc_random_cmp.composite"]
    random_node = next(
        obj for obj in random_definition.objects
        if obj.mh4blend.kind == "random")
    options = sorted(
        random_node.children,
        key=lambda obj: obj.mh4blend.option_index,
    )
    assert [obj.mh4blend.kind for obj in options] == [
        "composite", "composite", "composite"]
    assert [obj.mh4blend.weight for obj in options] == [1.0, 1.0, 1.0]
    assert [obj.get("mh_composite_resource") for obj in options] == [
        "gaz53_bread_b_cmp", "gaz53_wooden_b_cmp", "gaz53_wooden_c_cmp"]
    assert all(obj.get("mh_unresolved_placement") is True for obj in options)

    for name in (
            "gaz53_b_body_cmp", "gaz53_body_bc_random_cmp",
            "gaz53_b_random_cmp"):
        exported = export_composite_collection(
            bpy.data.collections[f"{name}.composite"],
            tmp_path,
            source_root=tmp_path,
        )
        assert Path(exported["filepath"]).read_bytes() == composite_json_bytes(
            documents[name])


def test_direct_dagor_composed_world_shear_fails_before_blender_mutation(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    path = _write(tmp_path / "sheared.composit.blk", '''\
className:t="composit"
node{
  tm:m=[[2,0,0] [0,1,0] [0,0,1] [0,0,0]]
  node{
    name:t="crate:rendinst"
    tm:m=[[0.70710678,0.70710678,0] [-0.70710678,0.70710678,0]
          [0,0,1] [0,0,0]]
  }
}
''')
    before = _counts()
    with pytest.raises(
            ValueError, match="MH_E_UNREPRESENTABLE_TRANSFORM") as caught:
        import_dagor_composite_file(path, source_root=tmp_path)
    assert "composed world" in str(caught.value)
    assert "resource:crate" in str(caught.value)
    assert "$.nodes[0]" in str(caught.value)
    assert "$.nodes[0].children[0]" in str(caught.value)
    assert _counts() == before


def test_direct_dagor_root_ambiguity_blocks_selected_duplicate(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    first = tmp_path / "a"
    second = tmp_path / "b"
    first.mkdir()
    second.mkdir()
    payload = 'className:t="composit"\n'
    selected = _write(first / "duplicate.composit.blk", payload)
    _write(second / "duplicate.composit.blk", payload)
    before = _counts()

    with pytest.raises(ValueError, match="MH_E_AMBIGUOUS_RESOURCE_NAME"):
        load_dagor_composite_documents(selected, source_root=tmp_path)
    assert _counts() == before


def test_direct_include_requires_explicit_output_and_publishes_profile_sidecar(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    output = tmp_path / "mh"
    output.mkdir()
    include = _write(tmp_path / "vehicle_scatter.blk", '''\
offset_x:p2=10,1
offset_y:p2=0,2
offset_z:p2=3,0
rot_x:p2=0,0
rot_y:p2=0,0
rot_z:p2=0,180
scale:p2=1,0.1
yScale:p2=1,0.2
''')
    source = _write(tmp_path / "profiled.composit.blk", '''\
className:t="composit"
node{
  include "vehicle_scatter.blk"
}
''')

    bundle = load_dagor_composite_bundle(source, source_root=tmp_path)
    assert bundle.documents["profiled"].nodes[0].profile == "vehicle_scatter"
    assert list(bundle.profiles) == ["vehicle_scatter"]
    before = _counts()
    with pytest.raises(ValueError, match="explicit output_dir"):
        import_dagor_composite_file(source, source_root=tmp_path)
    assert _counts() == before
    assert not (output / "vehicle_scatter.placement").exists()

    report = import_dagor_composite_file(
        source, source_root=tmp_path, output_dir=output)
    assert report["placement_profiles"] == [{
        "name": "vehicle_scatter",
        "filepath": str(output / "vehicle_scatter.placement"),
        "include_path": str(include),
        "bytes": len(bundle.profiles["vehicle_scatter"].canonical_bytes),
        "written": True,
        "reused": False,
    }]
    placement = output / "vehicle_scatter.placement"
    assert placement.read_bytes() == bundle.profiles[
        "vehicle_scatter"].canonical_bytes
    node = next(iter(bpy.data.collections["profiled.composite"].objects))
    assert node.mh4blend.profile == "vehicle_scatter"
    assert node["mh_composite_profile"] == "vehicle_scatter"

    node["mh_composite_profile"] = "artist_mirror_edit"
    real_publish = export_composite_module.atomic_publish_bytes

    def fail_publish(*_args, **_kwargs):
        raise RuntimeError("injected publish failure")

    monkeypatch.setattr(
        export_composite_module, "atomic_publish_bytes", fail_publish)
    with pytest.raises(RuntimeError, match="injected publish failure"):
        export_composite_collection(
            bpy.data.collections["profiled.composite"], output,
            source_root=tmp_path)
    assert node["mh_composite_profile"] == "artist_mirror_edit"
    monkeypatch.setattr(
        export_composite_module, "atomic_publish_bytes", real_publish)

    exported = export_composite_collection(
        bpy.data.collections["profiled.composite"], output,
        source_root=tmp_path)
    assert node["mh_composite_profile"] == "vehicle_scatter"
    assert Path(exported["filepath"]).read_bytes() == composite_json_bytes(
        bundle.documents["profiled"])

    profile_bytes = placement.read_bytes()
    placement.write_bytes(b" " + profile_bytes)
    with pytest.raises(ValueError, match="exact canonical bytes"):
        export_composite_collection(
            bpy.data.collections["profiled.composite"], output,
            source_root=tmp_path)
    placement.write_bytes(profile_bytes)

    duplicate_dir = tmp_path / "other"
    duplicate_dir.mkdir()
    duplicate = duplicate_dir / placement.name
    duplicate.write_bytes(profile_bytes)
    with pytest.raises(ValueError, match="MH_E_AMBIGUOUS_RESOURCE_NAME"):
        export_composite_collection(
            bpy.data.collections["profiled.composite"], output,
            source_root=tmp_path)
    duplicate.unlink()

    placement.unlink()
    with pytest.raises(ValueError, match="required source resource"):
        export_composite_collection(
            bpy.data.collections["profiled.composite"], output,
            source_root=tmp_path)
    placement.write_bytes(profile_bytes)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    reused = import_dagor_composite_file(
        source, source_root=tmp_path, output_dir=output)
    assert reused["placement_profiles"][0]["written"] is False
    assert reused["placement_profiles"][0]["reused"] is True


def test_random_option_typed_profile_is_never_silently_ignored():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = bpy.data.collections.new("option_profile_test")
    random_node = _empty("random", collection)
    random_node.mh4blend.kind = "random"
    option = _empty("option", collection, parent=random_node)
    option.mh4blend.kind = "empty"
    option.mh4blend.option_index = 0
    option.mh4blend.profile = "forbidden_profile"
    with pytest.raises(ValueError, match="cannot carry placement profile"):
        composite_authoring.validate_random_options(random_node)


def test_direct_include_collision_noncanonical_and_duplicate_files_fail_closed(
        tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    output = tmp_path / "mh"
    output.mkdir()
    include = _write(tmp_path / "scatter.blk", '''\
offset_x:p2=0,0
offset_y:p2=0,0
offset_z:p2=0,0
''')
    source = _write(tmp_path / "profiled.composit.blk", '''\
className:t="composit"
node{ include "scatter.blk" }
''')
    first = import_dagor_composite_file(
        source, source_root=tmp_path, output_dir=output)
    assert first["placement_profiles"][0]["written"] is True

    bpy.ops.wm.read_factory_settings(use_empty=True)
    _write(include, '''\
offset_x:p2=1,0
offset_y:p2=0,0
offset_z:p2=0,0
''')
    before = _counts()
    with pytest.raises(ValueError, match="MH_E_AMBIGUOUS_RESOURCE_NAME") as caught:
        import_dagor_composite_file(
            source, source_root=tmp_path, output_dir=output)
    assert str(include) in caught.value.subjects
    assert str(output / "scatter.placement") in caught.value.subjects
    assert _counts() == before

    _write(include, '''\
offset_x:p2=0,0
offset_y:p2=0,0
offset_z:p2=0,0
''')
    duplicate_dir = tmp_path / "duplicate"
    duplicate_dir.mkdir()
    (duplicate_dir / "scatter.placement").write_bytes(
        (output / "scatter.placement").read_bytes())
    with pytest.raises(ValueError, match="MH_E_AMBIGUOUS_RESOURCE_NAME"):
        import_dagor_composite_file(
            source, source_root=tmp_path, output_dir=output)

    noncanonical = _write(tmp_path / "Bad Profile.blk", "scale:p2=1,0\n")
    bad_source = _write(tmp_path / "bad.composit.blk", '''\
className:t="composit"
node{ include "Bad Profile.blk" }
''')
    with pytest.raises(
            ValueError, match="MH_E_NONCANONICAL_RESOURCE_NAME") as bad:
        import_dagor_composite_file(
            bad_source, source_root=tmp_path, output_dir=output)
    assert str(noncanonical) in bad.value.subjects


def test_profile_publication_failure_rolls_back_blender_transaction(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    output = tmp_path / "mh"
    output.mkdir()
    _write(tmp_path / "scatter.blk", "scale:p2=1,0\n")
    source = _write(tmp_path / "profiled.composit.blk", '''\
className:t="composit"
node{ include "scatter.blk" }
''')
    before = _counts()
    real_publish = placement_publication_module.atomic_publish_bytes

    def fail_publish(*_args, **_kwargs):
        raise RuntimeError("profile publication failed")

    monkeypatch.setattr(
        placement_publication_module, "atomic_publish_bytes", fail_publish)
    with pytest.raises(RuntimeError, match="profile publication failed"):
        import_dagor_composite_file(
            source, source_root=tmp_path, output_dir=output)
    assert _counts() == before
    assert bpy.data.collections.get("profiled.composite") is None
    assert not (output / "scatter.placement").exists()

    def inject_racing_identity(target, payload, **kwargs):
        Path(target).write_bytes(payload)
        return real_publish(target, payload, **kwargs)

    monkeypatch.setattr(
        placement_publication_module, "atomic_publish_bytes",
        inject_racing_identity)
    with pytest.raises(ValueError, match="race overwrite"):
        import_dagor_composite_file(
            source, source_root=tmp_path, output_dir=output)
    assert _counts() == before
    assert bpy.data.collections.get("profiled.composite") is None
    # The independently arrived file remains; the importer did not replace it.
    assert (output / "scatter.placement").is_file()


def test_dag4blend_helper_and_marker_paths_lift_options_without_helper_authority(
        tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    source = _dagor_resource_collection(
        "legacy_source", "legacy_vehicle", "composit")
    mesh = _dagor_resource_collection("legacy_mesh", "wheel", "rendInst")
    actor = _dagor_resource_collection("legacy_actor", "driver", "gameobj")

    group = _empty("legacy_group", source)
    group.matrix_local = Matrix.Translation((2.0, 0.0, 0.0))
    ordinary = _empty("legacy_wheel", source, instance=mesh, parent=group)
    ordinary.matrix_local = Matrix.Translation((0.25, 0.0, 0.0))

    named_helper = bpy.data.collections.new("random.001")
    named_random = _empty(
        "legacy_random_by_helper", source, instance=named_helper, parent=group)
    helper_mesh = _empty("ent_mesh", named_helper, instance=mesh)
    helper_mesh["weight:r"] = 2.0
    _empty("ent_actor", named_helper, instance=actor)

    marker_helper = bpy.data.collections.new("legacy_choices")
    marker_random = _empty(
        "legacy_random_by_marker", source, instance=marker_helper, parent=group)
    marker_random["type:t"] = "random"
    _empty("ent_marker", marker_helper, instance=actor)

    nested = _dagor_resource_collection(
        "legacy_nested", "nested_variant", "composit")
    nested_helper = bpy.data.collections.new("random.009")
    _empty("legacy_nested_random", nested, instance=nested_helper)
    _empty("nested_ent_mesh", nested_helper, instance=mesh)
    _empty("ent_nested", named_helper, instance=nested)

    document, discovered = convert_dag4blend_collection(source)
    assert document.name == "legacy_vehicle"
    assert list(discovered) == [
        ("mesh", "wheel"), ("actor", "driver"),
        ("composite", "nested_variant")]
    assert document.nodes[0].kind == "group"
    assert [child.kind for child in document.nodes[0].children] == [
        "mesh", "random", "random"]
    assert [option.weight for option in document.nodes[0].children[1].options] \
        == [2.0, 1.0, 1.0]

    closure_documents, _closure_overrides = (
        convert_dag4blend_collection_closure(source))
    report = import_dag4blend_composite_collection(
        source, load_mode="structure-only")
    assert report["composites"] == ["legacy_vehicle", "nested_variant"]
    target = report["collection"]
    converted_randoms = [
        obj for obj in target.objects if obj.mh4blend.kind == "random"]
    assert len(converted_randoms) == 2
    lifted = sorted(
        converted_randoms[0].children,
        key=lambda obj: obj.mh4blend.option_index,
    )
    assert [obj.instance_collection for obj in lifted] == [
        bpy.data.collections["wheel"], bpy.data.collections["driver.actor"],
        bpy.data.collections["nested_variant.composite"]]
    assert lifted[1].instance_collection is not actor
    nested_target = bpy.data.collections["nested_variant.composite"]
    converted_nested_random = next(
        obj for obj in nested_target.objects if obj.mh4blend.kind == "random")
    nested_lifted = list(converted_nested_random.children)
    assert len(nested_lifted) == 1
    assert nested_lifted[0].instance_collection is bpy.data.collections["wheel"]
    assert all(obj.instance_collection not in {named_helper, marker_helper}
               for obj in target.objects)
    tech = bpy.data.scenes["TECH"].collection
    assert tech.children.get(named_helper.name) is None
    assert tech.children.get(marker_helper.name) is None
    assert tech.children.get(nested_helper.name) is None
    assert helper_mesh["weight:r"] == 2.0

    for name in ("nested_variant", "legacy_vehicle"):
        exported = export_composite_collection(
            bpy.data.collections[f"{name}.composite"],
            tmp_path,
            source_root=tmp_path,
        )
        assert Path(exported["filepath"]).read_bytes() == composite_json_bytes(
            closure_documents[name])


def test_dag4blend_weight_cascade_prefers_dagorprops_then_id_then_one():
    class FakeOption(dict):
        def __init__(self, dagorprops, **values):
            super().__init__(values)
            self.dagorprops = dagorprops

    assert _option_weight(
        FakeOption({"weight:r": 3.0}, **{"weight:r": 2.0}), "ent[0]") == 3.0
    assert _option_weight(
        FakeOption({}, **{"weight:r": 2.0}), "ent[1]") == 2.0
    assert _option_weight(FakeOption({}), "ent[2]") == 1.0


def test_dag4blend_requires_explicit_resource_type_and_direct_mapping():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    source = _dagor_resource_collection("legacy", "strict_cmp", "composit")
    incomplete = bpy.data.collections.new("looks_like_mesh_but_is_not_authority")
    incomplete["name"] = "mesh_a"
    _empty("ambiguous", source, instance=incomplete)
    before = _counts()
    with pytest.raises(ValueError, match="MH_E_COMPOSITE_GRAMMAR"):
        convert_dag4blend_collection(source)
    assert _counts() == before

    graph = parse_dagor_composite(
        'className:t="composit" node{ name:t="crate:ReNdInSt"; }',
        source="typed.composit.blk",
        name="typed",
    )
    document = convert_dagor_composite(graph)
    assert document.nodes[0].kind == "mesh"
    assert document.nodes[0].resource == "crate"


def test_dag4blend_p2_preview_never_silently_replaces_profile_authority(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    source = _dagor_resource_collection(
        "legacy_profile", "legacy_profile", "composit")
    node = _empty("scatter_preview", source)
    node["offset_x:p2"] = [10.0, 2.0]
    node["mh_composite_profile"] = "mirror_is_not_authority"
    with pytest.raises(ValueError, match="lossless dag4blend conversion") as caught:
        convert_dag4blend_collection(source)
    assert "collection:legacy_profile/object:scatter_preview" in str(caught.value)
    assert "offset_x:p2" in str(caught.value)

    node.mh4blend.profile = "vehicle_scatter"
    document, _overrides = convert_dag4blend_collection(source)
    assert document.nodes[0].profile == "vehicle_scatter"

    canonical = placement_json_bytes(parse_dagor_placement_include(
        "scale:p2=1,0", source="vehicle_scatter.blk",
        name="vehicle_scatter"))
    (tmp_path / "vehicle_scatter.placement").write_bytes(canonical)
    before = _counts()
    with pytest.raises(ValueError, match="Project Source Root"):
        import_dag4blend_composite_collection(source)
    assert _counts() == before

    report = import_dag4blend_composite_collection(
        source, source_root=tmp_path)
    converted = next(iter(report["collection"].objects))
    assert converted.mh4blend.profile == "vehicle_scatter"


def test_dag4blend_same_composite_name_from_two_collections_is_ambiguous():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    source = _dagor_resource_collection("root", "root_cmp", "composit")
    first = _dagor_resource_collection("first", "same_cmp", "composit")
    second = _dagor_resource_collection("second", "same_cmp", "composit")
    _empty("first_ref", source, instance=first)
    _empty("second_ref", source, instance=second)
    before = _counts()
    with pytest.raises(ValueError, match="MH_E_AMBIGUOUS_RESOURCE_NAME"):
        import_dag4blend_composite_collection(source)
    assert _counts() == before


def test_dag4blend_random_option_cycle_fails_before_materialization():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    first = _dagor_resource_collection("cycle_a_raw", "cycle_a", "composit")
    second = _dagor_resource_collection("cycle_b_raw", "cycle_b", "composit")
    first_helper = bpy.data.collections.new("random.cycle_a")
    second_helper = bpy.data.collections.new("random.cycle_b")
    _empty("random_a", first, instance=first_helper)
    _empty("random_b", second, instance=second_helper)
    _empty("a_to_b", first_helper, instance=second)
    _empty("b_to_a", second_helper, instance=first)
    before = _counts()

    with pytest.raises(ValueError, match="MH_E_COMPOSITE_CYCLE"):
        import_dag4blend_composite_collection(first)
    assert _counts() == before
    assert bpy.data.scenes.get("COMPOSITE") is None


@pytest.mark.parametrize("boundary", ["local", "composed world"])
def test_dag4blend_shear_reports_working_scene_object_provenance(boundary):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    source = _dagor_resource_collection(
        "shear_source", "shear_source", "composit")
    bpy.context.scene.collection.children.link(source)
    parent = _empty("parent", source)
    if boundary == "local":
        parent.matrix_world = Matrix((
            (1.0, 0.5, 0.0, 0.0),
            (0.0, 1.0, 0.0, 0.0),
            (0.0, 0.0, 1.0, 0.0),
            (0.0, 0.0, 0.0, 1.0),
        ))
        expected_object = "object:parent"
    else:
        parent.scale = (2.0, 1.0, 1.0)
        child = _empty("child", source, parent=parent)
        child.rotation_euler[2] = math.radians(45.0)
        expected_object = "object:child"
        bpy.context.view_layer.update()
    before = _counts()
    with pytest.raises(
            ValueError, match="MH_E_UNREPRESENTABLE_TRANSFORM") as caught:
        convert_dag4blend_collection(source)
    assert boundary in str(caught.value)
    assert "collection:shear_source/object:parent" in str(caught.value)
    assert expected_object in str(caught.value)
    assert _counts() == before
