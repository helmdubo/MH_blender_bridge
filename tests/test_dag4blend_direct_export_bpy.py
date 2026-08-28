"""R2 direct-export gates: adapters read scenes, publication writes files."""

from pathlib import Path
import sys

import pytest

bpy = pytest.importorskip("bpy")
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "addon"))
from mh4blend.scene.export_closure import export_composite_closure_collection
from mh4blend.scene.export_composite import export_composite_collection
from mh4blend.core.batch_publish import BatchPartialPublishError
from mh4blend.core.composites import composite_json_bytes
from mh4blend.scene.import_dagor_composite import convert_dag4blend_collection
from mh4blend.scene.import_composite import materialize_composite_documents
from mh4blend.scene.export_composite import _extract_composite
from mh4blend.ui import composite_authoring, ops


@pytest.fixture(autouse=True)
def properties():
    own_material = not hasattr(bpy.types.Material, "mh4blend")
    own_object = not hasattr(bpy.types.Object, "mh4blend")
    if own_material:
        ops.register()
    if own_object:
        composite_authoring.register()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    try:
        yield
    finally:
        if own_object:
            composite_authoring.unregister()
        if own_material:
            ops.unregister()


def legacy(name):
    collection = bpy.data.collections.new(name)
    collection["type"] = "composit"
    collection["name"] = name
    return collection


def empty(name, collection, instance=None, parent=None):
    obj = bpy.data.objects.new(name, None)
    collection.objects.link(obj)
    if instance is not None:
        obj.instance_type = "COLLECTION"
        obj.instance_collection = instance
    obj.parent = parent
    return obj


def frozen(value):
    if hasattr(value, "items"):
        return tuple(sorted((key, frozen(item)) for key, item in value.items()))
    if isinstance(value, (tuple, list)) or hasattr(value, "to_list"):
        return tuple(frozen(item) for item in value)
    return value


def snapshot():
    def pointer(obj):
        return None if obj is None else obj.as_pointer()
    return (
        tuple((scene.name, scene.as_pointer()) for scene in bpy.data.scenes),
        tuple((col.name, col.as_pointer(), frozen(col),
               tuple(obj.as_pointer() for obj in col.objects),
               tuple(child.as_pointer() for child in col.children))
              for col in bpy.data.collections),
        tuple((obj.name, obj.as_pointer(), pointer(obj.parent),
               pointer(obj.instance_collection), frozen(obj),
               tuple(tuple(row) for row in obj.matrix_world))
              for obj in bpy.data.objects),
        bpy.context.scene.as_pointer(),
        tuple(obj.as_pointer() for obj in bpy.context.selected_objects),
        pointer(bpy.context.view_layer.objects.active),
    )


@pytest.mark.parametrize("mode", ["root_only", "composite_closure", "include_all"])
def test_direct_export_three_modes_do_not_create_or_modify_scene(tmp_path, mode):
    root = legacy("direct_root")
    empty("frame", root).location.x = 1
    external = empty("external", bpy.context.scene.collection, root)
    external.select_set(True)
    bpy.context.view_layer.objects.active = external
    before = snapshot()
    report = export_composite_closure_collection(
        root, tmp_path, source_root=tmp_path, mode=mode)
    assert snapshot() == before
    assert report["published"] == ["composite:direct_root"]
    assert report["mode"] == mode
    assert report["compatibility"]["unrecoverable"]
    assert (tmp_path / "direct_root.composite").read_bytes() == (
        composite_json_bytes(convert_dag4blend_collection(root)[0]))


def test_root_command_accepts_dag4blend_without_materializing(tmp_path):
    root = legacy("direct_root")
    empty("frame", root)
    before = snapshot()
    assert export_composite_collection(root, tmp_path, source_root=tmp_path)["ok"]
    assert snapshot() == before


@pytest.mark.parametrize("complete_mh_identity", [False, True])
def test_nested_dagor_collection_cannot_bypass_mixed_authority_gate(
        tmp_path, complete_mh_identity):
    root = legacy("direct_root")
    child = legacy("direct_child")
    child["mh_resource_kind"] = "composite"
    if complete_mh_identity:
        child["mh_resource_name"] = "direct_child"
    empty("nested", root, child)
    empty("frame", child)
    before = snapshot()
    with pytest.raises(ValueError, match="MH_E_INVALID_RESOURCE_SOURCE"):
        export_composite_closure_collection(
            root, tmp_path, source_root=tmp_path, mode="composite_closure")
    assert snapshot() == before
    assert not list(tmp_path.iterdir())


def test_saved_dagorprops_without_registered_rna_cannot_hide_p2(tmp_path):
    assert not hasattr(bpy.types.Object, "dagorprops")
    root = legacy("direct_root")
    empty("frame", root)["dagorprops"] = {"rot_y:p2": [0.0, 1.0]}
    before = snapshot()
    with pytest.raises(ValueError, match="rot_y:p2") as caught:
        export_composite_closure_collection(
            root, tmp_path, source_root=tmp_path, mode="include_all")
    assert "direct_root:nodes[0]" in str(caught.value)
    assert snapshot() == before
    assert not list(tmp_path.iterdir())


def test_saved_dagorprops_without_rna_keeps_weight_priority():
    assert not hasattr(bpy.types.Object, "dagorprops")
    root = legacy("direct_root")
    helper = bpy.data.collections.new("random.direct")
    empty("choice", root, helper)
    marker = legacy("loot_spawn_a")
    marker["type"] = "gameobj"
    option = empty("loot", helper, marker)
    option["dagorprops"] = {"weight:r": 7.0}
    option["weight:r"] = 2.0
    before = snapshot()
    doc, _ = convert_dag4blend_collection(root)
    assert doc.nodes[0].options[0].weight == 7.0
    assert snapshot() == before


def test_partial_publish_preserves_reached_drop_warnings(tmp_path):
    root = legacy("direct_root")
    child = legacy("direct_child")
    empty("nested", root, child)
    empty("frame", child)["label:t"] = "discarded"
    before = snapshot()

    def inject(event, item, _published):
        if event == "before_replace" and item.identity == "composite:direct_root":
            raise OSError("injected after labelled child")
    with pytest.raises(BatchPartialPublishError) as caught:
        export_composite_closure_collection(
            root, tmp_path, source_root=tmp_path, mode="composite_closure",
            _boundary_hook=inject)
    assert caught.value.published == ("composite:direct_child",)
    assert len(caught.value.warnings) == 1
    assert caught.value.warnings[0]["code"] == "MH_W_DAGOR_CONSTRUCT_DROPPED"
    assert caught.value.warnings[0]["node_path"] == "direct_child:nodes[0]"
    assert caught.value.compatibility["unrecoverable"]
    assert snapshot() == before


def test_adapter_does_not_allocate_missing_registered_property_groups():
    class DirectProbeDagorProperties(bpy.types.PropertyGroup):
        pass
    assert not hasattr(bpy.types.Object, "dagorprops")
    bpy.utils.register_class(DirectProbeDagorProperties)
    bpy.types.Object.dagorprops = bpy.props.PointerProperty(type=DirectProbeDagorProperties)
    try:
        root = legacy("direct_root")
        obj = empty("frame", root)
        assert "mh4blend" not in obj and "dagorprops" not in obj
        before = snapshot()
        convert_dag4blend_collection(root)
        assert snapshot() == before
    finally:
        del bpy.types.Object.dagorprops
        bpy.utils.unregister_class(DirectProbeDagorProperties)


@pytest.mark.parametrize("key,value", [("place_type:i", 3),
                                      ("ignoreParentInstSeed:b", True)])
def test_unratified_native_metadata_carrier_stops_before_staging(tmp_path, key, value):
    root = legacy("direct_root")
    empty("frame", root)[key] = value
    before = snapshot()
    with pytest.raises(ValueError, match="OPEN-V5-23") as caught:
        export_composite_closure_collection(
            root, tmp_path, source_root=tmp_path, mode="include_all")
    assert "direct_root:nodes[0]" in str(caught.value)
    assert key in str(caught.value)
    assert snapshot() == before
    assert not list(tmp_path.iterdir())


@pytest.mark.parametrize("construct", ["label:t", "require", "colors"])
def test_reached_dropped_construct_is_reported_with_node_path(tmp_path, construct):
    root = legacy("direct_root")
    empty("frame", root)[construct] = "do_not_execute"
    before = snapshot()
    report = export_composite_closure_collection(
        root, tmp_path, source_root=tmp_path, mode="include_all")
    assert snapshot() == before
    assert report["published"] == ["composite:direct_root"]
    assert len(report["warnings"]) == 1
    warning = report["warnings"][0]
    assert warning["code"] == "MH_W_DAGOR_CONSTRUCT_DROPPED"
    assert warning["node_path"] == "direct_root:nodes[0]"
    assert construct in warning["message"]
    assert construct.encode() not in (tmp_path / "direct_root.composite").read_bytes()


def test_prefab_conversion_requires_explicit_lossy_policy():
    root = legacy("direct_root")
    prefab = legacy("decor")
    prefab["type"] = "prefab"
    empty("frame", root, prefab)
    before = snapshot()
    with pytest.raises(ValueError, match="Allow Prefab as Mesh") as caught:
        convert_dag4blend_collection(root)
    assert "direct_root:nodes[0]" in str(caught.value)
    assert snapshot() == before


def test_explicit_prefab_policy_retains_mesh_identity_and_reports_loss():
    root = legacy("direct_root")
    prefab = legacy("decor")
    prefab["type"] = "prefab"
    empty("frame", root, prefab)
    before = snapshot()
    warnings = []
    doc, resources = convert_dag4blend_collection(
        root, allow_prefab_as_mesh_lossy=True, warnings=warnings)
    assert doc.nodes[0].kind == "mesh"
    assert doc.nodes[0].resource == "decor"
    assert resources == {("mesh", "decor"): prefab}
    assert len(warnings) == 1
    assert warnings[0]["code"] == "MH_W_DAGOR_CONSTRUCT_DROPPED"
    assert warnings[0]["node_path"] == "direct_root:nodes[0]"
    assert "collision/gameplay" in warnings[0]["message"]
    assert snapshot() == before


def test_marker_ordinary_and_option_match_native_mh_dto_without_resources(tmp_path):
    root = legacy("direct_root")
    marker = legacy("dummy_pivot")
    marker["type"] = "gameobj"
    empty("point", root, marker).location.x = 1
    helper = bpy.data.collections.new("random.direct")
    empty("choice", root, helper)
    option = legacy("loot_spawn_a")
    option["type"] = "gameobj"
    empty("loot", helper, option)["weight:r"] = 2
    empty("nothing", helper)["weight:r"] = 1
    before = snapshot()
    doc, _resources = convert_dag4blend_collection(root)
    assert snapshot() == before
    assert doc.nodes[0].kind == "marker"
    assert doc.nodes[1].options[0].kind == "marker"
    report = materialize_composite_documents(
        {doc.name: doc}, root_name=doc.name, source_root=tmp_path)
    assert _extract_composite(report["collection"]) == doc
    assert composite_json_bytes(_extract_composite(report["collection"])) == composite_json_bytes(doc)
    for obj in report["collection"].objects:
        if obj.mh4blend.kind == "marker":
            assert obj.instance_collection is None
            assert obj.get("mh_unresolved_placement") is None


def test_second_direct_json_export_reuses_source_without_replace(tmp_path):
    root = legacy("direct_root")
    empty("frame", root)
    export_composite_collection(root, tmp_path, source_root=tmp_path)
    before = snapshot()
    target = tmp_path / "direct_root.composite"
    previous = target.stat().st_mtime_ns
    events = []
    report = export_composite_closure_collection(
        root, tmp_path, source_root=tmp_path, mode="include_all",
        _boundary_hook=lambda *event: events.append(event))
    assert report["published"] == []
    assert report["reused"] == ["composite:direct_root"]
    assert events == []
    assert target.stat().st_mtime_ns == previous
    assert snapshot() == before


@pytest.mark.parametrize("markers", [
    {"mh_resource_kind": "composite"},
    {"mh_resource_name": "direct_root"},
    {"mh_resource_kind": "composite", "mh_resource_name": "direct_root"},
])
def test_mixed_or_partial_authority_rejected_before_any_write(tmp_path, markers):
    root = legacy("direct_root")
    for key, value in markers.items():
        root[key] = value
    before = snapshot()
    with pytest.raises(ValueError, match="MH_E_INVALID_RESOURCE_SOURCE") as error:
        export_composite_collection(root, tmp_path, source_root=tmp_path)
    assert root.name in str(error.value)
    assert all(key in str(error.value) for key in markers)
    assert list(tmp_path.iterdir()) == []
    assert snapshot() == before


@pytest.mark.parametrize("failed_key", ["composite:direct_child", "composite:direct_root"])
@pytest.mark.parametrize("boundary", ["before_replace", "after_replace"])
def test_direct_partial_boundary_and_retry_do_not_mutate_scene(
        tmp_path, failed_key, boundary):
    root = legacy("direct_root")
    child = legacy("direct_child")
    empty("nested", root, child)
    empty("frame", child)
    before = snapshot()

    def inject(event, item, _published):
        if event == boundary and item.identity == failed_key:
            raise OSError("injected direct boundary")
    with pytest.raises((OSError, BatchPartialPublishError)):
        export_composite_closure_collection(
            root, tmp_path, source_root=tmp_path, mode="composite_closure",
            _boundary_hook=inject)
    assert snapshot() == before
    if (tmp_path / "direct_root.composite").exists():
        assert (tmp_path / "direct_child.composite").exists()
    report = export_composite_closure_collection(
        root, tmp_path, source_root=tmp_path, mode="composite_closure")
    assert report["ok"]
    assert snapshot() == before
