"""Blender 4.5 gates for the transactional v4 mesh FBX importer."""

import importlib
from pathlib import Path
import sys

import pytest

bpy = pytest.importorskip("bpy")
REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.validate import MHValidationError  # noqa: E402
from mh4blend.scene.export_fbx import export_fbx_collection  # noqa: E402
from mh4blend.scene.import_fbx import (  # noqa: E402
    LOAD_MODE_LOD0,
    LOAD_MODE_STRUCTURE_ONLY,
    MeshImportTransaction,
    import_mesh_fbx,
    parse_mesh_fbx,
)
from mh4blend.scene.resource_markers import (  # noqa: E402
    COLLECTION_KIND_KEY,
    COLLECTION_RESOURCE_KEY,
    INCOMPLETE_IMPORT_KEY,
)
from mh4blend.ui import ops  # noqa: E402

import_fbx_module = importlib.import_module("mh4blend.scene.import_fbx")


@pytest.fixture(autouse=True)
def clean_registered_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    owned = not hasattr(bpy.types.Material, "mh4blend")
    if owned:
        ops.register()
    try:
        yield
    finally:
        if owned:
            ops.unregister()


def _collection(name):
    collection = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(collection)
    return collection


def _mesh(name, collection, mesh_name=None):
    mesh = bpy.data.meshes.new(mesh_name or name + "Mesh")
    mesh.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    return obj


def _empty(name, collection):
    obj = bpy.data.objects.new(name, None)
    collection.objects.link(obj)
    return obj


def _material(name="paint"):
    material = bpy.data.materials.new(name)
    material.mh4blend.mode = "CLASS"
    material.mh4blend.material_class = "simple"
    return material


def _material_source(root, name="paint"):
    path = root / f"{name}.material"
    path.write_bytes(b'{\n  "class": "simple"\n}\n')
    return path


def _export_joined(root, name="vehicle", *, two_materials=False):
    source = _collection(name)
    group = _empty("root", source)
    body = _mesh("body", source, "BodyGeometry")
    body.parent = group
    body.data.materials.append(_material())
    if two_materials:
        body.data.materials.append(_material("glass"))
    socket = _empty("SOCKET_grip", source)
    socket.parent = group
    collision = _mesh("UCX_body", source, "CollisionGeometry")
    collision.parent = group
    report = export_fbx_collection(source, root, source_root=root)
    _material_source(root)
    if two_materials:
        _material_source(root, "glass")
    return Path(report["filepath"])


def _export_lods(root, name="vehicle"):
    source = _collection(name + ".lods")
    lod0 = bpy.data.collections.new(name + ".lod00")
    lod1 = bpy.data.collections.new(name + ".lod01")
    source.children.link(lod0)
    source.children.link(lod1)
    body0 = _mesh("body", lod0, "BodyGeometry")
    body1 = _mesh("body_low", lod1, "BodyLowGeometry")
    paint = _material()
    body0.data.materials.append(paint)
    body1.data.materials.append(paint)
    _mesh("UCX_body", lod0, "CollisionGeometry")
    _empty("SOCKET_grip", lod0)
    report = export_fbx_collection(source, root, source_root=root)
    _material_source(root)
    return Path(report["filepath"])


def _reset_keep_sources():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def _inventory():
    attrs = ("objects", "collections", "meshes", "materials", "images")
    return {
        attr: tuple(sorted(item.name for item in getattr(bpy.data, attr)))
        for attr in attrs
    }


def _semantic_plan(path):
    plan = parse_mesh_fbx(path)
    return [
        (node.name, node.kind, node.lod_level, node.collision_mode,
         node.parent, node.material_slots)
        for node in plan.nodes
    ]


def test_import_rebuilds_parse_hierarchy_and_ordered_slots(tmp_path):
    source_path = _export_joined(tmp_path, two_materials=True)
    source_plan = _semantic_plan(source_path)
    _reset_keep_sources()

    report = import_mesh_fbx(source_path, source_root=tmp_path)
    collection = report["collection"]
    assert report["collection_name"] == "vehicle"
    assert collection == bpy.data.collections["vehicle"]
    assert collection[COLLECTION_KIND_KEY] == "mesh"
    assert collection[COLLECTION_RESOURCE_KEY] == "vehicle"
    assert report["materials_created"] == ["paint", "glass"]
    assert bpy.data.objects["body"].parent == bpy.data.objects["root"]
    assert bpy.data.objects["SOCKET_grip"].parent == bpy.data.objects["root"]
    assert [slot.material.name for slot in bpy.data.objects["body"].material_slots] == [
        "paint", "glass"]

    roundtrip_dir = tmp_path / "roundtrip"
    roundtrip_dir.mkdir()
    roundtrip = export_fbx_collection(
        collection, roundtrip_dir, source_root=tmp_path)["filepath"]
    assert _semantic_plan(roundtrip) == source_plan


def test_lods_restructure_without_rewriting_parsed_node_names(tmp_path):
    source_path = _export_lods(tmp_path)
    source_plan = _semantic_plan(source_path)
    _reset_keep_sources()

    report = import_mesh_fbx(source_path, source_root=tmp_path)
    root = report["collection"]
    assert report["collection_name"] == "vehicle.lods"
    assert report["lod_levels"] == [0, 1]
    assert [child.name for child in root.children] == [
        "vehicle.lod00", "vehicle.lod01"]
    assert "body_lod00" in root.children["vehicle.lod00"].objects
    assert "body_low_lod01" in root.children["vehicle.lod01"].objects
    assert "UCX_body" in root.children["vehicle.lod00"].objects
    assert "SOCKET_grip" in root.children["vehicle.lod00"].objects

    roundtrip_dir = tmp_path / "roundtrip"
    roundtrip_dir.mkdir()
    roundtrip = export_fbx_collection(
        root, roundtrip_dir, source_root=tmp_path)["filepath"]
    assert _semantic_plan(roundtrip) == source_plan


def test_lod0_keeps_structure_and_marks_definition_incomplete(tmp_path):
    source_path = _export_lods(tmp_path)
    _reset_keep_sources()

    report = import_mesh_fbx(
        source_path, source_root=tmp_path, load_mode=LOAD_MODE_LOD0)
    root = report["collection"]
    assert report["lod_levels"] == [0]
    assert root.get(INCOMPLETE_IMPORT_KEY) is True
    assert [child.name for child in root.children] == ["vehicle.lod00"]
    assert "body_lod00" in root.children["vehicle.lod00"].objects
    assert "UCX_body" in root.children["vehicle.lod00"].objects
    assert "SOCKET_grip" in root.children["vehicle.lod00"].objects
    assert bpy.data.objects.get("body_low_lod01") is None


def test_structure_only_creates_empty_definition_without_materials(tmp_path):
    source_path = _export_lods(tmp_path)
    _reset_keep_sources()

    report = import_mesh_fbx(
        source_path, source_root=tmp_path,
        load_mode=LOAD_MODE_STRUCTURE_ONLY)
    root = report["collection"]
    assert root.name == "vehicle.lods"
    assert root.get(INCOMPLETE_IMPORT_KEY) is True
    assert len(root.objects) == 0
    assert len(root.children) == 0
    assert len(bpy.data.materials) == 0


def test_reuse_preserves_complete_managed_definition_as_is(tmp_path):
    source_path = _export_joined(tmp_path)
    _reset_keep_sources()
    first = import_mesh_fbx(source_path, source_root=tmp_path)
    root = first["collection"]
    marker = _empty("artist_note", root)
    pointer = root.as_pointer()

    second = import_mesh_fbx(source_path, source_root=tmp_path)
    assert second["definition_action"] == "reuse"
    assert second["collection"].as_pointer() == pointer
    assert root.objects.get(marker.name) is marker


def test_refresh_preserves_collection_pointer_and_replaces_contents(tmp_path):
    source_path = _export_joined(tmp_path)
    _reset_keep_sources()
    root = import_mesh_fbx(source_path, source_root=tmp_path)["collection"]
    old_body = bpy.data.objects["body"]
    marker = _empty("artist_note", root)
    marker_name = marker.name
    placement = bpy.data.objects.new("external_placement", None)
    placement.instance_type = "COLLECTION"
    placement.instance_collection = root
    bpy.context.scene.collection.objects.link(placement)
    pointer = root.as_pointer()

    report = import_mesh_fbx(
        source_path, source_root=tmp_path, definition_policy="refresh")
    assert report["definition_action"] == "refresh"
    assert report["collection"].as_pointer() == pointer
    assert placement.instance_collection is root
    assert root.objects.get(marker_name) is None
    assert bpy.data.objects["body"] is not old_body
    assert root.get(INCOMPLETE_IMPORT_KEY) is None


def test_existing_canonical_material_is_reused_without_dot_duplicates(tmp_path):
    source_path = _export_joined(tmp_path)
    _reset_keep_sources()
    existing = _material()

    report = import_mesh_fbx(source_path, source_root=tmp_path)
    assert report["materials_reused"] == ["paint"]
    assert report["materials_created"] == []
    assert bpy.data.materials["paint"] == existing
    assert [material.name for material in bpy.data.materials] == ["paint"]
    # Exporter's disposable Mesh.copy() may already have written a literal
    # Geometry name ending in .001; that parse-truth is not an import rename.
    # No additional object/material/image/collection duplicate may appear.
    for attr in ("objects", "materials", "images", "collections"):
        assert not any(item.name.endswith(".001")
                       for item in getattr(bpy.data, attr))


def test_preflight_occupation_has_zero_datablock_delta(tmp_path):
    source_path = _export_joined(tmp_path)
    _reset_keep_sources()
    _empty("body", _collection("unrelated"))
    before = _inventory()

    with pytest.raises(MHValidationError) as exc:
        import_mesh_fbx(source_path, source_root=tmp_path)
    assert exc.value.code == "MH_E_IMPORT_TARGET_OCCUPIED"
    assert _inventory() == before


def test_preflight_rejects_resource_name_blender_would_truncate(tmp_path):
    source_path = _export_joined(tmp_path)
    _reset_keep_sources()
    long_name = "a" * 80
    long_path = source_path.with_name(f"{long_name}.mesh.fbx")
    source_path.rename(long_path)
    before = _inventory()

    with pytest.raises(MHValidationError) as exc:
        import_mesh_fbx(long_path, source_root=tmp_path)
    assert exc.value.code == "MH_E_IMPORT_TARGET_OCCUPIED"
    assert "cannot preserve exact Blender ID name" in str(exc.value)
    assert _inventory() == before


@pytest.mark.parametrize(
    "seam", ["_stage_geometry", "_bind_parsed_nodes", "_stage_restructure"])
def test_failure_at_each_mutating_stage_rolls_back_every_delta(
        tmp_path, monkeypatch, seam):
    source_path = _export_joined(tmp_path)
    _reset_keep_sources()
    before = _inventory()
    monkeypatch.setattr(
        import_fbx_module, seam,
        lambda *_args, **_kwargs: (_ for _ in ()).throw(
            RuntimeError("synthetic mutating-stage failure")))

    with pytest.raises(RuntimeError, match="synthetic mutating-stage failure"):
        import_mesh_fbx(source_path, source_root=tmp_path)
    assert _inventory() == before


def test_caller_transaction_rolls_back_mesh_and_later_placements(tmp_path):
    source_path = _export_joined(tmp_path)
    _reset_keep_sources()
    before = _inventory()

    with pytest.raises(RuntimeError, match="placement failure"):
        with MeshImportTransaction() as transaction:
            import_mesh_fbx(
                source_path, source_root=tmp_path, transaction=transaction)
            placement = bpy.data.objects.new("composite_placement", None)
            bpy.context.scene.collection.objects.link(placement)
            raise RuntimeError("placement failure")
    assert _inventory() == before


def test_missing_material_fails_before_geometry_and_rolls_back(tmp_path):
    source_path = _export_joined(tmp_path)
    (tmp_path / "paint.material").unlink()
    _reset_keep_sources()
    before = _inventory()

    with pytest.raises(MHValidationError) as exc:
        import_mesh_fbx(source_path, source_root=tmp_path)
    assert exc.value.code == "MH_E_UNRESOLVED_MATERIAL_REFERENCE"
    assert _inventory() == before
