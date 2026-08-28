"""Owner doc13 regression gates for the explicit dag4blend bridge."""

from pathlib import Path
import sys

import pytest

bpy = pytest.importorskip("bpy")
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "addon"))

from mh4blend.scene.import_dagor_composite import (  # noqa: E402
    convert_dag4blend_collection,
    import_dag4blend_composite_collection,
)
from mh4blend.scene import import_dagor_composite as bridge  # noqa: E402
from mh4blend.scene.export_composite import _extract_composite  # noqa: E402
from mh4blend.core.composites import composite_json_bytes  # noqa: E402
from mh4blend.core.batch_publish import BatchPartialPublishError  # noqa: E402
from mh4blend.scene.resource_markers import (  # noqa: E402
    COLLECTION_KIND_KEY,
    stamp_resource_collection,
)
from mh4blend.ui import composite_authoring, ops  # noqa: E402


@pytest.fixture(autouse=True)
def properties():
    own_material = not hasattr(bpy.types.Material, "mh4blend")
    own_object = not hasattr(bpy.types.Object, "mh4blend")
    if own_material:
        ops.register()
    if own_object:
        composite_authoring.register()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    yield
    if own_object:
        composite_authoring.unregister()
    if own_material:
        ops.unregister()


def legacy(name, kind="composit"):
    collection = bpy.data.collections.new(name)
    collection["name"] = name
    collection["type"] = kind
    return collection


def empty(name, collection, instance=None, parent=None):
    obj = bpy.data.objects.new(name, None)
    collection.objects.link(obj)
    if instance is not None:
        obj.instance_type = "COLLECTION"
        obj.instance_collection = instance
    obj.parent = parent
    return obj


def test_mixed_empty_option_keeps_weight_and_order():
    source = legacy("bridge_root")
    child = legacy("bridge_child")
    helper = bpy.data.collections.new("random.bridge")
    empty("choice", source, helper)
    empty("absent", helper)["weight:r"] = 2.0
    empty("present", helper, child)["weight:r"] = 1.0
    document, resources = convert_dag4blend_collection(source)
    assert [(row.kind, row.resource, row.weight)
            for row in document.nodes[0].options] == [
        ("empty", None, 2.0), ("composite", "bridge_child", 1.0)]
    assert list(resources) == [("composite", "bridge_child")]
    report = import_dag4blend_composite_collection(source)
    assert composite_json_bytes(_extract_composite(report["collection"])) == (
        composite_json_bytes(document))
    assert len(helper.objects) == 2
    assert helper.objects["absent"].instance_collection is None


def test_all_empty_options_do_not_create_an_entity_or_discard_children():
    source = legacy("bridge_empty")
    helper = bpy.data.collections.new("random.empty")
    parent = empty("choice", source, helper)
    parent.location.x = 2.0
    empty("absent", helper)["weight:r"] = 2.0
    empty("structural_child", source, parent=parent).location.x = 0.25
    document, resources = convert_dag4blend_collection(source)
    assert document.nodes[0].kind == "group"
    assert document.nodes[0].options == []
    assert len(document.nodes[0].children) == 1
    assert document.nodes[0].children[0].kind == "group"
    assert resources == {}


def test_external_placements_are_relinked_without_mutating_legacy(tmp_path):
    working_scene = bpy.context.scene
    source = legacy("bridge_root")
    child = legacy("bridge_child")
    legacy_inner = empty("legacy_inner", source, child)
    outside = empty("external", working_scene.collection, source)
    report = import_dag4blend_composite_collection(source, source_root=tmp_path)
    assert outside.instance_collection is report["collection"]
    assert legacy_inner.instance_collection is child
    assert bpy.data.collections.get(source.name) is source
    assert bpy.data.collections.get(child.name) is child
    assert report["relinked_placements"] == [outside.name]


@pytest.mark.parametrize("weight", [-1.0, float("nan"), float("inf")])
def test_invalid_empty_weight_still_fails_closed(weight):
    source = legacy("bridge_invalid")
    helper = bpy.data.collections.new("random.invalid")
    empty("choice", source, helper)
    empty("absent", helper)["weight:r"] = weight
    with pytest.raises(ValueError, match="MH_E_COMPOSITE_GRAMMAR"):
        convert_dag4blend_collection(source)


@pytest.mark.parametrize("weight", [0.0, 1.0, 2.0])
def test_all_empty_list_is_cleared_even_with_zero_total(weight):
    source = legacy("bridge_none")
    helper = bpy.data.collections.new("random.none")
    empty("choice", source, helper)
    empty("absent", helper)["weight:r"] = weight
    document, _resources = convert_dag4blend_collection(source)
    assert document.nodes[0].kind == "group"
    assert document.nodes[0].options == []


def test_relink_does_not_touch_helper_service_or_other_scene_objects(tmp_path):
    working = bpy.context.scene
    root = legacy("bridge_root")
    child = legacy("bridge_child")
    helper = bpy.data.collections.new("random.bridge")
    empty("random", root, helper)
    option = empty("option", helper, child)
    ordinary = empty("inner", root, child)
    # A shared link into the working scene does not transfer definition ownership.
    working.collection.objects.link(option)
    working.collection.objects.link(ordinary)
    protected_scene = bpy.data.scenes.new("TECH_STUFF")
    protected = empty("protected", protected_scene.collection, root)
    working.collection.objects.link(protected)
    other = bpy.data.scenes.new("other_working_scene")
    elsewhere = empty("elsewhere", other.collection, root)
    outside = empty("outside", working.collection, child)
    report = import_dag4blend_composite_collection(root, source_root=tmp_path)
    assert outside.instance_collection is bpy.data.collections["bridge_child.composite"]
    assert option.instance_collection is child
    assert ordinary.instance_collection is child
    assert protected.instance_collection is root
    assert elsewhere.instance_collection is root
    assert report["relinked_placements"] == ["outside"]


def test_relink_rollback_restores_external_pointer_and_entire_delta(
        tmp_path, monkeypatch):
    root = legacy("bridge_root")
    external = empty("outside", bpy.context.scene.collection, root)
    counts = (len(bpy.data.objects), len(bpy.data.collections), len(bpy.data.scenes))
    real_schedule = bridge._schedule_dag4blend_relinks

    def inject(context, planned, **kwargs):
        real_schedule(context, planned, **kwargs)

        def fail_after_relink():
            assert external.instance_collection is not root
            raise RuntimeError("injected after external relink")

        context["transaction"].add_finalize(fail_after_relink)

    monkeypatch.setattr(bridge, "_schedule_dag4blend_relinks", inject)
    with pytest.raises(RuntimeError, match="injected after external relink"):
        import_dag4blend_composite_collection(root, source_root=tmp_path)
    assert external.instance_collection is root
    assert (len(bpy.data.objects), len(bpy.data.collections), len(bpy.data.scenes)) == counts


def test_conversion_from_service_scene_never_relinks_service_contents(tmp_path):
    root = legacy("bridge_root")
    service = bpy.data.scenes.new("COMPOSITS")
    external = empty("service_placement", service.collection, root)
    bpy.context.window.scene = service
    report = import_dag4blend_composite_collection(root, source_root=tmp_path)
    assert external.instance_collection is root
    assert report["relinked_placements"] == []


@pytest.mark.parametrize("owner_kind", ["managed", "dagor_owner_type"])
def test_relink_protects_definition_ownership_outside_service_scenes(
        tmp_path, owner_kind):
    root = legacy("bridge_root")
    working = bpy.context.scene
    definition = bpy.data.collections.new("other_definition")
    working.collection.children.link(definition)
    internal = empty("protected_internal", definition, root)
    if owner_kind == "managed":
        stamp_resource_collection(definition, "composite", "other_definition")
    else:
        # This is the other legitimate _resource_identity path: collection
        # name + type:t on the instance owner, with no collection['type'].
        definition["name"] = "other_definition"
        placement = empty("other_instance", working.collection, definition)
        placement["type:t"] = "composit"
    outside = empty("external", working.collection, root)
    report = import_dag4blend_composite_collection(root, source_root=tmp_path)
    assert internal.instance_collection is root
    assert outside.instance_collection is report["collection"]
    assert report["relinked_placements"] == [outside.name]


def test_relink_revalidates_ownership_at_commit_without_changing_any_pointer(
        tmp_path, monkeypatch):
    root = legacy("bridge_root")
    first = empty("first", bpy.context.scene.collection, root)
    second = empty("second", bpy.context.scene.collection, root)
    real_schedule = bridge._schedule_dag4blend_relinks

    def inject(context, planned, **kwargs):
        # A handler gives a service scene ownership after preflight. Do not
        # link the instance into its own definition (Blender forbids cycles).
        protected = context["service_scenes"]["TECH"].collection
        protected.objects.link(second)
        context["transaction"].add_rollback(
            lambda: protected.objects.unlink(second))
        real_schedule(context, planned, **kwargs)

    monkeypatch.setattr(bridge, "_schedule_dag4blend_relinks", inject)
    with pytest.raises(ValueError, match="ownership changed"):
        import_dag4blend_composite_collection(root, source_root=tmp_path)
    assert first.instance_collection is root
    assert second.instance_collection is root
    assert root.objects.get(second.name) is None
    assert bpy.data.collections.get("bridge_root.composite") is None


def test_full_route_adopts_mesh_through_writer_before_composite(tmp_path):
    source = legacy("bridge_root")
    mesh = legacy("bridge_mesh", "rendinst")
    bpy.context.scene.collection.children.link(mesh)
    data = bpy.data.meshes.new("triangle_data")
    data.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
    obj = bpy.data.objects.new("triangle", data)
    mesh.objects.link(obj)
    empty("mesh_placement", source, mesh)
    report = bridge.publish_dag4blend_composite_collection(
        source, tmp_path, source_root=tmp_path)
    assert (tmp_path / "bridge_mesh.mesh.fbx").is_file()
    assert (tmp_path / "bridge_root.composite").is_file()
    assert report["ok"]


def test_inline_p2_failure_names_canonical_path_and_every_parameter():
    root = legacy("bridge_root")
    group = empty("group", root)
    node = empty("scatter", root, parent=group)
    node["rot_y:p2"] = [0.0, 180.0]
    node["offset_x:p2"] = [0.0, 2.0]
    with pytest.raises(ValueError, match="MH_E_COMPOSITE_GRAMMAR") as caught:
        convert_dag4blend_collection(root)
    message = str(caught.value)
    assert "bridge_root:nodes[0]/children[0]" in message
    assert "parameters: offset_x:p2, rot_y:p2" in message
    assert "object:scatter" in message


def _nested_mesh_bridge(tmp_path):
    source = tmp_path / "source"
    source.mkdir()
    root = legacy("bridge_root")
    child = legacy("bridge_child")
    mesh = legacy("bridge_mesh", "rendinst")
    bpy.context.scene.collection.children.link(mesh)
    material = bpy.data.materials.new("bridge_surface")
    material.mh4blend.material_class = "bridge_shader"
    data = bpy.data.meshes.new("triangle_data")
    data.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
    data.materials.append(material)
    mesh.objects.link(bpy.data.objects.new("triangle", data))
    empty("mesh_placement", child, mesh)
    empty("child_placement", root, child)
    outside = empty("external", bpy.context.scene.collection, root)
    return source, root, child, mesh, outside


def test_partial_bridge_adopts_only_published_mesh_and_retry_relinks(tmp_path):
    source, root, child, mesh, outside = _nested_mesh_bridge(tmp_path)

    def fail(event, item, _published):
        if event == "after_replace" and item.identity == "static_mesh:bridge_mesh":
            raise RuntimeError("injected after mesh publication")

    with pytest.raises(BatchPartialPublishError) as caught:
        bridge.publish_dag4blend_composite_collection(
            root, source, source_root=source,
            lock_root=tmp_path / "locks", _boundary_hook=fail)
    assert caught.value.published == (
        "material:bridge_surface", "static_mesh:bridge_mesh")
    assert caught.value.unpublished == (
        "composite:bridge_child", "composite:bridge_root")
    assert mesh[COLLECTION_KIND_KEY] == "mesh"
    assert COLLECTION_KIND_KEY not in root
    assert COLLECTION_KIND_KEY not in child
    assert outside.instance_collection is root
    assert not (source / "bridge_child.composite").exists()
    assert not (source / "bridge_root.composite").exists()
    report = bridge.publish_dag4blend_composite_collection(
        root, source, source_root=source, lock_root=tmp_path / "locks")
    assert outside.instance_collection is report["collection"]
    assert report["publication"]["published"] == [
        "material:bridge_surface", "static_mesh:bridge_mesh",
        "composite:bridge_child", "composite:bridge_root"]
    assert root.objects["child_placement"].instance_collection is child
    assert child.objects["mesh_placement"].instance_collection is mesh


def test_postpublication_blender_failure_reports_full_durable_set(
        tmp_path, monkeypatch):
    source, root, _child, mesh, outside = _nested_mesh_bridge(tmp_path)

    def fail(*_args, **_kwargs):
        raise RuntimeError("injected Blender finalization failure")

    with monkeypatch.context() as patch:
        patch.setattr(bridge, "materialize_composite_documents", fail)
        with pytest.raises(BatchPartialPublishError) as caught:
            bridge.publish_dag4blend_composite_collection(
                root, source, source_root=source, lock_root=tmp_path / "locks")
    assert caught.value.published == (
        "material:bridge_surface", "static_mesh:bridge_mesh",
        "composite:bridge_child", "composite:bridge_root")
    assert caught.value.unpublished == ()
    assert "Blender finalization" in str(caught.value)
    assert (source / "bridge_root.composite").exists()
    assert mesh[COLLECTION_KIND_KEY] == "mesh"
    assert outside.instance_collection is root
    report = bridge.publish_dag4blend_composite_collection(
        root, source, source_root=source, lock_root=tmp_path / "locks")
    assert outside.instance_collection is report["collection"]
