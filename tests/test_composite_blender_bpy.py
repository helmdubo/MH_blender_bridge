"""Blender gates for v5 parent-local Composite scene adapters."""

import math
from pathlib import Path
import sys

import pytest

bpy = pytest.importorskip("bpy")
from mathutils import Matrix
REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.composites import composite_json_bytes, parse_composite  # noqa: E402
from mh4blend.core.model import Composite, CompositeTransform, Node  # noqa: E402
from mh4blend.core.validate import MHValidationError  # noqa: E402
from mh4blend.scene import import_composite as import_module  # noqa: E402
from mh4blend.scene.export_composite import (  # noqa: E402
    COLLECTION_KIND_KEY,
    COLLECTION_RESOURCE_KEY,
    NODE_KIND_KEY,
    NODE_NAME_KEY,
    NODE_RESOURCE_KEY,
    export_composite_collection,
)
from mh4blend.scene.export_fbx import export_fbx_collection  # noqa: E402
from mh4blend.scene.import_composite import (  # noqa: E402
    UNRESOLVED_PLACEMENT_KEY,
    import_composite_file,
)
from mh4blend.ui import ops  # noqa: E402
from mh4blend.ui import composite_authoring  # noqa: E402


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


def _write(path, resource):
    path.write_bytes(composite_json_bytes(resource))
    return path


def _counts():
    return {
        "objects": len(bpy.data.objects),
        "collections": len(bpy.data.collections),
        "meshes": len(bpy.data.meshes),
        "materials": len(bpy.data.materials),
        "images": len(bpy.data.images),
    }


def test_group_actor_import_export_preserves_tree_order_and_parent_local_transform(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    source = Composite("street_lights", [
        Node(
            "group",
            name="lights",
            transform=CompositeTransform(
                translation_cm=(100.0, 0.0, 0.0),
                rotation_quat=(0.0, 0.0, 0.0, 1.0),
                scale=(1.0, 1.0, 1.0),
            ),
            children=[Node(
                "actor",
                resource="lamp_point_warm",
                transform=CompositeTransform(translation_cm=(25.0, 0.0, 0.0)),
            )],
        ),
        Node("actor", resource="fog_volume"),
    ])
    path = _write(tmp_path / "street_lights.composite", source)
    expected = path.read_bytes()

    report = import_composite_file(path, source_root=tmp_path)
    collection = report["collection"]
    assert report["nodes"] == 3
    assert len(collection.objects) == 3
    roots = [obj for obj in collection.objects if obj.parent is None]
    assert [obj[NODE_RESOURCE_KEY] for obj in roots
            if NODE_RESOURCE_KEY in obj] == ["fog_volume"]
    group = next(obj for obj in collection.objects
                 if obj[NODE_KIND_KEY] == "group")
    actor = next(obj for obj in collection.objects if obj.parent == group)
    assert group[NODE_NAME_KEY] == "lights"
    assert actor[NODE_RESOURCE_KEY] == "lamp_point_warm"
    assert tuple(round(value, 6) for value in group.matrix_local.translation) \
        == (1.0, 0.0, 0.0)
    assert tuple(round(value, 6) for value in actor.matrix_local.translation) \
        == (0.25, 0.0, 0.0)
    assert tuple(round(value, 6) for value in actor.matrix_world.translation) \
        == (1.25, 0.0, 0.0)

    exported = export_composite_collection(
        collection, tmp_path, source_root=tmp_path)
    assert Path(exported["filepath"]).read_bytes() == expected
    assert composite_json_bytes(parse_composite(expected, name="street_lights")) \
        == expected


def test_direct_writer_uses_explicit_actor_token_without_registry(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = bpy.data.collections.new("actor_only")
    bpy.context.scene.collection.children.link(collection)
    actor = bpy.data.objects.new("ArtistReadableName", None)
    collection.objects.link(actor)
    actor.mh4blend.kind = "actor"
    actor[NODE_RESOURCE_KEY] = "project_actor_token"
    report = export_composite_collection(
        collection, tmp_path, source_root=tmp_path)
    decoded = parse_composite(Path(report["filepath"]).read_bytes())
    assert decoded.nodes[0].resource == "project_actor_token"


@pytest.mark.parametrize("kind,resource", [
    ("mesh", "missing_mesh"),
    ("composite", "missing_composite"),
])
def test_root_only_writer_blocks_unresolved_source_dependency(
        tmp_path, kind, resource):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = bpy.data.collections.new("unresolved")
    bpy.context.scene.collection.children.link(collection)
    placement = bpy.data.objects.new("placement", None)
    collection.objects.link(placement)
    placement.mh4blend.kind = kind
    placement[NODE_RESOURCE_KEY] = resource

    with pytest.raises(MHValidationError) as caught:
        export_composite_collection(
            collection, tmp_path, source_root=tmp_path)
    assert caught.value.code == "MH_E_RESOURCE_NOT_FOUND"
    assert f"{'static_mesh' if kind == 'mesh' else kind}:{resource}" in (
        caught.value.subjects)
    assert "composite:unresolved" in caught.value.subjects
    assert "Export Composite Include All Stuff" in caught.value.subjects
    assert not (tmp_path / "unresolved.composite").exists()


@pytest.mark.parametrize("kind,resource", [
    ("mesh", "missing_mesh"),
    ("composite", "missing_composite"),
])
def test_missing_resource_imports_placeholder_and_fresh_import_resolves(
        tmp_path, kind, resource):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    source = Composite("unresolved_authoring", [Node(
        kind,
        resource=resource,
        name="missing_placement",
        transform=CompositeTransform(
            translation_cm=(-63530.69140625, -200.0, 300.0),
            rotation_quat=(0.0, 0.0, 0.70710677, 0.70710677),
            scale=(1.25, 0.75, 2.0),
        ),
        children=[Node(
            "actor",
            resource="lossless_actor_token",
            transform=CompositeTransform(
                translation_cm=(-211.1234588623047, 25.0, -50.0),
                rotation_quat=(0.0, 0.70710677, 0.0, 0.70710677),
                scale=(1.25, 0.75, 2.0),
            ),
        )],
    )])
    source_path = _write(
        tmp_path / "unresolved_authoring.composite", source)
    expected = source_path.read_bytes()

    report = import_composite_file(source_path, source_root=tmp_path)
    assert [warning["code"] for warning in report["warnings"]] == [
        "MH_W_UNRESOLVED_PLACEMENT",
    ]
    assert f"{kind}:{resource}" in report["warnings"][0]["subjects"]
    placeholder = next(
        obj for obj in report["collection"].objects
        if obj[NODE_KIND_KEY] == kind)
    assert placeholder.type == "EMPTY"
    assert placeholder.data is None
    assert placeholder.instance_collection is None
    assert placeholder.instance_type == "NONE"
    assert placeholder.empty_display_type == "CUBE"
    assert tuple(placeholder.color) == pytest.approx((1.0, 0.0, 0.0, 1.0))
    assert placeholder[NODE_KIND_KEY] == kind
    assert placeholder[NODE_RESOURCE_KEY] == resource
    assert placeholder[UNRESOLVED_PLACEMENT_KEY] is True

    with pytest.raises(MHValidationError) as missing:
        export_composite_collection(
            report["collection"], tmp_path, source_root=tmp_path)
    assert missing.value.code == "MH_E_RESOURCE_NOT_FOUND"
    assert source_path.read_bytes() == expected

    child = next(obj for obj in report["collection"].objects
                 if obj.parent is placeholder)
    edited_matrix = child.matrix_world.copy()
    edited_matrix.translation.x += 1.0
    child.matrix_world = edited_matrix
    with pytest.raises(MHValidationError):
        export_composite_collection(
            report["collection"], tmp_path, source_root=tmp_path)
    assert source_path.read_bytes() == expected

    bpy.ops.wm.read_factory_settings(use_empty=True)
    if kind == "mesh":
        authored = bpy.data.collections.new(resource)
        bpy.context.scene.collection.children.link(authored)
        mesh = bpy.data.meshes.new("BodyGeometry")
        mesh.from_pydata(
            [(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
        authored.objects.link(bpy.data.objects.new("body", mesh))
        export_fbx_collection(authored, tmp_path, source_root=tmp_path)
    else:
        _write(tmp_path / f"{resource}.composite", Composite(resource))

    bpy.ops.wm.read_factory_settings(use_empty=True)
    resolved = import_composite_file(source_path, source_root=tmp_path)
    placement = next(
        obj for obj in resolved["collection"].objects
        if obj[NODE_KIND_KEY] == kind)
    assert resolved["warnings"] == []
    assert placement.instance_collection is not None
    assert UNRESOLVED_PLACEMENT_KEY not in placement
    exported = export_composite_collection(
        resolved["collection"], tmp_path, source_root=tmp_path)
    assert Path(exported["filepath"]).is_file()


@pytest.mark.parametrize("kind,extension", [
    ("mesh", ".mesh.fbx"),
    ("composite", ".composite"),
])
def test_ambiguous_same_kind_dependency_remains_fail_closed(
        tmp_path, kind, extension):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    source_path = _write(
        tmp_path / "ambiguous_root.composite",
        Composite("ambiguous_root", [Node(kind, resource="duplicate")]),
    )
    for directory in (tmp_path / "a", tmp_path / "b"):
        directory.mkdir()
        (directory / f"duplicate{extension}").write_bytes(b"duplicate")

    before = _counts()
    with pytest.raises(ValueError, match="MH_E_AMBIGUOUS_RESOURCE_NAME"):
        import_composite_file(source_path, source_root=tmp_path)
    assert _counts() == before

    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = bpy.data.collections.new("writer_ambiguous")
    bpy.context.scene.collection.children.link(collection)
    placement = bpy.data.objects.new("placement", None)
    collection.objects.link(placement)
    placement.mh4blend.kind = kind
    placement[NODE_RESOURCE_KEY] = "duplicate"
    preserved = composite_json_bytes(Composite("writer_ambiguous"))
    target = tmp_path / "writer_ambiguous.composite"
    target.write_bytes(preserved)
    with pytest.raises(ValueError, match="MH_E_AMBIGUOUS_RESOURCE_NAME"):
        export_composite_collection(
            collection, tmp_path, source_root=tmp_path)
    assert target.read_bytes() == preserved


def test_writer_rejects_self_and_ancestor_cycles_before_replace(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = bpy.data.collections.new("cycle_a")
    bpy.context.scene.collection.children.link(collection)
    placement = bpy.data.objects.new("placement", None)
    collection.objects.link(placement)
    placement.mh4blend.kind = "composite"
    placement[NODE_RESOURCE_KEY] = "cycle_a"
    with pytest.raises(ValueError, match="MH_E_COMPOSITE_CYCLE"):
        export_composite_collection(collection, tmp_path, source_root=tmp_path)
    assert not (tmp_path / "cycle_a.composite").exists()

    (tmp_path / "cycle_b.composite").write_bytes(composite_json_bytes(
        Composite("cycle_b", [Node("composite", resource="cycle_a")])
    ))
    placement[NODE_RESOURCE_KEY] = "cycle_b"
    preserved = b'{\n  "v": 5,\n  "nodes": []\n}\n'
    (tmp_path / "cycle_a.composite").write_bytes(preserved)
    with pytest.raises(ValueError, match="MH_E_COMPOSITE_CYCLE"):
        export_composite_collection(collection, tmp_path, source_root=tmp_path)
    assert (tmp_path / "cycle_a.composite").read_bytes() == preserved


def test_writer_rejects_non_roundtrippable_shear(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = bpy.data.collections.new("sheared")
    bpy.context.scene.collection.children.link(collection)
    group = bpy.data.objects.new("sheared_group", None)
    collection.objects.link(group)
    group.mh4blend.kind = "group"
    group.matrix_world = Matrix((
        (1.0, 0.25, 0.0, 0.0),
        (0.0, 1.0, 0.0, 0.0),
        (0.0, 0.0, 1.0, 0.0),
        (0.0, 0.0, 0.0, 1.0),
    ))
    with pytest.raises(
            ValueError,
            match="MH_E_UNREPRESENTABLE_TRANSFORM") as excinfo:
        export_composite_collection(collection, tmp_path, source_root=tmp_path)
    assert "sheared_group" in str(excinfo.value)
    assert not (tmp_path / "sheared.composite").exists()


def test_writer_rejects_child_shear_from_rotated_nonuniform_parent(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = bpy.data.collections.new("parent_shear")
    bpy.context.scene.collection.children.link(collection)

    parent = bpy.data.objects.new("scaled_parent", None)
    collection.objects.link(parent)
    parent.mh4blend.kind = "group"
    parent.scale = (1.0, 2.0, 1.0)

    child = bpy.data.objects.new("rotated_child", None)
    collection.objects.link(child)
    child.mh4blend.kind = "group"
    child.parent = parent
    child.rotation_euler[2] = math.radians(45.0)
    bpy.context.view_layer.update()

    with pytest.raises(
            ValueError,
            match="MH_E_UNREPRESENTABLE_TRANSFORM") as excinfo:
        export_composite_collection(collection, tmp_path, source_root=tmp_path)
    rendered = str(excinfo.value)
    assert "rotated_child" in rendered
    assert "float32 T/R/S" in rendered
    assert not (tmp_path / "parent_shear.composite").exists()


def test_writer_identifies_raw_mesh_instead_of_collection_instance(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = bpy.data.collections.new("unmarked")
    bpy.context.scene.collection.children.link(collection)
    mesh = bpy.data.meshes.new("UnmarkedGeometry")
    placement = bpy.data.objects.new("UnmarkedPlacement", mesh)
    collection.objects.link(placement)

    with pytest.raises(ValueError, match="MH_E_COMPOSITE_GRAMMAR") as excinfo:
        export_composite_collection(collection, tmp_path, source_root=tmp_path)
    rendered = str(excinfo.value)
    assert "UnmarkedPlacement" in rendered
    assert "no typed mh4blend.kind" in rendered
    assert "diagnostic mirror" in rendered


def test_writer_rejects_unmanaged_mesh_collection_instance(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mesh_definition = bpy.data.collections.new("garage_shell")
    mesh = bpy.data.meshes.new("GarageGeometry")
    definition_object = bpy.data.objects.new("GarageGeometry", mesh)
    mesh_definition.objects.link(definition_object)

    composite = bpy.data.collections.new("garage_set")
    bpy.context.scene.collection.children.link(composite)
    placement = bpy.data.objects.new("GaragePlacement", None)
    composite.objects.link(placement)
    placement.mh4blend.kind = "mesh"
    placement.instance_type = "COLLECTION"
    placement.instance_collection = mesh_definition

    with pytest.raises(MHValidationError) as caught:
        export_composite_collection(
            composite, tmp_path, source_root=tmp_path)
    assert caught.value.code == "MH_E_INVALID_RESOURCE_SOURCE"
    assert not (tmp_path / "garage_set.composite").exists()


def test_composite_import_materializes_mesh_once_and_builds_instance(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    authored = bpy.data.collections.new("vehicle")
    bpy.context.scene.collection.children.link(authored)
    mesh = bpy.data.meshes.new("BodyGeometry")
    mesh.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
    body = bpy.data.objects.new("body", mesh)
    authored.objects.link(body)
    paint = bpy.data.materials.new("paint")
    body.data.materials.append(paint)
    export_fbx_collection(authored, tmp_path, source_root=tmp_path)
    (tmp_path / "paint.material").write_bytes(
        b'{\n  "class": "simple"\n}\n')
    composite_path = _write(
        tmp_path / "garage.composite",
        Composite("garage", [
            Node("mesh", resource="vehicle"),
            Node("mesh", resource="vehicle",
                 transform=CompositeTransform(translation_cm=(250.0, 0.0, 0.0))),
        ]),
    )
    bpy.ops.wm.read_factory_settings(use_empty=True)

    report = import_composite_file(composite_path, source_root=tmp_path)
    root = report["collection"]
    instances = list(root.objects)
    assert len(instances) == 2
    assert instances[0].instance_collection is instances[1].instance_collection
    mesh_collection = instances[0].instance_collection
    assert mesh_collection[COLLECTION_KIND_KEY] == "mesh"
    assert mesh_collection.name not in bpy.context.scene.collection.children
    assert [material.name for material in bpy.data.materials] == ["paint"]
    assert len(report["meshes"]) == 1


def test_preflight_occupied_collection_leaves_datablock_delta_empty(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    path = _write(tmp_path / "occupied.composite", Composite("occupied"))
    bpy.data.collections.new("occupied.composite")
    before = _counts()
    with pytest.raises(ValueError, match="MH_E_IMPORT_TARGET_OCCUPIED"):
        import_composite_file(path, source_root=tmp_path)
    assert _counts() == before


def test_composite_preflight_rejects_generated_id_name_truncation(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    long_name = "a" * 60
    path = _write(tmp_path / f"{long_name}.composite", Composite(long_name))
    before = _counts()
    with pytest.raises(ValueError, match="MH_E_IMPORT_TARGET_OCCUPIED"):
        import_composite_file(path, source_root=tmp_path)
    assert _counts() == before


def test_failure_during_placement_rolls_back_entire_delta(tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    path = _write(
        tmp_path / "rollback.composite",
        Composite("rollback", [Node("mesh", resource="missing_mesh")]),
    )
    before = _counts()
    original = import_module._build_definition

    def fail_after_build(*args, **kwargs):
        original(*args, **kwargs)
        raise RuntimeError("synthetic placement failure")

    monkeypatch.setattr(import_module, "_build_definition", fail_after_build)
    with pytest.raises(RuntimeError, match="synthetic placement failure"):
        import_composite_file(path, source_root=tmp_path)
    assert _counts() == before


def test_composite_cycle_fails_before_scene_mutation(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    _write(tmp_path / "cycle_a.composite", Composite(
        "cycle_a", [Node("composite", resource="cycle_b")]))
    _write(tmp_path / "cycle_b.composite", Composite(
        "cycle_b", [Node("composite", resource="cycle_a")]))
    before = _counts()
    with pytest.raises(ValueError, match="MH_E_COMPOSITE_CYCLE"):
        import_composite_file(
            tmp_path / "cycle_a.composite", source_root=tmp_path)
    assert _counts() == before


def test_dag4blend_scene_export_names_the_conversion_remedy(tmp_path):
    """A dag4blend scene must be signposted, never implicitly converted."""

    bpy.ops.wm.read_factory_settings(use_empty=True)
    definition = bpy.data.collections.new("gaz53_b_random_cmp")
    definition[COLLECTION_KIND_KEY] = "composite"
    definition[COLLECTION_RESOURCE_KEY] = "gaz53_b_random_cmp"

    # Exactly what dag4blend's cmp_import.py leaves behind: an unnamed node
    # Empty instancing a random.NNN helper, plus its stamped mirrors.
    helper = bpy.data.collections.new("random.000")
    node = bpy.data.objects.new("node", None)
    definition.objects.link(node)
    node.instance_type = "COLLECTION"
    node.instance_collection = helper
    node["type:t"] = "random"

    option_resource = bpy.data.collections.new("gaz53_bread_b_cmp")
    option_resource["name"] = "gaz53_bread_b_cmp"
    option_resource["type"] = "composit"
    option = bpy.data.objects.new("gaz53_bread_b_cmp.001", None)
    helper.objects.link(option)
    option.instance_type = "COLLECTION"
    option.instance_collection = option_resource
    option["weight:r"] = 1.0
    option["type:t"] = "composit"

    for subject in (node, option):
        assert subject.mh4blend.kind == "unset"

    with pytest.raises(MHValidationError) as caught:
        export_composite_collection(definition, tmp_path, source_root=tmp_path)
    message = caught.value.message
    assert "mh.convert_dag4blend_composite" in message
    assert "mh.import_dagor_composite" in message
    assert "Export never converts a scene implicitly" in message
    assert "random helper 'random.000'" in message

    # An ordinary unstamped Empty carries no dag4blend trace and keeps the
    # original diagnostic without misleading conversion advice.
    bpy.data.objects.remove(node)
    plain = bpy.data.objects.new("plain_empty", None)
    definition.objects.link(plain)
    with pytest.raises(MHValidationError) as plain_caught:
        export_composite_collection(definition, tmp_path, source_root=tmp_path)
    assert "mh.convert_dag4blend_composite" not in plain_caught.value.message
    assert NODE_KIND_KEY in plain_caught.value.message
