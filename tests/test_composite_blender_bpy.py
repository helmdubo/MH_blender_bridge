"""Blender gates for v4 Composite scene adapters."""

from pathlib import Path
import sys

import pytest

bpy = pytest.importorskip("bpy")
from mathutils import Matrix
REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.composites import composite_json_bytes, parse_composite  # noqa: E402
from mh4blend.core.model import Composite, CompositeTransform, Node  # noqa: E402
from mh4blend.scene import import_composite as import_module  # noqa: E402
from mh4blend.scene.export_composite import (  # noqa: E402
    COLLECTION_KIND_KEY,
    NODE_KIND_KEY,
    NODE_NAME_KEY,
    NODE_RESOURCE_KEY,
    export_composite_collection,
)
from mh4blend.scene.export_fbx import export_fbx_collection  # noqa: E402
from mh4blend.scene.import_composite import import_composite_file  # noqa: E402
from mh4blend.ui import ops  # noqa: E402


@pytest.fixture(autouse=True)
def registered_addon_properties():
    owned = not hasattr(bpy.types.Material, "mh4blend")
    if owned:
        ops.register()
    try:
        yield
    finally:
        if owned:
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


def test_group_actor_import_export_preserves_tree_order_and_world_transform(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    source = Composite("street_lights", [
        Node(
            "group",
            name="lights",
            transform=CompositeTransform(
                translation_cm=(100.0, -200.0, 300.0),
                rotation_quat=(0.0, 0.0, 0.0, 1.0),
                scale=(1.0, 1.0, 1.0),
            ),
            children=[Node("actor", resource="lamp_point_warm")],
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
    actor[NODE_KIND_KEY] = "actor"
    actor[NODE_RESOURCE_KEY] = "project_actor_token"
    report = export_composite_collection(
        collection, tmp_path, source_root=tmp_path)
    decoded = parse_composite(Path(report["filepath"]).read_bytes())
    assert decoded.nodes[0].resource == "project_actor_token"


@pytest.mark.parametrize("kind,resource", [
    ("mesh", "missing_mesh"),
    ("composite", "missing_composite"),
])
def test_writer_rejects_unresolved_source_dependency_before_publish(
        tmp_path, kind, resource):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = bpy.data.collections.new("unresolved")
    bpy.context.scene.collection.children.link(collection)
    placement = bpy.data.objects.new("placement", None)
    collection.objects.link(placement)
    placement[NODE_KIND_KEY] = kind
    placement[NODE_RESOURCE_KEY] = resource

    with pytest.raises(
            ValueError, match="MH_E_UNRESOLVED_COMPOSITE_REFERENCE"):
        export_composite_collection(collection, tmp_path, source_root=tmp_path)
    assert not (tmp_path / "unresolved.composite").exists()


def test_writer_rejects_self_and_ancestor_cycles_before_replace(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = bpy.data.collections.new("cycle_a")
    bpy.context.scene.collection.children.link(collection)
    placement = bpy.data.objects.new("placement", None)
    collection.objects.link(placement)
    placement[NODE_KIND_KEY] = "composite"
    placement[NODE_RESOURCE_KEY] = "cycle_a"
    with pytest.raises(ValueError, match="MH_E_COMPOSITE_CYCLE"):
        export_composite_collection(collection, tmp_path, source_root=tmp_path)
    assert not (tmp_path / "cycle_a.composite").exists()

    (tmp_path / "cycle_b.composite").write_bytes(composite_json_bytes(
        Composite("cycle_b", [Node("composite", resource="cycle_a")])
    ))
    placement[NODE_RESOURCE_KEY] = "cycle_b"
    preserved = b'{\n  "nodes": []\n}\n'
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
    group[NODE_KIND_KEY] = "group"
    group.matrix_world = Matrix((
        (1.0, 0.25, 0.0, 0.0),
        (0.0, 1.0, 0.0, 0.0),
        (0.0, 0.0, 1.0, 0.0),
        (0.0, 0.0, 0.0, 1.0),
    ))
    with pytest.raises(ValueError, match="MH_E_COMPOSITE_GRAMMAR"):
        export_composite_collection(collection, tmp_path, source_root=tmp_path)


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
        Composite("rollback", [Node("group")]),
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
