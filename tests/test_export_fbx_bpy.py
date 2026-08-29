"""Blender gates for the Source Protocol v4 FBX writer."""

import importlib
from pathlib import Path
import sys

import pytest

bpy = pytest.importorskip("bpy")
REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.validate import MHValidationError  # noqa: E402
from mh4blend.scene.export_fbx import (  # noqa: E402
    export_fbx_collection,
    prepare_fbx_collection,
    stage_prepared_fbx,
)
from mh4blend.scene.import_fbx import parse_mesh_fbx  # noqa: E402
from mh4blend.scene.resource_markers import (  # noqa: E402
    COLLECTION_KIND_KEY,
    COLLECTION_RESOURCE_KEY,
    INCOMPLETE_IMPORT_KEY,
)

export_fbx_module = importlib.import_module("mh4blend.scene.export_fbx")


def _mesh_object(name, collection):
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    return obj


def _empty(name, collection):
    obj = bpy.data.objects.new(name, None)
    collection.objects.link(obj)
    return obj


def _material(name):
    return bpy.data.materials.new(name)


def _assign_material(obj, material):
    obj.data.materials.append(material)


def _collection(name="selected_resource"):
    collection = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(collection)
    return collection


def _build_joined(name="selected_resource"):
    root = _collection(name)
    child = bpy.data.collections.new("Child")
    root.children.link(child)
    direct = _mesh_object("Direct", root)
    nested = _mesh_object("Nested", child)
    return root, direct, nested


def _build_lods(base="garage"):
    root = _collection(base + ".lods")
    lod0 = bpy.data.collections.new(base + ".lod00")
    lod1 = bpy.data.collections.new(base + ".lod01")
    root.children.link(lod0)
    root.children.link(lod1)
    render0 = _mesh_object("Body", lod0)
    render1a = _mesh_object("BodyA", lod1)
    render1b = _mesh_object("BodyB_lod01", lod1)
    collision = _mesh_object("Body_cls_phys", lod0)
    ucx_collision = _mesh_object("UCX_Body", lod0)
    socket = _empty("SOCKET_Muzzle", lod0)
    ignored_collision = _mesh_object("UCX_Body_High", lod1)
    ignored_socket = _empty("SOCKET_High", lod1)
    return {
        "root": root,
        "render": (render0, render1a, render1b),
        "collision": collision,
        "ucx_collision": ucx_collision,
        "socket": socket,
        "ignored": (ignored_collision, ignored_socket),
    }


def test_incomplete_import_cannot_overwrite_mesh_source(tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = _collection("incomplete_mesh")
    _mesh_object("body", collection)
    collection[INCOMPLETE_IMPORT_KEY] = True
    target = tmp_path / "incomplete_mesh.mesh.fbx"
    target.write_bytes(b"existing-authority")
    calls = []
    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx",
        lambda filepath: calls.append(filepath))

    with pytest.raises(
            MHValidationError, match="MH_E_INVALID_RESOURCE_SOURCE"):
        prepare_fbx_collection(collection, tmp_path, source_root=tmp_path)

    with pytest.raises(
            MHValidationError, match="MH_E_INVALID_RESOURCE_SOURCE"):
        export_fbx_collection(collection, tmp_path, source_root=tmp_path)

    assert target.read_bytes() == b"existing-authority"
    assert calls == []
    assert sorted(path.name for path in tmp_path.iterdir()) == [target.name]


def test_prepare_and_stage_leave_source_target_unchanged(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, _direct, _nested = _build_joined("closure_mesh")
    source_dir = tmp_path / "source"
    source_dir.mkdir()
    target = source_dir / "closure_mesh.mesh.fbx"
    target.write_bytes(b"existing-source-authority")

    prepared = prepare_fbx_collection(
        collection, source_dir, source_root=source_dir)
    assert prepared.target == target
    assert target.read_bytes() == b"existing-source-authority"
    assert COLLECTION_KIND_KEY not in collection
    assert COLLECTION_RESOURCE_KEY not in collection

    stage_dir = tmp_path / "stage"
    stage_dir.mkdir()
    staged = stage_prepared_fbx(
        prepared, stage_dir / "closure_mesh.mesh.fbx")

    assert target.read_bytes() == b"existing-source-authority"
    assert COLLECTION_KIND_KEY not in collection
    assert COLLECTION_RESOURCE_KEY not in collection
    assert staged.filepath.read_bytes() == staged.payload
    assert len(staged.payload) > 0
    assert parse_mesh_fbx(staged.filepath).resource_name == "closure_mesh"


def test_stage_failure_restores_blender_state_and_cleans_payload(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    built = _build_lods("closure_restore")
    source_dir = tmp_path / "source"
    source_dir.mkdir()
    target = source_dir / "closure_restore.mesh.fbx"
    target.write_bytes(b"existing-source-authority")
    stage_dir = tmp_path / "stage"
    stage_dir.mkdir()
    stage_path = stage_dir / target.name

    active = built["render"][1]
    active.select_set(True)
    bpy.context.view_layer.objects.active = active
    built["render"][0].hide_select = True
    built["render"][0].location = (1.25, -2.5, 3.75)
    original_names = tuple(obj.name for obj in built["render"])
    original_data = tuple(obj.data for obj in built["render"])
    original_matrices = tuple(
        obj.matrix_basis.copy() for obj in built["render"])
    original_units = (
        bpy.context.scene.unit_settings.system,
        bpy.context.scene.unit_settings.scale_length,
        bpy.context.scene.unit_settings.length_unit,
    )
    prepared = prepare_fbx_collection(
        built["root"], source_dir, source_root=source_dir)

    def fail_after_partial_write(filepath):
        Path(filepath).write_bytes(b"partial-stage")
        raise RuntimeError("synthetic stage failure")

    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx", fail_after_partial_write)
    with pytest.raises(RuntimeError, match="synthetic stage failure"):
        stage_prepared_fbx(prepared, stage_path)

    assert target.read_bytes() == b"existing-source-authority"
    assert not stage_path.exists()
    assert tuple(obj.name for obj in built["render"]) == original_names
    assert tuple(obj.data for obj in built["render"]) == original_data
    assert tuple(obj.matrix_basis for obj in built["render"]) == original_matrices
    assert (
        bpy.context.scene.unit_settings.system,
        bpy.context.scene.unit_settings.scale_length,
        bpy.context.scene.unit_settings.length_unit,
    ) == original_units
    assert built["render"][0].hide_select is True
    assert bpy.context.view_layer.objects.active is active
    assert active.select_get() is True


def test_empty_stage_readback_is_rejected_and_removed(tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, _direct, _nested = _build_joined("empty_readback")
    source_dir = tmp_path / "source"
    source_dir.mkdir()
    target = source_dir / "empty_readback.mesh.fbx"
    target.write_bytes(b"existing-source-authority")
    stage_dir = tmp_path / "stage"
    stage_dir.mkdir()
    stage_path = stage_dir / target.name
    prepared = prepare_fbx_collection(
        collection, source_dir, source_root=source_dir)
    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx",
        lambda filepath: Path(filepath).write_bytes(b""))

    with pytest.raises(RuntimeError, match="read-back is empty"):
        stage_prepared_fbx(prepared, stage_path)

    assert target.read_bytes() == b"existing-source-authority"
    assert not stage_path.exists()


def test_non_fbx_stage_readback_is_rejected_and_removed(tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, _direct, _nested = _build_joined("junk_readback")
    source_dir = tmp_path / "source"
    source_dir.mkdir()
    target = source_dir / "junk_readback.mesh.fbx"
    target.write_bytes(b"existing-source-authority")
    stage_dir = tmp_path / "stage"
    stage_dir.mkdir()
    stage_path = stage_dir / target.name
    prepared = prepare_fbx_collection(
        collection, source_dir, source_root=source_dir)
    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx",
        lambda filepath: Path(filepath).write_bytes(b"not-an-fbx"))

    with pytest.raises(
            RuntimeError, match="structural read-back validation"):
        stage_prepared_fbx(prepared, stage_path)

    assert target.read_bytes() == b"existing-source-authority"
    assert not stage_path.exists()


def test_stage_rejects_dependency_change_after_prepare(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, direct, _nested = _build_joined("changed_after_preflight")
    source_dir = tmp_path / "source"
    source_dir.mkdir()
    stage_dir = tmp_path / "stage"
    stage_dir.mkdir()
    prepared = prepare_fbx_collection(
        collection, source_dir, source_root=source_dir)

    _assign_material(direct, _material("late_material"))
    with pytest.raises(
            MHValidationError, match="changed after preflight"):
        stage_prepared_fbx(
            prepared, stage_dir / "changed_after_preflight.mesh.fbx")

    assert list(stage_dir.iterdir()) == []
    assert not (source_dir / "changed_after_preflight.mesh.fbx").exists()


def test_collision_material_slots_are_writer_validated_and_reader_compatible(
        tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = _collection("vehicle")
    _mesh_object("body", collection)
    collision = _mesh_object("UCX_body", collection)
    invalid = _material("Wall")
    _assign_material(collision, invalid)

    with pytest.raises(
            MHValidationError, match="MH_E_NONCANONICAL_RESOURCE_NAME"):
        export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert not (tmp_path / "vehicle.mesh.fbx").exists()

    invalid.name = "wall"
    report = export_fbx_collection(
        collection, tmp_path, source_root=tmp_path)
    plan = parse_mesh_fbx(report["filepath"])
    collision_node = next(
        node for node in plan.nodes if node.name == "UCX_body")
    assert collision_node.material_slots == ("wall",)


def _fbx_models(path):
    from io_scene_fbx import parse_fbx
    root, _version = parse_fbx.parse(str(path))
    objects = next(item for item in root.elems if item.id == b"Objects")
    result = {}
    for model in objects.elems:
        if model.id != b"Model":
            continue
        name = model.props[1].split(b"\x00", 1)[0].decode("utf-8")
        props = {}
        props70 = next(
            (item for item in model.elems if item.id == b"Properties70"), None)
        if props70 is not None:
            for item in props70.elems:
                if item.id == b"P" and len(item.props) >= 5:
                    props[item.props[0].decode("utf-8")] = item.props[4]
        result[name] = {"kind": model.props[2], "properties": props}
    return result


def _fbx_model_parents(path):
    from io_scene_fbx import parse_fbx
    root, _version = parse_fbx.parse(str(path))
    objects = next(item for item in root.elems if item.id == b"Objects")
    names = {model.props[0]: model.props[1].split(b"\x00", 1)[0].decode("utf-8")
             for model in objects.elems if model.id == b"Model"}
    connections = next(item for item in root.elems if item.id == b"Connections")
    parents = {name: None for name in names.values()}
    for link in connections.elems:
        if (link.id == b"C" and len(link.props) >= 3
                and link.props[0] == b"OO"):
            child_id, parent_id = link.props[1], link.props[2]
            if child_id in names and parent_id in names:
                parents[names[child_id]] = names[parent_id]
    return parents


def test_export_rewrites_plain_fbx_without_custom_properties(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, direct, nested = _build_joined()
    direct["mh_old_property"] = "artist value"
    first = export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    second = export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert Path(first["filepath"]).name == "selected_resource.mesh.fbx"
    assert first["written"] is second["written"] is True
    assert collection[COLLECTION_KIND_KEY] == "mesh"
    assert collection[COLLECTION_RESOURCE_KEY] == "selected_resource"
    assert direct["mh_old_property"] == "artist value"
    models = _fbx_models(second["filepath"])
    assert {"Direct", "Nested"} <= set(models)
    assert all(not key.startswith("mh_")
               for row in models.values() for key in row["properties"])
    assert nested.name == "Nested"


def test_existing_file_is_replaced_but_directory_is_blocked(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, _direct, _nested = _build_joined("replace_me")
    target = tmp_path / "replace_me.mesh.fbx"
    target.write_bytes(b"old")
    export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert target.read_bytes() != b"old"
    target.unlink()
    target.mkdir()
    with pytest.raises(ValueError, match="exists as a directory"):
        export_fbx_collection(collection, tmp_path, source_root=tmp_path)


@pytest.mark.parametrize(
    "name", ["Garage", "garage a", "garage.a", "гараж"])
def test_export_rejects_noncanonical_logical_name(tmp_path, name):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, _direct, _nested = _build_joined(name)
    with pytest.raises(ValueError, match="MH_E_NONCANONICAL_RESOURCE_NAME"):
        export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert not list(tmp_path.glob("*.mesh.fbx"))


def test_noncanonical_material_slot_preserves_resource_name_code(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, direct, _nested = _build_joined("canonical_mesh")
    _assign_material(direct, _material("Wall"))
    with pytest.raises(MHValidationError) as excinfo:
        export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert excinfo.value.code == "MH_E_NONCANONICAL_RESOURCE_NAME"
    assert not (tmp_path / "canonical_mesh.mesh.fbx").exists()


def test_lod_mesh_names_are_temporary_and_classifiable(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    built = _build_lods()
    original_names = [obj.name for obj in built["render"]]
    report = export_fbx_collection(built["root"], tmp_path, source_root=tmp_path)
    models = _fbx_models(report["filepath"])
    assert report["lod_levels"] == [0, 1]
    assert {"Body_lod00", "BodyA_lod01", "BodyB_lod01"} <= set(models)
    # docs/15 §2.2: recognized Dagor collision leaves the render payload;
    # UCX_ stays because it is the native UE collision transport, not a
    # Dagor `cls` construct.
    assert "Body_cls_phys" not in models
    assert "UCX_Body" in models
    assert "SOCKET_Muzzle" in models
    assert "UCX_Body_High" not in models
    assert "SOCKET_High" not in models
    assert [obj.name for obj in built["render"]] == original_names


def test_mismatched_existing_lod_suffix_fails_closed(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    built = _build_lods("mismatch")
    built["render"][0].name = "Body_lod01"
    with pytest.raises(MHValidationError) as excinfo:
        export_fbx_collection(built["root"], tmp_path, source_root=tmp_path)
    assert excinfo.value.code == "MH_E_INVALID_LOD_HIERARCHY"


def test_lod_names_restore_when_exporter_raises(tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    built = _build_lods("atomic")
    original_names = [obj.name for obj in built["render"]]
    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx",
        lambda _path: (_ for _ in ()).throw(RuntimeError("synthetic failure")))
    with pytest.raises(RuntimeError, match="synthetic failure"):
        export_fbx_collection(built["root"], tmp_path, source_root=tmp_path)
    assert [obj.name for obj in built["render"]] == original_names
    assert not (tmp_path / "atomic.mesh.fbx").exists()
    assert not list(tmp_path.glob(".atomic.mesh.fbx.mh-tmp-*"))


def test_group_empty_hierarchy_is_transported(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, direct, nested = _build_joined("grouped_resource")
    group = _empty("GroupNode", collection)
    direct.parent = group
    nested.parent = group
    result = export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    models = _fbx_models(result["filepath"])
    parents = _fbx_model_parents(result["filepath"])
    assert models["GroupNode"]["kind"] == b"Null"
    assert parents["Direct"] == "GroupNode"
    assert parents["Nested"] == "GroupNode"


def test_lods_container_group_empty_is_transported(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    built = _build_lods("depot")
    group = _empty("Rig", built["root"].children[0])
    built["render"][0].parent = group

    result = export_fbx_collection(
        built["root"], tmp_path, source_root=tmp_path)
    models = _fbx_models(result["filepath"])
    parents = _fbx_model_parents(result["filepath"])
    assert models["Rig"]["kind"] == b"Null"
    assert parents["Body_lod00"] == "Rig"


def test_lod_material_union_replaces_the_lod0_subset_rule(tmp_path):
    """Retired behavior: MH_E_LOD_SLOT_NOT_IN_BASE is never raised again.

    Owner decision 2026-08-30 (docs/15 §1.1/§2.1). The published material list
    is the ordered union of every authored LOD, LOD-major by first appearance.
    """
    bpy.ops.wm.read_factory_settings(use_empty=True)
    built = _build_lods("slots")
    base = _material("base")
    _assign_material(built["render"][0], base)
    _assign_material(built["render"][1], _material("only_higher"))
    _assign_material(built["render"][2], base)
    report = export_fbx_collection(
        built["root"], tmp_path, source_root=tmp_path)
    assert report["materials"] == ["base", "only_higher"]
    plan = parse_mesh_fbx(report["filepath"])
    assert plan.material_names == ("base", "only_higher")


def test_material_union_order_is_lod_major_first_appearance(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    built = _build_lods("union_order")
    glass = _material("glass")
    paint_low = _material("paint_low")
    _assign_material(built["render"][0], _material("paint"))
    _assign_material(built["render"][0], glass)
    _assign_material(built["render"][1], glass)
    _assign_material(built["render"][1], paint_low)
    _assign_material(built["render"][2], _material("rubber"))
    report = export_fbx_collection(
        built["root"], tmp_path, source_root=tmp_path)
    plan = parse_mesh_fbx(report["filepath"])
    assert plan.material_names == (
        "paint", "glass", "paint_low", "rubber")
    assert report["materials"] == list(plan.material_names)


def test_centimeter_context_preserves_exact_mesh_datablock(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, direct, _nested = _build_joined("exact_restore")
    direct.data.vertices[0].co = (1.0e-7, 123456789.0, -0.333333333)
    direct.shape_key_add(name="Basis")
    key = direct.shape_key_add(name="Offset")
    key.data[0].co = (2.0e-7, 123456700.0, -0.666666667)

    original_mesh = direct.data
    vertex_coordinates = [vertex.co.copy() for vertex in original_mesh.vertices]
    shape_coordinates = {
        block.name: [point.co.copy() for point in block.data]
        for block in original_mesh.shape_keys.key_blocks
    }

    export_fbx_collection(collection, tmp_path, source_root=tmp_path)

    assert direct.data is original_mesh
    assert [vertex.co.copy() for vertex in original_mesh.vertices] == vertex_coordinates
    assert {
        block.name: [point.co.copy() for point in block.data]
        for block in original_mesh.shape_keys.key_blocks
    } == shape_coordinates


def test_parent_outside_resource_fails_closed(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, direct, _nested = _build_joined("escaping_resource")
    outsider = bpy.data.objects.new("Outsider", None)
    bpy.context.scene.collection.objects.link(outsider)
    direct.parent = outsider
    with pytest.raises(MHValidationError) as exc:
        export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert exc.value.code == "MH_E_PARENT_OUTSIDE_RESOURCE"


def test_shift_d_duplicate_exports_without_identity_warnings(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, direct, _nested = _build_joined("duplicated_resource")
    duplicate = direct.copy()
    duplicate.data = direct.data.copy()
    collection.objects.link(duplicate)
    result = export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert result["ok"] is True
    assert result["validation"]["warnings"] == []
    assert len([name for name in _fbx_models(result["filepath"])
                if name.startswith("Direct")]) == 2


def test_linked_duplicate_geometry_is_rejected_before_fbx_write(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, direct, _nested = _build_joined("linked_duplicate")
    linked = direct.copy()
    linked.name = "DirectLinked"
    collection.objects.link(linked)

    with pytest.raises(MHValidationError) as exc:
        export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert exc.value.code == "MH_E_UNSUPPORTED_NODE_KIND"
    assert not (tmp_path / "linked_duplicate.mesh.fbx").exists()


@pytest.mark.parametrize(
    "name",
    ["UCX_body_cls_both", "UCX_body_lod00", "SOCKET_render"],
)
def test_export_rejects_conflicting_mesh_node_markers(tmp_path, name):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = _collection("invalid_markers")
    _mesh_object(name, collection)
    with pytest.raises(MHValidationError) as exc:
        export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert exc.value.code == "MH_E_INVALID_NODE_MARKERS"
    assert not (tmp_path / "invalid_markers.mesh.fbx").exists()


def test_export_rejects_socket_with_children(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = _collection("invalid_socket")
    socket = _empty("SOCKET_root", collection)
    mesh = _mesh_object("body", collection)
    mesh.parent = socket
    with pytest.raises(MHValidationError) as exc:
        export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert exc.value.code == "MH_E_INVALID_NODE_MARKERS"


@pytest.fixture
def registered_material_properties():
    from mh4blend.ui import ops

    owned = not hasattr(bpy.types.Material, "mh4blend")
    if owned:
        ops.register()
    try:
        yield
    finally:
        if owned:
            ops.unregister()


def _warning_codes(report):
    return [row["code"] for row in report["validation"]["warnings"]]


def _warning_message(report, code):
    return next(row["message"] for row in report["validation"]["warnings"]
                if row["code"] == code)


@pytest.mark.parametrize("name", [
    "hull_cls_phys", "hull_cls_trace", "hull_cls_both",
    "gaz53_a_body.lod01 cls phys.001", "gaz53_a_bumper.lod01 cls.002",
])
def test_dagor_collision_nodes_leave_the_render_payload(tmp_path, name):
    """docs/15 §1.3/§2.2: recognized collision is excluded, never transported.

    Transport of collision geometry is V5-S6.1.2; this slice only has to stop
    silently shipping it as render payload.
    """
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = _collection("collision_free")
    _mesh_object("body", collection)
    _mesh_object(name, collection)
    report = export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    models = _fbx_models(report["filepath"])
    assert "body" in models
    assert name not in models
    assert "MH_W_DAGOR_CONSTRUCT_DROPPED" in _warning_codes(report)
    assert name in _warning_message(report, "MH_W_DAGOR_CONSTRUCT_DROPPED")


def test_cls_material_mesh_is_excluded_from_payload_and_closure(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = _collection("technical_free")
    body = _mesh_object("body", collection)
    _assign_material(body, _material("paint"))
    # Real dag4blend content names collision meshes arbitrarily; the `cls`
    # material is the only reliable marker.
    technical = _mesh_object("Cube.774", collection)
    _assign_material(technical, _material("cls"))

    report = export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    models = _fbx_models(report["filepath"])
    assert "Cube.774" not in models
    assert report["materials"] == ["paint"]
    plan = parse_mesh_fbx(report["filepath"])
    assert plan.material_names == ("paint",)
    assert "cls" in _warning_message(report, "MH_W_DAGOR_CONSTRUCT_DROPPED")


def test_gi_black_material_mesh_is_excluded(tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = _collection("gi_black_free")
    body = _mesh_object("body", collection)
    _assign_material(body, _material("paint"))
    technical = _mesh_object("Cube.775", collection)
    _assign_material(technical, _material("technical_shell"))

    import mh4blend.scene.export_material as export_material_module
    from types import SimpleNamespace

    monkeypatch.setattr(
        export_material_module, "_authored_dagormat",
        lambda material: (
            SimpleNamespace(shader_class="gi_black")
            if material.name == "technical_shell"
            else SimpleNamespace(shader_class="rendinst_simple")))

    report = export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert "Cube.775" not in _fbx_models(report["filepath"])
    assert report["materials"] == ["paint"]


def test_mesh_mixing_technical_and_render_slots_fails_closed(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = _collection("mixed_slots")
    body = _mesh_object("body", collection)
    _assign_material(body, _material("paint"))
    _assign_material(body, _material("cls"))
    with pytest.raises(MHValidationError) as exc:
        export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert exc.value.code == "MH_E_MATERIAL_SLOT_CONFLICT"


def test_equivalent_duplicate_material_merges_into_the_base_name(
        tmp_path, registered_material_properties):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = _collection("merged_material")
    base = _material("paint")
    base.mh4blend.material_class = "rendinst_simple"
    duplicate = _material("paint.001")
    duplicate.mh4blend.material_class = "rendinst_simple"
    first = _mesh_object("front", collection)
    second = _mesh_object("rear", collection)
    _assign_material(first, base)
    _assign_material(second, duplicate)

    report = export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert report["materials"] == ["paint"]
    plan = parse_mesh_fbx(report["filepath"])
    assert plan.material_names == ("paint",)
    # The scene keeps both authored datablocks and their authored names.
    assert duplicate.name == "paint.001"
    assert [slot.material for slot in second.material_slots] == [duplicate]


def test_duplicate_material_without_base_uses_the_unsuffixed_name(
        tmp_path, registered_material_properties):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = _collection("suffix_only_material")
    duplicate = _material("gaz53_tiled_wood_b.001")
    duplicate.mh4blend.material_class = "rendinst_perlin_layered"
    body = _mesh_object("body", collection)
    _assign_material(body, duplicate)

    report = export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert report["materials"] == ["gaz53_tiled_wood_b"]
    plan = parse_mesh_fbx(report["filepath"])
    assert plan.material_names == ("gaz53_tiled_wood_b",)
    assert duplicate.name == "gaz53_tiled_wood_b.001"


def test_divergent_duplicate_material_is_ambiguous(
        tmp_path, registered_material_properties):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = _collection("ambiguous_material")
    base = _material("paint")
    base.mh4blend.material_class = "rendinst_simple"
    duplicate = _material("paint.001")
    duplicate.mh4blend.material_class = "rendinst_mask_layered"
    body = _mesh_object("body", collection)
    _assign_material(body, duplicate)

    with pytest.raises(MHValidationError) as exc:
        export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert exc.value.code == "MH_E_AMBIGUOUS_RESOURCE_NAME"
    assert exc.value.subjects == ["paint", "paint.001"]
    # The refusal has to name the field that actually diverges: real content
    # hides the divergence in one Dagor parameter out of a dozen.
    assert "class 'rendinst_mask_layered' vs 'rendinst_simple'" in str(
        exc.value)
    assert not (tmp_path / "ambiguous_material.mesh.fbx").exists()


def test_divergent_duplicate_blocks_even_when_only_the_base_is_transported(
        tmp_path, registered_material_properties):
    """`<name>.material` is one file, so the conflict is file-wide.

    The exported mesh only uses `paint`, but publishing `paint.material` while
    a second datablock claims that logical name would hand the artist a file
    that is wrong for the other mesh.
    """
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = _collection("base_only_mesh")
    base = _material("paint")
    base.mh4blend.material_class = "rendinst_simple"
    elsewhere = _material("paint.001")
    elsewhere.mh4blend.material_class = "rendinst_mask_layered"
    _assign_material(_mesh_object("body", collection), base)

    with pytest.raises(MHValidationError) as exc:
        export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert exc.value.code == "MH_E_AMBIGUOUS_RESOURCE_NAME"
    assert exc.value.subjects == ["paint", "paint.001"]


def test_divergent_duplicate_names_the_diverging_dagor_parameter(
        tmp_path, registered_material_properties):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = _collection("ambiguous_parameter")
    base = _material("paint")
    base.mh4blend.material_class = "rendinst_mask_layered"
    base_row = base.mh4blend.params.add()
    base_row.name = "paint_details"
    base_row.kind = "VECTOR"
    base_row.vector = (0.9, 0.0, 0.0, 94.0)
    duplicate = _material("paint.001")
    duplicate.mh4blend.material_class = "rendinst_mask_layered"
    duplicate_row = duplicate.mh4blend.params.add()
    duplicate_row.name = "paint_details"
    duplicate_row.kind = "VECTOR"
    duplicate_row.vector = (0.6, 0.0, 0.0, 94.0)
    _assign_material(_mesh_object("body", collection), duplicate)

    with pytest.raises(MHValidationError) as exc:
        export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert exc.value.code == "MH_E_AMBIGUOUS_RESOURCE_NAME"
    assert "params.paint_details" in str(exc.value)


def test_export_rejects_socket_with_child_outside_resource(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = _collection("invalid_external_socket_child")
    _mesh_object("body", collection)
    socket = _empty("SOCKET_root", collection)
    external = _collection("external")
    child = _mesh_object("external_child", external)
    child.parent = socket
    with pytest.raises(MHValidationError) as exc:
        export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert exc.value.code == "MH_E_INVALID_NODE_MARKERS"
    assert not (tmp_path / "invalid_external_socket_child.mesh.fbx").exists()
