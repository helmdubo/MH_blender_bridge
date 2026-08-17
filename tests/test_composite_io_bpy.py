"""Blender integration tests for standalone composite import/export."""

import json
import importlib
import math
import re
import sys
from pathlib import Path

import pytest

bpy = pytest.importorskip("bpy")

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.model import composite_disk_dict  # noqa: E402
from mh4blend.core.uid import PROP_UID  # noqa: E402
from mh4blend.core.validate import MHValidationError  # noqa: E402
from mh4blend.scene.composite_extract import (  # noqa: E402
    PROP_KIND,
    extract_composite,
)
from mh4blend.scene.export_composite import (  # noqa: E402
    export_composite_collection,
)
from mh4blend.scene.export_fbx import export_fbx_collection  # noqa: E402
from mh4blend.scene.import_composite import (  # noqa: E402
    CompositeImportError,
    import_composite_file,
)

ROOT_UID = "10000000-0000-0000-0000-000000000001"
CHILD_UID = "20000000-0000-0000-0000-000000000002"
MESH_UID = "30000000-0000-0000-0000-000000000003"
GROUP_UID = "40000000-0000-0000-0000-000000000004"
CHILD_NODE_UID = "50000000-0000-0000-0000-000000000005"
MESH_NODE_UID = "60000000-0000-0000-0000-000000000006"
MATERIAL_UID = "a0000000-0000-0000-0000-00000000000a"
MISSING_MATERIAL_UID = "b0000000-0000-0000-0000-00000000000b"
SECOND_MESH_UID = "30000000-0000-0000-0000-000000000004"


class _CompositeIOTestTextures(bpy.types.PropertyGroup):
    tex0: bpy.props.StringProperty(default="", subtype="FILE_PATH")


class _CompositeIOTestOptional(bpy.types.PropertyGroup):
    pass


class _CompositeIOTestDagormat(bpy.types.PropertyGroup):
    shader_class: bpy.props.StringProperty(default="")
    sides: bpy.props.IntProperty(default=0, min=0, max=2)
    optional: bpy.props.PointerProperty(type=_CompositeIOTestOptional)
    textures: bpy.props.PointerProperty(type=_CompositeIOTestTextures)


@pytest.fixture(autouse=True)
def empty_file():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    yield


@pytest.fixture
def dagormat_rna():
    if hasattr(bpy.types.Material, "dagormat"):
        pytest.skip("test dagormat RNA conflicts with an enabled addon")
    classes = (
        _CompositeIOTestTextures,
        _CompositeIOTestOptional,
        _CompositeIOTestDagormat,
    )
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.Material.dagormat = bpy.props.PointerProperty(
        type=_CompositeIOTestDagormat)
    try:
        yield
    finally:
        del bpy.types.Material.dagormat
        for cls in reversed(classes):
            bpy.utils.unregister_class(cls)


def _node(uid, kind, name, resource_uid=None, parent_uid=None,
          translation=(0.0, 0.0, 0.0),
          rotation=(0.0, 0.0, 0.0, 1.0), scale=(1.0, 1.0, 1.0),
          properties=None, **extra):
    row = {
        "node_uid": uid,
        "parent_uid": parent_uid,
        "kind": kind,
        "display_name": name,
        "local_transform": {
            "translation_cm": list(translation),
            "rotation_quat": list(rotation),
            "scale": list(scale),
        },
        "properties": properties or {},
    }
    if resource_uid is not None:
        row["resource_uid"] = resource_uid
    row.update(extra)
    return row


def _document(uid, name, nodes):
    return {
        "schema": "mh.composite",
        "schema_version": 1,
        "uid": uid,
        "name": name,
        "nodes": nodes,
    }


def _write_json(path, document):
    path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def _manifest(resources):
    return {
        "schema": "mh.export_manifest",
        "schema_version": 1,
        "exporter_version": "test",
        "source": {"blend_file": "test.blend"},
        "resources": resources,
        "materials": [],
    }


def _resource(uid, kind, name, source):
    return {
        "uid": uid, "kind": kind, "name": name, "source": source,
        "content_hash": "xxh3:test",
    }


def _collection_by_uid(uid):
    return next(collection for collection in bpy.data.collections
                if collection.get(PROP_UID) == uid)


def _blend_id_state():
    names = (
        "objects", "collections", "scenes", "meshes", "materials",
        "armatures", "curves", "cameras", "lights", "images", "actions",
        "node_groups", "textures",
    )
    return {
        name: {item.as_pointer() for item in getattr(bpy.data, name)}
        for name in names
    }


def _export_test_fbx(path, material_name=None):
    bpy.ops.mesh.primitive_cube_add()
    cube = bpy.context.object
    if material_name:
        cube.data.materials.append(bpy.data.materials.new(material_name))
    cube.select_set(True)
    bpy.ops.export_scene.fbx(filepath=str(path), use_selection=True)


def _write_mesh_import_fixture(tmp_path, mesh_rows, materials=()):
    nodes = [
        _node(
            f"c0000000-0000-0000-0000-{index:012d}",
            "mesh", row["name"], row["uid"])
        for index, row in enumerate(mesh_rows, 1)
    ]
    root_path = tmp_path / "root.composite"
    _write_json(root_path, _document(ROOT_UID, "root_cmp", nodes))
    manifest = _manifest([
        _resource(ROOT_UID, "composite", "root_cmp", "root.composite"),
        *mesh_rows,
    ])
    manifest["materials"] = list(materials)
    _write_json(tmp_path / "export_manifest.json", manifest)
    return root_path


def test_export_selected_collection_only_and_updates_manifest(tmp_path):
    definition = bpy.data.collections.new("root_cmp")
    definition[PROP_UID] = ROOT_UID
    mesh = bpy.data.collections.new("mesh_a")
    mesh[PROP_UID] = MESH_UID
    mesh.objects.link(bpy.data.objects.new(
        "mesh_content", bpy.data.meshes.new("mesh_content_data")))
    child = bpy.data.collections.new("child_cmp")
    child[PROP_UID] = CHILD_UID

    group = bpy.data.objects.new("group", None)
    group[PROP_UID] = GROUP_UID
    definition.objects.link(group)
    mesh_node = bpy.data.objects.new("mesh_node", None)
    mesh_node[PROP_UID] = MESH_NODE_UID
    mesh_node.instance_type = "COLLECTION"
    mesh_node.instance_collection = mesh
    mesh_node.parent = group
    mesh_node.location = (1.0, 2.0, 3.0)
    mesh_node["mh_p_weight"] = 0.25
    definition.objects.link(mesh_node)
    child_node = bpy.data.objects.new("child_node", None)
    child_node[PROP_UID] = CHILD_NODE_UID
    child_node.instance_type = "COLLECTION"
    child_node.instance_collection = child
    definition.objects.link(child_node)

    # Literal subcollection content and direct non-Empty helpers are not nodes.
    literal = bpy.data.collections.new("literal_child")
    definition.children.link(literal)
    ignored_empty = bpy.data.objects.new("nested_empty", None)
    ignored_empty[PROP_UID] = "70000000-0000-0000-0000-000000000007"
    literal.objects.link(ignored_empty)
    ignored_mesh = bpy.data.objects.new("authoring_helper", bpy.data.meshes.new("m"))
    definition.objects.link(ignored_mesh)

    report = export_composite_collection(definition, str(tmp_path))
    assert report["ok"]
    assert report["resource_entry"]["content_hash"].startswith("xxh3:")
    document = json.loads(Path(report["path"]).read_text(encoding="utf-8"))
    assert {row["node_uid"] for row in document["nodes"]} == {
        GROUP_UID, MESH_NODE_UID, CHILD_NODE_UID}
    by_uid = {row["node_uid"]: row for row in document["nodes"]}
    assert by_uid[MESH_NODE_UID]["kind"] == "mesh"
    assert by_uid[CHILD_NODE_UID]["kind"] == "composite_ref"
    assert by_uid[MESH_NODE_UID]["parent_uid"] == GROUP_UID
    manifest = json.loads((tmp_path / "export_manifest.json").read_text())
    assert manifest["schema"] == "mh.export_manifest"
    assert manifest["resources"] == [report["resource_entry"]]
    assert not (tmp_path / "export_manifest.json.tmp").exists()


def test_nonzero_instance_offset_blocks_before_destination_write(tmp_path):
    definition = bpy.data.collections.new("root_cmp")
    definition[PROP_UID] = ROOT_UID
    target = bpy.data.collections.new("old_mesh_name")
    target[PROP_UID] = MESH_UID
    target.instance_offset = (0.5, 0.0, 0.0)
    node = bpy.data.objects.new("mesh_node", None)
    node[PROP_UID] = MESH_NODE_UID
    node.instance_type = "COLLECTION"
    node.instance_collection = target
    definition.objects.link(node)
    with pytest.raises(MHValidationError,
                       match="MH_E_INVALID_COLLECTION_OFFSET"):
        export_composite_collection(definition, str(tmp_path))
    assert list(tmp_path.iterdir()) == []


def test_stale_collection_pointer_with_instance_type_none_blocks_export(
        tmp_path):
    definition = bpy.data.collections.new("root_cmp")
    definition[PROP_UID] = ROOT_UID
    target = bpy.data.collections.new("mesh_a")
    target[PROP_UID] = MESH_UID
    node = bpy.data.objects.new("mesh_node", None)
    node[PROP_UID] = MESH_NODE_UID
    node.instance_collection = target
    node.instance_type = "NONE"
    assert node.instance_collection == target
    definition.objects.link(node)

    with pytest.raises(MHValidationError, match="MH_E_INVALID_COMPOSITE"):
        export_composite_collection(definition, str(tmp_path))
    assert list(tmp_path.iterdir()) == []


def test_export_stages_manifest_before_payload_replace(tmp_path, monkeypatch):
    definition = bpy.data.collections.new("root_cmp")
    definition[PROP_UID] = ROOT_UID
    module = importlib.import_module("mh4blend.scene.export_composite")

    def fail_payload(_path, _document):
        raise RuntimeError("simulated payload failure")

    monkeypatch.setattr(module, "_write_json_atomic", fail_payload)
    with pytest.raises(RuntimeError, match="simulated payload failure"):
        export_composite_collection(definition, str(tmp_path))
    assert (tmp_path / "export_manifest.json.tmp").exists()
    assert not (tmp_path / "export_manifest.json").exists()
    assert not list(tmp_path.glob("*.composite"))


def _write_dependency_fixture(tmp_path, *, cycle=False):
    root_nodes = [
        _node(GROUP_UID, "group", "group", properties={"seed": 7}),
        _node(CHILD_NODE_UID, "composite_ref", "child_instance",
              CHILD_UID, GROUP_UID, (100.0, -200.0, 300.0),
              (0.0, 0.0, -0.707107, 0.707107), (2.0, 1.0, 0.5),
              properties={"offset": 0.5, "nullable": None},
              future_tag={"kept": True}),
        _node(MESH_NODE_UID, "mesh", "mesh_instance", MESH_UID),
    ]
    child_nodes = []
    if cycle:
        child_nodes.append(_node(
            "80000000-0000-0000-0000-000000000008",
            "composite_ref", "back_to_root", ROOT_UID))
    root_path = tmp_path / "root.composite"
    child_path = tmp_path / "child.composite"
    _write_json(root_path, _document(ROOT_UID, "root_cmp", root_nodes))
    _write_json(child_path, _document(CHILD_UID, "child_cmp", child_nodes))
    resources = [
        _resource(ROOT_UID, "composite", "root_cmp", "root.composite"),
        _resource(CHILD_UID, "composite", "child_cmp", "child.composite"),
        _resource(MESH_UID, "static_mesh", "mesh_a", "missing.mesh.fbx"),
    ]
    resources[0]["properties"] = {"category": "building"}
    resources[2]["material_slots"] = [
        {"slot_name": "missing_material",
         "material_uid": MISSING_MATERIAL_UID}]
    _write_json(tmp_path / "export_manifest.json", _manifest(resources))
    return root_path


def test_dependency_import_builds_collections_instances_and_custom_props(tmp_path):
    root_path = _write_dependency_fixture(tmp_path)
    report = import_composite_file(str(root_path), import_fbx=False)
    assert report["ok"]
    assert report["scene"].name == "GEOMETRY"
    assert set(report["imported_composites"]) == {ROOT_UID, CHILD_UID}
    assert report["placeholders"] == [MESH_UID]
    assert {warning["code"] for warning in report["warnings"]} == {
        "MH_W_MATERIAL_NOT_FOUND"}

    root = _collection_by_uid(ROOT_UID)
    child = _collection_by_uid(CHILD_UID)
    mesh = _collection_by_uid(MESH_UID)
    assert root.get(PROP_KIND) == child.get(PROP_KIND) == "composite"
    assert root["mh_p_category"] == "building"
    assert mesh.get(PROP_KIND) == "static_mesh"
    assert len(child.objects) == 0
    assert len(mesh.objects) == 0

    nodes = {obj.get(PROP_UID): obj for obj in root.objects}
    instance = nodes[CHILD_NODE_UID]
    assert instance.type == "EMPTY"
    assert instance.instance_collection == child
    assert instance.parent == nodes[GROUP_UID]
    assert instance["mh_kind"] == "composite_ref"
    assert instance["mh_resource_uid"] == CHILD_UID
    assert list(instance["mh_translation_cm"]) == [100.0, -200.0, 300.0]
    assert list(instance["mh_rotation_quat"]) == \
        [0.0, 0.0, -0.707107, 0.707107]
    assert list(instance["mh_scale"]) == [2.0, 1.0, 0.5]
    assert list(instance["mh_instance_offset"]) == [0.0, 0.0, 0.0]
    assert instance["mh_p_offset"] == 0.5
    assert json.loads(instance["mh_properties_fallback_json"]) == \
        {"nullable": None}
    assert json.loads(instance["mh_custom_metadata_json"])["future_tag"] \
        == {"kept": True}
    # UE (100,-200,300) cm -> Blender (1,2,3) m. Actual matrix is authority.
    assert tuple(round(value, 6) for value in instance.matrix_local.translation) \
        == (1.0, 2.0, 3.0)

    source = json.loads(root_path.read_text(encoding="utf-8"))
    for row in source["nodes"]:
        row.pop("future_tag", None)
    round_trip = composite_disk_dict(extract_composite(root))
    assert round_trip == source


def test_reimport_updates_same_collection_and_node_in_place(tmp_path):
    root_path = _write_dependency_fixture(tmp_path)
    first = import_composite_file(str(root_path), import_fbx=False)
    root = first["root_collection"]
    instance = next(obj for obj in root.objects
                    if obj.get(PROP_UID) == CHILD_NODE_UID)
    root_pointer = root.as_pointer()
    node_pointer = instance.as_pointer()

    stale = bpy.data.objects.new("stale", None)
    stale[PROP_UID] = "90000000-0000-0000-0000-000000000009"
    stale["mh_imported_composite_node"] = True
    root.objects.link(stale)
    stale_name = stale.name
    document = json.loads(root_path.read_text(encoding="utf-8"))
    document["nodes"] = [row for row in document["nodes"]
                         if row["node_uid"] != MESH_NODE_UID]
    child_row = next(row for row in document["nodes"]
                     if row["node_uid"] == CHILD_NODE_UID)
    child_row["local_transform"]["translation_cm"] = [400.0, 0.0, 0.0]
    child_row["properties"] = {"offset": 0.75}
    _write_json(root_path, document)

    second = import_composite_file(str(root_path), import_fbx=False)
    assert second["root_collection"].as_pointer() == root_pointer
    updated = next(obj for obj in root.objects
                   if obj.get(PROP_UID) == CHILD_NODE_UID)
    assert updated.as_pointer() == node_pointer
    assert tuple(round(v, 6) for v in updated.matrix_local.translation) \
        == (4.0, 0.0, 0.0)
    assert updated["mh_p_offset"] == 0.75
    assert stale_name not in bpy.data.objects


def test_cycle_is_rejected_before_geometry_scene_creation(tmp_path):
    root_path = _write_dependency_fixture(tmp_path, cycle=True)
    before_collections = len(bpy.data.collections)
    assert bpy.data.scenes.get("GEOMETRY") is None
    with pytest.raises(CompositeImportError, match="MH_E_COMPOSITE_CYCLE"):
        import_composite_file(str(root_path), import_fbx=False)
    assert bpy.data.scenes.get("GEOMETRY") is None
    assert len(bpy.data.collections) == before_collections


def test_manifest_staged_after_payload_reads_blocks_before_mutation(
        tmp_path, monkeypatch):
    root_path = _write_dependency_fixture(tmp_path)
    baseline = _blend_id_state()
    module = importlib.import_module("mh4blend.scene.import_composite")
    real_preflight = module._preflight_destination

    def exporter_starts_after_payload_reads(plan):
        result = real_preflight(plan)
        stable = (tmp_path / "export_manifest.json").read_bytes()
        (tmp_path / "export_manifest.json.tmp").write_bytes(stable)
        return result

    monkeypatch.setattr(
        module, "_preflight_destination",
        exporter_starts_after_payload_reads)
    with pytest.raises(CompositeImportError,
                       match="MH_E_INVALID_EXPORT_MANIFEST"):
        import_composite_file(str(root_path), import_fbx=False)
    assert _blend_id_state() == baseline
    assert bpy.data.scenes.get("GEOMETRY") is None


def test_manifest_staged_during_fbx_import_rolls_back_all_blender_data(
        tmp_path, monkeypatch):
    fbx_path = tmp_path / "raced.mesh.fbx"
    _export_test_fbx(fbx_path)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mesh_row = _resource(
        MESH_UID, "static_mesh", "mesh_a", fbx_path.name)
    root_path = _write_mesh_import_fixture(tmp_path, [mesh_row])
    baseline = _blend_id_state()
    module = importlib.import_module("mh4blend.scene.import_composite")
    real_import = module._import_fbx_into_collection

    def exporter_starts_during_fbx(scene, collection, source, resource_uid):
        result = real_import(scene, collection, source, resource_uid)
        stable = (tmp_path / "export_manifest.json").read_bytes()
        (tmp_path / "export_manifest.json.tmp").write_bytes(stable)
        return result

    monkeypatch.setattr(
        module, "_import_fbx_into_collection",
        exporter_starts_during_fbx)
    with pytest.raises(CompositeImportError,
                       match="MH_E_INVALID_EXPORT_MANIFEST"):
        import_composite_file(str(root_path))
    assert _blend_id_state() == baseline
    assert bpy.data.scenes.get("GEOMETRY") is None


def test_reserved_random_kind_is_diagnosed_without_scene_mutation(tmp_path):
    path = tmp_path / "random.composite"
    _write_json(path, _document(ROOT_UID, "root_cmp", [
        _node(GROUP_UID, "variant_set", "random_variants")]))
    with pytest.raises(CompositeImportError,
                       match="MH_E_UNSUPPORTED_NODE_KIND"):
        import_composite_file(str(path), import_fbx=False)
    assert bpy.data.scenes.get("GEOMETRY") is None


def test_available_fbx_is_imported_into_mesh_collection(tmp_path):
    bpy.ops.mesh.primitive_cube_add()
    cube = bpy.context.object
    source_material = bpy.data.materials.new("slot_mat")
    unmapped_material = bpy.data.materials.new("unmapped_mat")
    cube.data.materials.append(source_material)
    cube.data.materials.append(unmapped_material)
    for index, polygon in enumerate(cube.data.polygons):
        if index % 2 == 0:
            polygon.material_index = 1
    cube.select_set(True)
    fbx_path = tmp_path / "mesh.mesh.fbx"
    bpy.ops.export_scene.fbx(filepath=str(fbx_path), use_selection=True)
    bpy.data.objects.remove(cube, do_unlink=True)
    bpy.data.materials.remove(source_material)
    bpy.data.materials.remove(unmapped_material)

    root_path = tmp_path / "root.composite"
    _write_json(root_path, _document(ROOT_UID, "root_cmp", [
        _node(MESH_NODE_UID, "mesh", "mesh_instance", MESH_UID)]))
    mesh_resource = _resource(
        MESH_UID, "static_mesh", "mesh_a", fbx_path.name)
    mesh_resource["material_slots"] = [
        {"slot_name": "slot_mat", "material_uid": MATERIAL_UID},
        {"slot_name": "ghost_slot",
         "material_uid": MISSING_MATERIAL_UID},
    ]
    material_row = {
        "uid": MATERIAL_UID,
        "kind": "material",
        "name": "slot_mat",
        "shader_class": "rendinst_simple",
        "params": {"sides": 2, "micro_detail_layer": 6},
        "textures": {"tex0": "A:\\textures\\metal_d.tif"},
        "content_hash": "xxh3:material-test",
    }
    manifest = _manifest([
        _resource(ROOT_UID, "composite", "root_cmp", "root.composite"),
        mesh_resource,
    ])
    manifest["materials"] = [material_row]
    _write_json(tmp_path / "export_manifest.json", manifest)

    report = import_composite_file(str(root_path))
    assert report["imported_meshes"] == [MESH_UID]
    mesh_collection = _collection_by_uid(MESH_UID)
    assert len(mesh_collection.objects) > 0
    assert all(obj.get("mh_imported_resource_uid") == MESH_UID
               for obj in mesh_collection.objects)
    imported_material = next(
        slot.material for obj in mesh_collection.objects if obj.type == "MESH"
        for slot in obj.material_slots if slot.name == "slot_mat")
    assert imported_material[PROP_UID] == MATERIAL_UID
    assert json.loads(imported_material["mh_material_payload_json"]) == \
        material_row
    assert not hasattr(imported_material, "dagormat")
    assert report["rehydrated_materials"] == [MATERIAL_UID]
    warning_codes = {warning["code"] for warning in report["warnings"]}
    assert warning_codes == {
        "MH_W_MATERIAL_NOT_FOUND",
        "MH_W_MATERIAL_PAYLOAD_FALLBACK",
        "MH_W_MATERIAL_SLOT_NOT_FOUND",
        "MH_W_MATERIAL_SLOT_UNMAPPED",
    }


def test_unrepresentable_dagormat_payload_roundtrips_via_json_fallback(
        tmp_path, dagormat_rna):
    fbx_path = tmp_path / "material.mesh.fbx"
    _export_test_fbx(fbx_path, "slot_mat")
    bpy.ops.wm.read_factory_settings(use_empty=True)

    mesh_row = _resource(
        MESH_UID, "static_mesh", "mesh_a", fbx_path.name)
    mesh_row["material_slots"] = [
        {"slot_name": "slot_mat", "material_uid": MATERIAL_UID}]
    params = {
        "sides": 2,
        "nullable": None,
        "nested": {"items": [1, None, {"x": 2}]},
    }
    material_row = {
        "uid": MATERIAL_UID,
        "kind": "material",
        "name": "slot_mat",
        "shader_class": "rendinst_simple",
        "params": params,
        "textures": {"tex0": r"A:\textures\metal_d.tif"},
        "content_hash": "xxh3:material-fallback-test",
    }
    root_path = _write_mesh_import_fixture(
        tmp_path, [mesh_row], [material_row])

    report = import_composite_file(str(root_path))
    fallback = [warning for warning in report["warnings"]
                if warning["code"] == "MH_W_MATERIAL_PAYLOAD_FALLBACK"]
    assert len(fallback) == 1
    assert fallback[0]["subjects"] == [MATERIAL_UID]

    mesh_collection = _collection_by_uid(MESH_UID)
    material = next(
        slot.material for obj in mesh_collection.objects if obj.type == "MESH"
        for slot in obj.material_slots if slot.name == "slot_mat")
    assert material.dagormat.shader_class == ""
    assert material.dagormat.sides == 0
    assert list(material.dagormat.optional.keys()) == []
    assert list(material.dagormat.textures.keys()) == []
    assert json.loads(material["mh_material_payload_json"])["params"] == params

    reexport = export_fbx_collection(
        mesh_collection, tmp_path / "reexport", dry_run=True)
    assert reexport["ok"] is True
    assert len(reexport["material_entries"]) == 1
    entry = reexport["material_entries"][0]
    assert entry["uid"] == MATERIAL_UID
    assert entry["shader_class"] == material_row["shader_class"]
    assert entry["params"] == params
    assert entry["textures"] == material_row["textures"]


def test_second_fbx_failure_rolls_back_every_created_blender_id(
        tmp_path, monkeypatch):
    first_fbx = tmp_path / "first.mesh.fbx"
    second_fbx = tmp_path / "second.mesh.fbx"
    _export_test_fbx(first_fbx)
    _export_test_fbx(second_fbx)
    bpy.ops.wm.read_factory_settings(use_empty=True)

    rows = [
        _resource(MESH_UID, "static_mesh", "mesh_a", first_fbx.name),
        _resource(
            SECOND_MESH_UID, "static_mesh", "mesh_b", second_fbx.name),
    ]
    root_path = _write_mesh_import_fixture(tmp_path, rows)
    baseline = _blend_id_state()
    module = importlib.import_module("mh4blend.scene.import_composite")
    real_import = module._import_fbx_into_collection
    calls = []

    def fail_second(scene, collection, source, resource_uid):
        calls.append(resource_uid)
        if len(calls) == 2:
            raise RuntimeError("injected second FBX failure")
        return real_import(scene, collection, source, resource_uid)

    monkeypatch.setattr(module, "_import_fbx_into_collection", fail_second)
    with pytest.raises(RuntimeError, match="injected second FBX failure"):
        import_composite_file(str(root_path))
    assert calls == [MESH_UID, SECOND_MESH_UID]
    assert _blend_id_state() == baseline
    assert bpy.data.scenes.get("GEOMETRY") is None


def test_material_apply_failure_restores_old_payload_and_mesh(
        tmp_path, monkeypatch):
    fbx_path = tmp_path / "material.mesh.fbx"
    _export_test_fbx(fbx_path, "slot_mat")
    bpy.ops.wm.read_factory_settings(use_empty=True)

    geometry = bpy.data.scenes.new("GEOMETRY")
    target = bpy.data.collections.new("old_mesh_name")
    target[PROP_UID] = MESH_UID
    target[PROP_KIND] = "static_mesh"
    target["old_collection_state"] = "keep"
    geometry.collection.children.link(target)
    old_mesh = bpy.data.meshes.new("old_mesh")
    old_object = bpy.data.objects.new("old_mesh", old_mesh)
    old_object["mh_imported_resource_uid"] = MESH_UID
    target.objects.link(old_object)
    existing_material = bpy.data.materials.new("old_material_name")
    existing_material[PROP_UID] = MATERIAL_UID
    existing_material["mh_material_payload_json"] = "old-payload"
    old_mesh.materials.append(existing_material)

    mesh_row = _resource(
        MESH_UID, "static_mesh", "mesh_a", fbx_path.name)
    mesh_row["material_slots"] = [
        {"slot_name": "slot_mat", "material_uid": MATERIAL_UID}]
    material_row = {
        "uid": MATERIAL_UID, "kind": "material", "name": "slot_mat",
        "shader_class": "rendinst_simple", "params": {"sides": 0},
        "textures": {}, "content_hash": "xxh3:new-payload",
    }
    root_path = _write_mesh_import_fixture(
        tmp_path, [mesh_row], [material_row])
    baseline = _blend_id_state()
    module = importlib.import_module("mh4blend.scene.import_composite")

    def fail_material(_material, _row):
        raise RuntimeError("injected material apply failure")

    monkeypatch.setattr(
        module, "_hydrate_dagormat_if_available", fail_material)
    with pytest.raises(RuntimeError, match="injected material apply failure"):
        import_composite_file(str(root_path))

    assert _blend_id_state() == baseline
    assert list(target.objects) == [old_object]
    assert target["old_collection_state"] == "keep"
    assert target.name == "old_mesh_name"
    assert existing_material[PROP_UID] == MATERIAL_UID
    assert existing_material.name == "old_material_name"
    assert existing_material["mh_material_payload_json"] == "old-payload"


def test_node_apply_failure_restores_existing_hierarchy_and_stale_nodes(
        tmp_path, monkeypatch):
    root_path = _write_dependency_fixture(tmp_path)
    geometry = bpy.data.scenes.new("GEOMETRY")
    root = bpy.data.collections.new("root_cmp")
    root[PROP_UID] = ROOT_UID
    root[PROP_KIND] = "composite"
    geometry.collection.children.link(root)
    existing_group = bpy.data.objects.new("old_group_name", None)
    existing_group[PROP_UID] = GROUP_UID
    existing_group["old_property"] = "keep"
    existing_group.location = (9.0, 8.0, 7.0)
    root.objects.link(existing_group)
    stale = bpy.data.objects.new("stale_node", None)
    stale[PROP_UID] = "d0000000-0000-0000-0000-00000000000d"
    stale["mh_imported_composite_node"] = True
    root.objects.link(stale)
    baseline = _blend_id_state()
    root_pointer = root.as_pointer()
    group_pointer = existing_group.as_pointer()
    stale_pointer = stale.as_pointer()
    module = importlib.import_module("mh4blend.scene.import_composite")
    real_apply = module._apply_document

    def fail_after_apply(collection, document, collections, transaction):
        result = real_apply(collection, document, collections, transaction)
        if document["uid"] == ROOT_UID:
            raise RuntimeError("injected node apply failure")
        return result

    monkeypatch.setattr(module, "_apply_document", fail_after_apply)
    with pytest.raises(RuntimeError, match="injected node apply failure"):
        import_composite_file(str(root_path), import_fbx=False)

    assert _blend_id_state() == baseline
    assert root.as_pointer() == root_pointer
    assert existing_group.as_pointer() == group_pointer
    assert stale.as_pointer() == stale_pointer
    assert existing_group.name == "old_group_name"
    assert existing_group["old_property"] == "keep"
    assert tuple(existing_group.location) == (9.0, 8.0, 7.0)
    assert {obj.as_pointer() for obj in root.objects} == {
        group_pointer, stale_pointer}


def test_same_uid_collection_and_material_rename_in_place(tmp_path):
    fbx_path = tmp_path / "renamed.mesh.fbx"
    _export_test_fbx(fbx_path, "slot_mat")
    bpy.ops.wm.read_factory_settings(use_empty=True)

    geometry = bpy.data.scenes.new("GEOMETRY")
    target = bpy.data.collections.new("old_mesh_name")
    target[PROP_UID] = MESH_UID
    target[PROP_KIND] = "static_mesh"
    geometry.collection.children.link(target)
    material = bpy.data.materials.new("old_material_name")
    material[PROP_UID] = MATERIAL_UID
    collection_pointer = target.as_pointer()
    material_pointer = material.as_pointer()

    mesh_row = _resource(
        MESH_UID, "static_mesh", "renamed_mesh", fbx_path.name)
    mesh_row["material_slots"] = [
        {"slot_name": "slot_mat", "material_uid": MATERIAL_UID}]
    material_row = {
        "uid": MATERIAL_UID, "kind": "material", "name": "slot_mat",
        "shader_class": "rendinst_simple", "params": {"sides": 0},
        "textures": {}, "content_hash": "xxh3:renamed",
    }
    root_path = _write_mesh_import_fixture(
        tmp_path, [mesh_row], [material_row])
    report = import_composite_file(str(root_path))

    renamed_collection = _collection_by_uid(MESH_UID)
    renamed_material = next(item for item in bpy.data.materials
                            if item.get(PROP_UID) == MATERIAL_UID)
    assert renamed_collection.as_pointer() == collection_pointer
    assert renamed_collection.name == "renamed_mesh"
    assert renamed_material.as_pointer() == material_pointer
    assert renamed_material.name == "slot_mat"
    assert report["rehydrated_materials"] == [MATERIAL_UID]
    stable_counts = {
        "materials": len(bpy.data.materials),
        "images": len(bpy.data.images),
        "collections": len(bpy.data.collections),
    }
    second = import_composite_file(str(root_path))
    assert second["rehydrated_materials"] == [MATERIAL_UID]
    assert {name: len(getattr(bpy.data, name)) for name in stable_counts} \
        == stable_counts
    assert not [material.name for material in bpy.data.materials
                if re.search(r"\.\d{3}$", material.name)]


def _write_material_ownership_fixture(tmp_path):
    mesh_row = _resource(
        MESH_UID, "static_mesh", "mesh_a", "missing.mesh.fbx")
    mesh_row["material_slots"] = [
        {"slot_name": "slot_mat", "material_uid": MATERIAL_UID}]
    material_row = {
        "uid": MATERIAL_UID, "kind": "material", "name": "slot_mat",
        "shader_class": "rendinst_simple", "params": {}, "textures": {},
        "content_hash": "xxh3:material",
    }
    return _write_mesh_import_fixture(
        tmp_path, [mesh_row], [material_row])


def test_duplicate_material_uid_blocks_before_mutation(tmp_path):
    root_path = _write_material_ownership_fixture(tmp_path)
    first = bpy.data.materials.new("slot_mat")
    second = bpy.data.materials.new("slot_mat_copy")
    first[PROP_UID] = second[PROP_UID] = MATERIAL_UID
    baseline = _blend_id_state()
    with pytest.raises(CompositeImportError,
                       match="MH_E_DUPLICATE_RESOURCE_UID"):
        import_composite_file(str(root_path), import_fbx=False)
    assert _blend_id_state() == baseline
    assert bpy.data.scenes.get("GEOMETRY") is None


def test_material_name_owned_by_different_uid_blocks_before_mutation(tmp_path):
    root_path = _write_material_ownership_fixture(tmp_path)
    conflict = bpy.data.materials.new("slot_mat")
    conflict[PROP_UID] = MISSING_MATERIAL_UID
    baseline = _blend_id_state()
    with pytest.raises(CompositeImportError,
                       match="MH_E_TARGET_NAME_COLLISION"):
        import_composite_file(str(root_path), import_fbx=False)
    assert _blend_id_state() == baseline
    assert bpy.data.scenes.get("GEOMETRY") is None


def test_collection_target_name_collision_blocks_before_mutation(tmp_path):
    root_path = _write_material_ownership_fixture(tmp_path)
    conflict = bpy.data.collections.new("mesh_a")
    conflict[PROP_UID] = SECOND_MESH_UID
    baseline = _blend_id_state()
    with pytest.raises(CompositeImportError,
                       match="MH_E_TARGET_NAME_COLLISION"):
        import_composite_file(str(root_path), import_fbx=False)
    assert _blend_id_state() == baseline
    assert bpy.data.scenes.get("GEOMETRY") is None


def test_linked_material_uid_blocks_before_mutation(tmp_path):
    root_path = _write_material_ownership_fixture(tmp_path)
    library_path = tmp_path / "linked_material.blend"
    source = bpy.data.materials.new("slot_mat")
    source[PROP_UID] = MATERIAL_UID
    bpy.data.libraries.write(str(library_path), {source})
    bpy.data.materials.remove(source)
    with bpy.data.libraries.load(str(library_path), link=True) as (
            data_from, data_to):
        assert "slot_mat" in data_from.materials
        data_to.materials = ["slot_mat"]
    linked = data_to.materials[0]
    assert linked.library is not None
    baseline = _blend_id_state()
    with pytest.raises(CompositeImportError,
                       match="MH_E_FOREIGN_UID_OWNER"):
        import_composite_file(str(root_path), import_fbx=False)
    assert _blend_id_state() == baseline
    assert bpy.data.scenes.get("GEOMETRY") is None
