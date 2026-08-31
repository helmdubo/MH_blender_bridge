"""R2 direct-export gates: adapters read scenes, publication writes files."""

from pathlib import Path
import sys

import pytest

bpy = pytest.importorskip("bpy")
from mathutils import Matrix  # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "addon"))
from mh4blend.scene.export_closure import export_composite_closure_collection
from mh4blend.scene.export_composite import export_composite_collection
from mh4blend.core.batch_publish import BatchPartialPublishError
from mh4blend.core.composites import composite_json_bytes
from mh4blend.core.composites import read_composite_file
from mh4blend.core.model import Composite, IDENTITY_TRANSFORM, Node, PlacementRange
from mh4blend.core.placements import read_placement_file
from mh4blend.core.validate import MHValidationError
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


def _carrier_scene(form):
    """One logical node with place_type 3 and a seed boundary, in both forms."""
    if form == "native":
        collection = bpy.data.collections.new("carrier_root")
        obj = empty("native_frame", collection)
        obj.mh4blend.place_type = 3
        obj.mh4blend.appearance_seed_boundary = True
        return collection
    collection = legacy("legacy_carrier_holder")
    collection["name"] = "carrier_root"
    obj = empty("legacy_frame", collection)
    obj["place_type:i"] = 3
    obj["ignoreParentInstSeed:b"] = True
    return collection


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


def test_saved_dagorprops_inline_p2_authors_node_placement_without_double_base(
        tmp_path):
    assert not hasattr(bpy.types.Object, "dagorprops")
    root = legacy("direct_root")
    first = empty("frame_a", root)
    second = empty("frame_b", root)
    for obj in (first, second):
        obj["dagorprops"] = {
            "rot_y:p2": [15.0, 1.0],
            "rot_z:p2": [-5.0, 2.0],
        }
        # dag4blend's matrix is a preview of p2 base values. It must never be
        # added to the inline placement a second time.
        obj.location = (91.0, 92.0, 93.0)
    before = snapshot()
    report = export_composite_closure_collection(
        root, tmp_path, source_root=tmp_path, mode="include_all")
    assert snapshot() == before

    # Owner revision of OPEN-V5-15 (2026-08-31): inline Dagor p2 stays inline
    # in the node itself; no derived external .placement resource exists.
    # Dagor is Y-up left-handed; placement-v1 axes are UE Z-up. The adapter
    # converts p2 axes exactly like node matrices (owner 2026-08-31, teapot
    # field defect): rot_x -> [0] base -x, rot_z -> [1] base +z,
    # rot_y (Dagor vertical) -> [2] base -y.
    document = read_composite_file(tmp_path / "direct_root.composite")
    assert document.nodes[0].profile is None
    assert document.nodes[1].profile is None
    assert document.nodes[0].placement == document.nodes[1].placement
    assert document.nodes[0].placement.rotation_deg == (
        PlacementRange(0.0, 0.0),
        PlacementRange(-5.0, 2.0),
        PlacementRange(-15.0, 1.0),
    )
    assert document.nodes[0].transform == IDENTITY_TRANSFORM
    assert document.nodes[1].transform == IDENTITY_TRANSFORM

    assert report["published"] == ["composite:direct_root"]
    assert not list(tmp_path.glob("*.placement"))

    repeated = export_composite_closure_collection(
        root, tmp_path, source_root=tmp_path, mode="include_all")
    assert repeated["published"] == []
    assert repeated["reused"] == ["composite:direct_root"]
    assert snapshot() == before


def test_inline_p2_preserves_all_ranges_and_fills_only_missing_axes(tmp_path):
    root = legacy("direct_root")
    node = empty("frame", root)
    node["dagorprops"] = {
        "offset_x:p2": [10.0, 2.0],
        "rot_z:p2": [-30.0, 5.0],
        "scale:p2": [1.25, 0.1],
        "yScale:p2": [0.75, 0.05],
    }
    before = snapshot()
    export_composite_closure_collection(
        root, tmp_path, source_root=tmp_path, mode="root_only")
    assert snapshot() == before

    document = read_composite_file(tmp_path / "direct_root.composite")
    assert document.nodes[0].profile is None
    assert not list(tmp_path.glob("*.placement"))
    profile = document.nodes[0].placement
    # Dagor authors offsets in meters along Y-up axes; placement-v1 stores
    # UE centimeters along Z-up axes: offset_x -> [0] x100,
    # offset_z -> [1] base negated x100, offset_y (vertical) -> [2] x100.
    assert profile.offset_cm == (
        PlacementRange(1000.0, 200.0),
        PlacementRange(0.0, 0.0),
        PlacementRange(0.0, 0.0),
    )
    assert profile.rotation_deg == (
        PlacementRange(0.0, 0.0),
        PlacementRange(-30.0, 5.0),
        PlacementRange(0.0, 0.0),
    )
    assert profile.uniform_scale.base == 1.25
    assert profile.uniform_scale.deviation == pytest.approx(0.1)
    assert profile.vertical_scale.base == 0.75
    assert profile.vertical_scale.deviation == pytest.approx(0.05)


def test_inline_p2_converts_every_dagor_axis_to_ue_space(tmp_path):
    root = legacy("direct_root")
    node = empty("frame", root)
    node["dagorprops"] = {
        "offset_x:p2": [1.0, 0.5],
        "offset_y:p2": [2.0, 0.25],
        "offset_z:p2": [3.0, 0.125],
        "rot_x:p2": [10.0, 1.0],
        "rot_y:p2": [20.0, 2.0],
        "rot_z:p2": [30.0, 3.0],
    }
    export_composite_closure_collection(
        root, tmp_path, source_root=tmp_path, mode="root_only")

    profile = read_composite_file(
        tmp_path / "direct_root.composite").nodes[0].placement
    # Conjugation by the Dagor(Y-up, LH) -> UE(Z-up) axis swap: X stays X,
    # Dagor Z becomes UE Y with a negated base, Dagor Y (vertical) becomes
    # UE Z; the reflection flips rotation direction for X and the vertical.
    assert profile.offset_cm == (
        PlacementRange(100.0, 50.0),
        PlacementRange(-300.0, 12.5),
        PlacementRange(200.0, 25.0),
    )
    assert profile.rotation_deg == (
        PlacementRange(-10.0, 1.0),
        PlacementRange(30.0, 3.0),
        PlacementRange(-20.0, 2.0),
    )


def test_inline_p2_signed_spread_normalizes_to_the_same_symmetric_range(
        tmp_path):
    root = legacy("direct_root")
    first = empty("negative_spread", root)
    second = empty("positive_spread", root)
    first["dagorprops"] = {"offset_x:p2": [0.0, -0.01]}
    second["dagorprops"] = {"offset_x:p2": [0.0, 0.01]}
    before = snapshot()
    report = export_composite_closure_collection(
        root, tmp_path, source_root=tmp_path, mode="include_all")
    assert snapshot() == before

    document = read_composite_file(tmp_path / "direct_root.composite")
    assert document.nodes[0].placement == document.nodes[1].placement
    profile = document.nodes[0].placement
    assert profile.offset_cm[0].base == 0.0
    # 0.01 Dagor meters -> 1 cm in placement-v1.
    assert profile.offset_cm[0].deviation == pytest.approx(1.0)
    assert not any(
        item.startswith("placement_profile:")
        for item in report["published"])
    assert not list(tmp_path.glob("*.placement"))


@pytest.mark.parametrize("properties", [
    {"rot_y:p2": [0.0]},
    {"scale:p2": [0.5, 0.5]},
    {"rot_y:p2": [0.0, float("inf")]},
])
def test_invalid_inline_p2_fails_before_any_publication(tmp_path, properties):
    root = legacy("direct_root")
    empty("frame", root)["dagorprops"] = properties
    before = snapshot()
    with pytest.raises(ValueError, match="MH_E_(PLACEMENT_PROFILE_GRAMMAR|NAN_INF_VALUE)"):
        export_composite_closure_collection(
            root, tmp_path, source_root=tmp_path, mode="include_all")
    assert snapshot() == before
    assert not list(tmp_path.iterdir())


def test_scene_include_without_typed_profile_remains_fail_closed(tmp_path):
    root = legacy("direct_root")
    empty("frame", root)["dagorprops"] = {"include:t": "scatter.blk"}
    before = snapshot()
    with pytest.raises(ValueError, match="include placement data"):
        export_composite_closure_collection(
            root, tmp_path, source_root=tmp_path, mode="include_all")
    assert snapshot() == before
    assert not list(tmp_path.iterdir())


def test_donor_scale_noise_is_canonicalized_with_warning(tmp_path):
    # Owner decision 2026-08-31 (revision of doc 10 §6.2 for the adapter
    # boundary only): Dagor matrices carry accumulated float noise, so scale
    # components that differ by less than 2e-4 relative are authored-equal
    # and snap to their mean. Downstream composed-world admission then stops
    # tripping over sub-ULP shear that no artist ever authored.
    root = legacy("direct_root")
    noisy = empty("noisy_frame", root)
    noisy.matrix_basis = (
        Matrix.Translation((1.0, 2.0, 3.0))
        @ Matrix.Diagonal((1.0212899, 1.0212300, 1.05, 1.0)))
    report = export_composite_closure_collection(
        root, tmp_path, source_root=tmp_path, mode="include_all")

    document = read_composite_file(tmp_path / "direct_root.composite")
    scale = document.nodes[0].transform.scale
    assert scale[0] == scale[1]
    assert scale[0] == pytest.approx(1.02126, rel=1e-5)
    assert scale[2] == pytest.approx(1.05)
    assert any(
        warning.get("code") == "MH_W_SCALE_NOISE_CANONICALIZED"
        for warning in report["warnings"]
        if isinstance(warning, dict))


def test_donor_sub_percent_anisotropy_snaps_to_uniform(tmp_path):
    # Owner decision 2026-08-31 (A3): a donor scale whose full spread stays
    # below 1e-2 relative is a Dagor size-fit, not meaningful anisotropy.
    # It snaps to one uniform value, so the composed world stays
    # TRS-representable under arbitrarily rotated descendants.
    root = legacy("direct_root")
    fitted = empty("fitted_frame", root)
    fitted.matrix_basis = Matrix.Diagonal((1.02127, 1.02127, 1.0266, 1.0))
    report = export_composite_closure_collection(
        root, tmp_path, source_root=tmp_path, mode="include_all")

    document = read_composite_file(tmp_path / "direct_root.composite")
    scale = document.nodes[0].transform.scale
    assert scale[0] == scale[1] == scale[2]
    assert scale[0] == pytest.approx(1.023047, rel=1e-5)
    assert any(
        warning.get("code") == "MH_W_SCALE_NOISE_CANONICALIZED"
        for warning in report["warnings"]
        if isinstance(warning, dict))


def test_authored_scale_anisotropy_is_never_touched(tmp_path):
    root = legacy("direct_root")
    authored = empty("authored_frame", root)
    authored.matrix_basis = Matrix.Diagonal((1.0, 1.02, 1.0, 1.0))
    report = export_composite_closure_collection(
        root, tmp_path, source_root=tmp_path, mode="include_all")

    document = read_composite_file(tmp_path / "direct_root.composite")
    scale = document.nodes[0].transform.scale
    assert scale[0] == pytest.approx(1.0)
    assert scale[1] == pytest.approx(1.02)
    assert scale[2] == pytest.approx(1.0)
    assert not any(
        warning.get("code") == "MH_W_SCALE_NOISE_CANONICALIZED"
        for warning in report["warnings"]
        if isinstance(warning, dict))


def test_inline_p2_reexport_ignores_foreign_placement_files(tmp_path):
    # The 2026-08-31 owner revision removed derived .placement resources, so
    # a foreign placement file in the output folder is not our resource and
    # must never be read, claimed, or overwritten by inline p2 publication.
    root = legacy("direct_root")
    empty("frame", root)["dagorprops"] = {"rot_y:p2": [0.0, 1.0]}
    foreign = tmp_path / "dagor_p2_deadbeefdeadbeef.placement"
    foreign.write_bytes(b"foreign bytes\n")
    before = snapshot()
    report = export_composite_closure_collection(
        root, tmp_path, source_root=tmp_path, mode="include_all")
    assert report["published"] == ["composite:direct_root"]
    assert foreign.read_bytes() == b"foreign bytes\n"
    document = read_composite_file(tmp_path / "direct_root.composite")
    assert document.nodes[0].placement is not None
    assert snapshot() == before


def test_saved_dagorprops_without_rna_keeps_weight_priority():
    assert not hasattr(bpy.types.Object, "dagorprops")
    root = legacy("direct_root")
    helper = bpy.data.collections.new("random.direct")
    empty("choice", root, helper)
    gameobj = legacy("loot_spawn_a")
    gameobj["type"] = "gameobj"
    option = empty("loot", helper, gameobj)
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


@pytest.mark.parametrize("carrier", ["id", "dagorprops"])
@pytest.mark.parametrize("properties,place_type,boundary", [
    ({}, None, False),
    ({"place_type:i": 3}, 3, False),
    ({"place_type:i": 0}, 0, False),
    ({"placeOnCollision:b": True}, 1, False),
    ({"placeOnCollision:b": True, "place_type:i": 4}, 4, False),
    ({"placeOnCollision:b": True, "place_type:i": 0}, 0, False),
    ({"placeOnCollision:b": False}, None, False),
    ({"ignoreParentInstSeed:b": True}, None, True),
    ({"ignoreParentInstSeed:b": False}, None, False),
    ({"useParentInstSeed:b": True}, None, False),
    ({"place_type:i": 3, "ignoreParentInstSeed:b": True}, 3, True),
    ({"place_type:i": 9}, 9, False),
])
def test_dagor_placement_and_seed_properties_map_into_the_dto(
        carrier, properties, place_type, boundary):
    root = legacy("direct_root")
    obj = empty("frame", root)
    if carrier == "dagorprops":
        obj["dagorprops"] = dict(properties)
    else:
        for key, value in properties.items():
            obj[key] = value
    before = snapshot()
    document, _resources = convert_dag4blend_collection(root)
    assert snapshot() == before
    assert document.nodes[0].place_type == place_type
    assert document.nodes[0].appearance_seed_boundary is boundary
    payload = composite_json_bytes(document)
    assert (b"place_type" in payload) is (place_type is not None)
    assert (b"appearance_seed_boundary" in payload) is boundary


@pytest.mark.parametrize("properties", [
    {"place_type:i": -1},
    {"place_type:i": 1.5},
    {"place_type:i": "3"},
    {"ignoreParentInstSeed:b": "yes"},
    {"useParentInstSeed:b": False},
    {"ignoreParentInstSeed:b": True, "useParentInstSeed:b": True},
])
def test_unrepresentable_dagor_placement_metadata_stays_fail_closed(
        tmp_path, properties):
    root = legacy("direct_root")
    obj = empty("frame", root)
    for key, value in properties.items():
        obj[key] = value
    before = snapshot()
    with pytest.raises(ValueError, match="MH_E_COMPOSITE_GRAMMAR") as caught:
        export_composite_closure_collection(
            root, tmp_path, source_root=tmp_path, mode="include_all")
    assert "direct_root:nodes[0]" in str(caught.value)
    assert any(key in str(caught.value) for key in properties)
    assert snapshot() == before
    assert not list(tmp_path.iterdir())


def test_saved_dagorprops_outrank_id_mirrors_for_placement_metadata():
    assert not hasattr(bpy.types.Object, "dagorprops")
    root = legacy("direct_root")
    obj = empty("frame", root)
    obj["dagorprops"] = {"place_type:i": 5, "ignoreParentInstSeed:b": True}
    obj["place_type:i"] = 2
    before = snapshot()
    document, _resources = convert_dag4blend_collection(root)
    assert snapshot() == before
    assert document.nodes[0].place_type == 5
    assert document.nodes[0].appearance_seed_boundary is True


def test_random_option_cannot_silently_drop_placement_metadata(tmp_path):
    root = legacy("direct_root")
    helper = bpy.data.collections.new("random.direct")
    empty("choice", root, helper)
    mesh = legacy("wheel")
    mesh["type"] = "rendInst"
    empty("ent", helper, mesh)["place_type:i"] = 3
    before = snapshot()
    with pytest.raises(ValueError, match="MH_E_COMPOSITE_GRAMMAR") as caught:
        export_composite_closure_collection(
            root, tmp_path, source_root=tmp_path, mode="include_all")
    assert "place_type:i" in str(caught.value)
    assert snapshot() == before


def test_compatibility_report_names_the_ratified_carriers_as_preserved(tmp_path):
    root = legacy("direct_root")
    empty("frame", root)["place_type:i"] = 3
    report = export_composite_closure_collection(
        root, tmp_path, source_root=tmp_path, mode="include_all")
    preserved = " ".join(report["compatibility"]["preserved"])
    assert "place_type" in preserved
    assert "ignoreParentInstSeed" in preserved
    assert not any("OPEN-V5-23" in item
                   for item in report["compatibility"]["blocked"])


def test_place_type_is_never_inherited_from_the_parent_node():
    """Owner 2026-08-29: place_type belongs to each node on its own.

    Every composite node, and the composite itself, carries its own place_type
    and it does NOT propagate. Doc 12 §2.9 one-level inheritance is retracted
    as mistaken, so absence on a child stays absence: ``None`` means the source
    never stated a value, and the adapter must not invent one from the parent.
    """

    root = legacy("direct_root")
    parent = empty("parent", root)
    parent["place_type:i"] = 3
    empty("child", root, parent=parent)
    before = snapshot()
    document, _resources = convert_dag4blend_collection(root)
    assert snapshot() == before

    parent_node = document.nodes[0]
    child_node = parent_node.children[0]
    assert parent_node.place_type == 3
    assert child_node.place_type is None
    payload = composite_json_bytes(document)
    # One occurrence only: the parent's. Nothing was materialized on the child.
    assert payload.count(b"place_type") == 1


def test_native_mh_carriers_round_trip_through_import_and_export(tmp_path):
    document, _resources = convert_dag4blend_collection(_carrier_scene("legacy"))
    report = materialize_composite_documents(
        {document.name: document}, root_name=document.name, source_root=tmp_path)
    restored = report["collection"].objects[0]
    assert restored.mh4blend.place_type == 3
    assert restored.mh4blend.appearance_seed_boundary is True
    assert _extract_composite(report["collection"]) == document


def test_native_mh_and_dag4blend_carrier_forms_are_byte_identical(tmp_path):
    native = _carrier_scene("native")
    legacy_form = _carrier_scene("legacy")
    before = snapshot()
    native_document = _extract_composite(native)
    legacy_document, _resources = convert_dag4blend_collection(legacy_form)
    assert snapshot() == before
    assert native_document == legacy_document
    assert (composite_json_bytes(native_document)
            == composite_json_bytes(legacy_document))
    assert b'"place_type": 3' in composite_json_bytes(native_document)
    assert b'"appearance_seed_boundary": true' in composite_json_bytes(
        native_document)


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


def test_gameobj_ordinary_and_option_match_native_mh_dto_without_resources(tmp_path):
    root = legacy("direct_root")
    gameobj = legacy("dummy_pivot")
    gameobj["type"] = "gameobj"
    empty("point", root, gameobj).location.x = 1
    helper = bpy.data.collections.new("random.direct")
    empty("choice", root, helper)
    option = legacy("loot_spawn_a")
    option["type"] = "gameobj"
    empty("loot", helper, option)["weight:r"] = 2
    empty("nothing", helper)["weight:r"] = 1
    before = snapshot()
    doc, _resources = convert_dag4blend_collection(root)
    assert snapshot() == before
    assert doc.nodes[0].kind == "gameobj"
    assert doc.nodes[1].options[0].kind == "gameobj"
    report = materialize_composite_documents(
        {doc.name: doc}, root_name=doc.name, source_root=tmp_path)
    assert _extract_composite(report["collection"]) == doc
    assert composite_json_bytes(_extract_composite(report["collection"])) == composite_json_bytes(doc)
    for obj in report["collection"].objects:
        if obj.mh4blend.kind == "gameobj":
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


# --- V5-S6.1.1 doc 15 2.5: the generic MH reader never interprets Dagor ---


def _native_root_instancing_dagor(dagor_type):
    """A native MH form whose placement instances a legacy dag4blend resource."""
    native = bpy.data.collections.new("native_root.composite")
    bpy.context.scene.collection.children.link(native)
    resource = bpy.data.collections.new("legacy_resource")
    resource["type"] = dagor_type
    resource["name"] = "legacy_resource"
    empty("placement", native, resource)
    return native, resource


@pytest.mark.parametrize("dagor_type", ["gameobj", "prefab"])
def test_native_mh_reader_refuses_dagor_identity_instead_of_translating_it(
        dagor_type):
    native, resource = _native_root_instancing_dagor(dagor_type)
    with pytest.raises(MHValidationError) as caught:
        _extract_composite(native)
    assert caught.value.code == "MH_E_INVALID_RESOURCE_SOURCE"
    assert "mixed scene representation" in caught.value.message
    assert resource.name in caught.value.subjects
    assert "type" in caught.value.subjects and "name" in caught.value.subjects


@pytest.mark.parametrize("dagor_type", ["gameobj", "prefab"])
def test_mixed_root_export_writes_nothing(tmp_path, dagor_type):
    native, _resource = _native_root_instancing_dagor(dagor_type)
    with pytest.raises(ValueError, match="MH_E_INVALID_RESOURCE_SOURCE") as caught:
        export_composite_collection(native, tmp_path, source_root=tmp_path)
    assert "mixed scene representation" in str(caught.value)
    assert list(tmp_path.iterdir()) == []
    # The native MH reader lazily allocates its typed carrier IDProperty group
    # on first read of mh4blend; that pre-existing artifact is not this gate's
    # subject. Compare the scene across a REPEATED refusal instead.
    before = snapshot()
    with pytest.raises(ValueError, match="MH_E_INVALID_RESOURCE_SOURCE"):
        export_composite_collection(native, tmp_path, source_root=tmp_path)
    assert list(tmp_path.iterdir()) == []
    assert snapshot() == before


def test_pure_dag4blend_root_still_reaches_the_dag4blend_adapter(tmp_path):
    """The dispatcher route must stay open: this is not a mixed scene."""
    root = legacy("direct_root")
    gameobj = legacy("dummy_pivot")
    gameobj["type"] = "gameobj"
    empty("point", root, gameobj)
    report = export_composite_closure_collection(
        root, tmp_path, source_root=tmp_path, mode="composite_closure")
    assert report["published"] == ["composite:direct_root"]
    document, _overrides = convert_dag4blend_collection(root)
    assert document.nodes[0].kind == "gameobj"


# --- V5-S6.1.1 doc 15 1.5/2.4: structural completeness admission ---


def _source_composite_file(directory, name):
    """A real, non-empty Source Root payload for one nested composite."""
    payload = composite_json_bytes(Composite(name, [Node("group")]))
    (directory / f"{name}.composite").write_bytes(payload)
    return payload


def _root_with_empty_nested(child_name="direct_child"):
    root = legacy("direct_root")
    child = legacy(child_name)
    assert not child.objects and not child.children
    empty("nested", root, child)
    return root, child


@pytest.mark.parametrize("mode", ["composite_closure", "include_all"])
def test_empty_nested_definition_reuses_the_real_source_payload(tmp_path, mode):
    root, _child = _root_with_empty_nested()
    payload = _source_composite_file(tmp_path, "direct_child")
    before = snapshot()
    report = export_composite_closure_collection(
        root, tmp_path, source_root=tmp_path, mode=mode)
    assert "composite:direct_child" in report["reused"]
    assert "composite:direct_child" not in report["published"]
    assert (tmp_path / "direct_child.composite").read_bytes() == payload
    assert snapshot() == before


@pytest.mark.parametrize("mode", ["composite_closure", "include_all"])
def test_empty_nested_definition_without_source_is_refused(tmp_path, mode):
    root, _child = _root_with_empty_nested()
    before = snapshot()
    with pytest.raises(ValueError, match="MH_E_INVALID_RESOURCE_SOURCE") as caught:
        export_composite_closure_collection(
            root, tmp_path, source_root=tmp_path, mode=mode)
    assert "direct_child" in str(caught.value)
    assert "collection is empty" in str(caught.value)
    assert "Recursive" in str(caught.value)
    assert list(tmp_path.iterdir()) == []
    assert snapshot() == before


def test_root_only_mode_keeps_its_source_root_admission(tmp_path):
    """root_only makes no scene-geometry demand; the source must still exist."""
    root, _child = _root_with_empty_nested()
    payload = _source_composite_file(tmp_path, "direct_child")
    report = export_composite_closure_collection(
        root, tmp_path, source_root=tmp_path, mode="root_only")
    assert report["published"] == ["composite:direct_root"]
    assert (tmp_path / "direct_child.composite").read_bytes() == payload


def test_all_empty_random_variants_are_not_an_empty_collection(tmp_path):
    """The legal authored case: every variant empty, but the node is present."""
    root = legacy("direct_root")
    child = legacy("direct_child")
    helper = bpy.data.collections.new("random.direct")
    empty("choice", child, helper)
    empty("nothing_a", helper)["weight:r"] = 1
    empty("nothing_b", helper)["weight:r"] = 2
    empty("nested", root, child)
    report = export_composite_closure_collection(
        root, tmp_path, source_root=tmp_path, mode="composite_closure")
    assert "composite:direct_child" in report["published"]


@pytest.mark.parametrize("mode", ["root_only", "composite_closure", "include_all"])
def test_empty_root_collection_is_always_refused(tmp_path, mode):
    root = legacy("direct_root")
    assert not root.objects and not root.children
    _source_composite_file(tmp_path, "direct_root")
    before = snapshot()
    with pytest.raises(ValueError, match="MH_E_INVALID_RESOURCE_SOURCE") as caught:
        export_composite_closure_collection(
            root, tmp_path, source_root=tmp_path, mode=mode)
    assert "direct_root" in str(caught.value)
    assert "collection is empty" in str(caught.value)
    assert snapshot() == before


# --- V5-S6.1.1 doc 15 2.6: save/reopen gate ---


def test_adapter_transforms_do_not_depend_on_view_layer_evaluation():
    """Object.matrix_local is a depsgraph mirror; the adapter must not read it.

    An unlinked definition Collection is never evaluated, so its matrix_local
    reports identity forever. Reading it would silently publish identity
    transforms and would change the exported bytes the moment the same scene
    were reopened with that Collection linked.
    """
    def build(name, link):
        root = legacy(name)
        if link:
            bpy.context.scene.collection.children.link(root)
        parent = empty(f"{name}_parent", root)
        parent.location = (1.0, 2.0, 3.0)
        child = empty(f"{name}_child", root, parent=parent)
        child.location = (0.0, 4.0, 0.0)
        return root, parent

    linked, linked_parent = build("linked_root", True)
    unlinked, unlinked_parent = build("unlinked_root", False)
    bpy.context.view_layer.update()
    assert linked_parent.matrix_local[0][3] == 1.0
    assert unlinked_parent.matrix_local[0][3] == 0.0

    linked_document, _overrides = convert_dag4blend_collection(linked)
    unlinked_document, _overrides = convert_dag4blend_collection(unlinked)
    assert linked_document.nodes[0].transform.translation_cm != (0.0, 0.0, 0.0)
    assert linked_document.nodes == unlinked_document.nodes


def test_dag4blend_export_is_byte_identical_across_save_and_reopen(tmp_path):
    root = legacy("direct_root")
    bpy.context.scene.collection.children.link(root)
    child = legacy("direct_child")
    bpy.context.scene.collection.children.link(child)
    empty("frame_a", root).location.x = 1
    empty("nested", root, child).location.y = 2
    empty("frame_b", root).location.z = 3
    empty("child_frame", child)
    helper = bpy.data.collections.new("random.direct")
    empty("choice", child, helper).location.x = 4
    option_a = legacy("variant_a")
    option_a["type"] = "gameobj"
    option_b = legacy("variant_b")
    option_b["type"] = "gameobj"
    empty("pick_a", helper, option_a)["weight:r"] = 3.0
    empty("pick_b", helper, option_b)["weight:r"] = 1.0
    empty("pick_nothing", helper)["weight:r"] = 2.0

    first_dir = tmp_path / "first"
    first_dir.mkdir()
    export_composite_closure_collection(
        root, first_dir, source_root=first_dir, mode="composite_closure")
    before_documents = {
        path.name: path.read_bytes() for path in sorted(first_dir.iterdir())}
    before_dto, _overrides = convert_dag4blend_collection(root)

    blend = tmp_path / "reopen.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(blend))
    bpy.ops.wm.open_mainfile(filepath=str(blend))

    reopened = bpy.data.collections["direct_root"]
    assert reopened.get("type") == "composit"
    second_dir = tmp_path / "second"
    second_dir.mkdir()
    export_composite_closure_collection(
        reopened, second_dir, source_root=second_dir, mode="composite_closure")
    after_documents = {
        path.name: path.read_bytes() for path in sorted(second_dir.iterdir())}
    after_dto, _overrides = convert_dag4blend_collection(reopened)

    assert after_documents == before_documents
    assert list(after_documents) == list(before_documents)
    assert after_dto == before_dto
