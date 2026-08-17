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
from mh4blend.core.materials import material_content_hash  # noqa: E402
from mh4blend.core.material_source import build_material_document  # noqa: E402
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
from mh4blend.scene.source_manifest import (  # noqa: E402
    abandon_staged_manifest,
    ManifestError,
    prepare_manifest_update,
    stage_manifest,
)

ROOT_UID = "10000000-0000-0000-0000-000000000001"
CHILD_UID = "20000000-0000-0000-0000-000000000002"
MESH_UID = "30000000-0000-0000-0000-000000000003"
GROUP_UID = "40000000-0000-0000-0000-000000000004"
CHILD_NODE_UID = "50000000-0000-0000-0000-000000000005"
MESH_NODE_UID = "60000000-0000-0000-0000-000000000006"
MATERIAL_UID = "a0000000-0000-0000-0000-00000000000a"
MISSING_MATERIAL_UID = "b0000000-0000-0000-0000-00000000000b"
SECOND_MESH_UID = "31000000-0000-0000-0000-000000000004"
MISSING_COMPOSITE_UID = "21000000-0000-0000-0000-000000000012"
MISSING_COMPOSITE_NODE_UID = "51000000-0000-0000-0000-000000000015"


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
        "resources": sorted(resources, key=lambda row: row["uid"].encode()),
    }


def _resource(uid, kind, name, source):
    return {
        "uid": uid, "kind": kind, "name": name, "source": source,
        "content_hash": "xxh3:0000000000000000",
    }


def _source_name(uid, kind, name):
    suffix = {
        "static_mesh": ".mesh.fbx",
        "composite": ".composite",
        "material": ".material",
    }[kind]
    return f"{name}__{uid[:8]}{suffix}"


def _write_material_fixture(directory, material):
    document, _diagnostics = build_material_document(
        uid=material["uid"],
        name=material["name"],
        shader_class=material["shader_class"],
        params=material.get("params", {}),
        textures=material.get("textures", {}),
        source_root=str(directory),
    )
    source = _source_name(material["uid"], "material", material["name"])
    _write_json(directory / source, document)
    row = _resource(
        material["uid"], "material", material["name"], source)
    row["content_hash"] = material_content_hash(
        document["shader_class"], document["params"], document["textures"])
    return row, document


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
    root_source = _source_name(ROOT_UID, "composite", "root_cmp")
    root_path = tmp_path / root_source
    _write_json(root_path, _document(ROOT_UID, "root_cmp", nodes))
    material_rows = [
        _write_material_fixture(tmp_path, material)[0]
        for material in materials
    ]
    manifest = _manifest([
        _resource(ROOT_UID, "composite", "root_cmp", root_source),
        *mesh_rows,
        *material_rows,
    ])
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

    mesh_source = _source_name(MESH_UID, "static_mesh", "mesh_a")
    child_source = _source_name(CHILD_UID, "composite", "child_cmp")
    (tmp_path / mesh_source).write_bytes(b"test-fbx-payload")
    _write_json(
        tmp_path / child_source,
        _document(CHILD_UID, "child_cmp", []))
    _write_json(tmp_path / "export_manifest.json", _manifest([
        _resource(MESH_UID, "static_mesh", "mesh_a", mesh_source),
        _resource(CHILD_UID, "composite", "child_cmp", child_source),
    ]))

    report = export_composite_collection(
        definition, str(tmp_path), source_root=str(tmp_path))
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
    rows = {row["uid"]: row for row in manifest["resources"]}
    assert rows[ROOT_UID] == report["resource_entry"]
    assert set(rows) == {ROOT_UID, CHILD_UID, MESH_UID}
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
        export_composite_collection(
            definition, str(tmp_path), source_root=str(tmp_path))
    assert list(tmp_path.iterdir()) == []


def test_composite_export_blocks_unresolved_mesh_dependency(tmp_path):
    definition = bpy.data.collections.new("root_cmp")
    definition[PROP_UID] = ROOT_UID
    target = bpy.data.collections.new("mesh_a")
    target[PROP_UID] = MESH_UID
    target.objects.link(bpy.data.objects.new(
        "mesh_content", bpy.data.meshes.new("mesh_content_data")))
    node = bpy.data.objects.new("mesh_node", None)
    node[PROP_UID] = MESH_NODE_UID
    node.instance_type = "COLLECTION"
    node.instance_collection = target
    definition.objects.link(node)

    with pytest.raises(MHValidationError,
                       match="MH_E_UNRESOLVED_EXTERNAL"):
        export_composite_collection(
            definition, str(tmp_path), source_root=str(tmp_path))
    assert list(tmp_path.iterdir()) == []


def test_composite_export_blocks_external_dependency_cycle(tmp_path):
    definition = bpy.data.collections.new("root_cmp")
    definition[PROP_UID] = ROOT_UID
    child = bpy.data.collections.new("child_cmp")
    child[PROP_UID] = CHILD_UID
    node = bpy.data.objects.new("child_node", None)
    node[PROP_UID] = CHILD_NODE_UID
    node.instance_type = "COLLECTION"
    node.instance_collection = child
    definition.objects.link(node)

    child_source = _source_name(CHILD_UID, "composite", "child_cmp")
    _write_json(tmp_path / child_source, _document(CHILD_UID, "child_cmp", [
        _node(
            "80000000-0000-0000-0000-000000000008",
            "composite_ref", "back_to_root", ROOT_UID),
    ]))
    _write_json(tmp_path / "export_manifest.json", _manifest([
        _resource(CHILD_UID, "composite", "child_cmp", child_source),
    ]))

    with pytest.raises(MHValidationError, match="MH_E_COMPOSITE_CYCLE"):
        export_composite_collection(
            definition, str(tmp_path), source_root=str(tmp_path))
    assert not list(tmp_path.glob(f"*__{ROOT_UID[:8]}.composite"))


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
        export_composite_collection(
            definition, str(tmp_path), source_root=str(tmp_path))
    assert list(tmp_path.iterdir()) == []


def test_export_stages_manifest_before_payload_replace(tmp_path, monkeypatch):
    definition = bpy.data.collections.new("root_cmp")
    definition[PROP_UID] = ROOT_UID
    module = importlib.import_module("mh4blend.scene.export_composite")

    def fail_payload(_path, _document):
        raise RuntimeError("simulated payload failure")

    monkeypatch.setattr(module, "_write_json_atomic", fail_payload)
    with pytest.raises(RuntimeError, match="simulated payload failure"):
        export_composite_collection(
            definition, str(tmp_path), source_root=str(tmp_path))
    assert (tmp_path / "export_manifest.json.tmp").exists()
    assert not (tmp_path / "export_manifest.json").exists()
    assert not list(tmp_path.glob("*.composite"))


def test_unchanged_composite_hash_skips_payload_rewrite(
        tmp_path, monkeypatch):
    definition = bpy.data.collections.new("root_cmp")
    definition[PROP_UID] = ROOT_UID
    initial = export_composite_collection(
        definition, str(tmp_path), source_root=str(tmp_path))
    payload_path = Path(initial["path"])
    original_payload = payload_path.read_bytes()
    module = importlib.import_module("mh4blend.scene.export_composite")
    writes = []
    monkeypatch.setattr(
        module, "_write_json_atomic",
        lambda path, document: writes.append((path, document)))

    repeated = export_composite_collection(
        definition, str(tmp_path), source_root=str(tmp_path))

    assert repeated["ok"]
    assert repeated["written"] is False
    assert repeated["manifest_written"] is True
    assert writes == []
    assert payload_path.read_bytes() == original_payload


def test_manifest_only_composite_change_skips_payload_rewrite(
        tmp_path, monkeypatch):
    definition = bpy.data.collections.new("root_cmp")
    definition[PROP_UID] = ROOT_UID
    initial = export_composite_collection(
        definition, str(tmp_path), source_root=str(tmp_path))
    payload_path = Path(initial["path"])
    original_payload = payload_path.read_bytes()
    definition["mh_p_category"] = "building"
    module = importlib.import_module("mh4blend.scene.export_composite")
    writes = []
    monkeypatch.setattr(
        module, "_write_json_atomic",
        lambda path, document: writes.append((path, document)))

    updated = export_composite_collection(
        definition, str(tmp_path), source_root=str(tmp_path))

    assert updated["written"] is False
    assert updated["manifest_written"] is True
    assert writes == []
    assert payload_path.read_bytes() == original_payload
    manifest = json.loads(
        (tmp_path / "export_manifest.json").read_text(encoding="utf-8"))
    row = next(item for item in manifest["resources"]
               if item["uid"] == ROOT_UID)
    assert row["properties"] == {"category": "building"}


def test_missing_owned_composite_payload_is_rebuilt(tmp_path):
    definition = bpy.data.collections.new("root_cmp")
    definition[PROP_UID] = ROOT_UID
    initial = export_composite_collection(
        definition, str(tmp_path), source_root=str(tmp_path))
    payload_path = Path(initial["path"])
    payload_path.unlink()

    rebuilt = export_composite_collection(
        definition, str(tmp_path), source_root=str(tmp_path))

    assert rebuilt["ok"]
    assert rebuilt["written"] is True
    assert rebuilt["manifest_written"] is True
    assert payload_path.is_file()
    assert json.loads(payload_path.read_text(encoding="utf-8"))["uid"] == \
        ROOT_UID


def test_corrupt_owned_composite_payload_is_rebuilt(tmp_path):
    definition = bpy.data.collections.new("root_cmp")
    definition[PROP_UID] = ROOT_UID
    initial = export_composite_collection(
        definition, str(tmp_path), source_root=str(tmp_path))
    payload_path = Path(initial["path"])
    payload_path.write_bytes(b"not-json")

    rebuilt = export_composite_collection(
        definition, str(tmp_path), source_root=str(tmp_path))

    assert rebuilt["ok"]
    assert rebuilt["written"] is True
    assert rebuilt["manifest_written"] is True
    assert json.loads(payload_path.read_text(encoding="utf-8"))["uid"] == \
        ROOT_UID


def test_dependency_snapshot_is_reproved_under_stage_root_lock(
        tmp_path, monkeypatch):
    root = bpy.data.collections.new("root_cmp")
    root[PROP_UID] = ROOT_UID
    child = bpy.data.collections.new("child_cmp")
    child[PROP_UID] = CHILD_UID
    initial_root = export_composite_collection(
        root, str(tmp_path), source_root=str(tmp_path))
    export_composite_collection(
        child, str(tmp_path), source_root=str(tmp_path))
    root_payload = Path(initial_root["path"])
    original_root_payload = root_payload.read_bytes()

    root_to_child = bpy.data.objects.new("root_to_child", None)
    root_to_child[PROP_UID] = CHILD_NODE_UID
    root_to_child.instance_type = "COLLECTION"
    root_to_child.instance_collection = child
    root.objects.link(root_to_child)

    module = importlib.import_module("mh4blend.scene.export_composite")
    real_stage = module.stage_manifest
    interleaved = False

    def stage_after_child_commit(directory, document, **kwargs):
        nonlocal interleaved
        if not interleaved:
            interleaved = True
            child_to_root = bpy.data.objects.new("child_to_root", None)
            child_to_root[PROP_UID] = \
                "80000000-0000-0000-0000-000000000008"
            child_to_root.instance_type = "COLLECTION"
            child_to_root.instance_collection = root
            child.objects.link(child_to_root)
            export_composite_collection(
                child, str(tmp_path), source_root=str(tmp_path))
        return real_stage(directory, document, **kwargs)

    monkeypatch.setattr(module, "stage_manifest", stage_after_child_commit)
    with pytest.raises(ManifestError, match="changed after snapshot"):
        export_composite_collection(
            root, str(tmp_path), source_root=str(tmp_path))

    assert interleaved
    assert not (tmp_path / "export_manifest.json.tmp").exists()
    assert root_payload.read_bytes() == original_root_payload
    child_payload = json.loads(next(
        tmp_path.glob(f"*__{CHILD_UID[:8]}.composite")
    ).read_text(encoding="utf-8"))
    assert child_payload["nodes"][0]["resource_uid"] == ROOT_UID


def test_composite_retry_recovers_marker_and_forces_payload_rewrite(
        tmp_path, monkeypatch):
    definition = bpy.data.collections.new("root_cmp")
    definition[PROP_UID] = ROOT_UID
    initial = export_composite_collection(
        definition, str(tmp_path), source_root=str(tmp_path))
    payload_path = Path(initial["path"])
    original_payload = payload_path.read_bytes()

    group = bpy.data.objects.new("new_group", None)
    group[PROP_UID] = GROUP_UID
    definition.objects.link(group)
    module = importlib.import_module("mh4blend.scene.export_composite")
    real_write = module._write_json_atomic

    def fail_payload(_path, _document):
        raise RuntimeError("injected composite payload failure")

    monkeypatch.setattr(module, "_write_json_atomic", fail_payload)
    with pytest.raises(RuntimeError,
                       match="injected composite payload failure"):
        export_composite_collection(
            definition, str(tmp_path), source_root=str(tmp_path))
    marker = tmp_path / "export_manifest.json.tmp"
    assert marker.is_file()
    assert payload_path.read_bytes() == original_payload

    writes = []

    def record_recovery_write(path, document):
        writes.append((path, document["uid"]))
        return real_write(path, document)

    monkeypatch.setattr(module, "_write_json_atomic", record_recovery_write)
    recovered = export_composite_collection(
        definition, str(tmp_path), source_root=str(tmp_path))

    assert recovered["ok"]
    assert writes == [(str(payload_path), ROOT_UID)]
    assert not marker.exists()
    assert payload_path.read_bytes() != original_payload
    payload = json.loads(payload_path.read_text(encoding="utf-8"))
    assert [row["node_uid"] for row in payload["nodes"]] == [GROUP_UID]
    manifest = json.loads(
        (tmp_path / "export_manifest.json").read_text(encoding="utf-8"))
    manifest_row = next(
        row for row in manifest["resources"] if row["uid"] == ROOT_UID)
    assert manifest_row == recovered["resource_entry"]


def test_composite_retry_rejects_different_selected_uid(tmp_path, monkeypatch):
    definition = bpy.data.collections.new("root_cmp")
    definition[PROP_UID] = ROOT_UID
    module = importlib.import_module("mh4blend.scene.export_composite")

    def fail_payload(_path, _document):
        raise RuntimeError("leave root marker")

    monkeypatch.setattr(module, "_write_json_atomic", fail_payload)
    with pytest.raises(RuntimeError, match="leave root marker"):
        export_composite_collection(
            definition, str(tmp_path), source_root=str(tmp_path))

    other = bpy.data.collections.new("other_cmp")
    other[PROP_UID] = CHILD_UID
    with pytest.raises(MHValidationError,
                       match="MH_E_INVALID_EXPORT_MANIFEST"):
        export_composite_collection(
            other, str(tmp_path), source_root=str(tmp_path))
    assert (tmp_path / "export_manifest.json.tmp").is_file()


def test_composite_export_rejects_pending_non_composite_kind(tmp_path):
    mesh_source = _source_name(MESH_UID, "static_mesh", "mesh_a")
    pending = prepare_manifest_update(
        str(tmp_path),
        resources=[_resource(
            MESH_UID, "static_mesh", "mesh_a", mesh_source)],
        exporter_version="test",
        source_root=str(tmp_path),
    )
    stage_manifest(str(tmp_path), pending)
    abandon_staged_manifest(str(tmp_path))

    definition = bpy.data.collections.new("root_cmp")
    definition[PROP_UID] = ROOT_UID
    with pytest.raises(ValueError, match="MH_E_INVALID_EXPORT_MANIFEST"):
        export_composite_collection(
            definition, str(tmp_path), source_root=str(tmp_path))
    assert (tmp_path / "export_manifest.json.tmp").is_file()


def _write_dependency_fixture(tmp_path, *, cycle=False):
    root_nodes = [
        _node(GROUP_UID, "group", "group", properties={"seed": 7}),
        _node(CHILD_NODE_UID, "composite_ref", "child_instance",
              CHILD_UID, GROUP_UID, (100.0, -200.0, 300.0),
              (0.0, 0.0, -0.707107, 0.707107), (2.0, 1.0, 0.5),
              properties={"offset": 0.5, "nullable": None}),
        _node(MESH_NODE_UID, "mesh", "mesh_instance", MESH_UID),
    ]
    child_nodes = []
    if cycle:
        child_nodes.append(_node(
            "80000000-0000-0000-0000-000000000008",
            "composite_ref", "back_to_root", ROOT_UID))
    root_source = _source_name(ROOT_UID, "composite", "root_cmp")
    child_source = _source_name(CHILD_UID, "composite", "child_cmp")
    mesh_source = _source_name(MESH_UID, "static_mesh", "missing")
    root_path = tmp_path / root_source
    child_path = tmp_path / child_source
    _write_json(root_path, _document(ROOT_UID, "root_cmp", root_nodes))
    _write_json(child_path, _document(CHILD_UID, "child_cmp", child_nodes))
    resources = [
        _resource(ROOT_UID, "composite", "root_cmp", root_source),
        _resource(CHILD_UID, "composite", "child_cmp", child_source),
        _resource(MESH_UID, "static_mesh", "mesh_a", mesh_source),
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
    # UE (100,-200,300) cm -> Blender (1,2,3) m. Actual matrix is authority.
    assert tuple(round(value, 6) for value in instance.matrix_local.translation) \
        == (1.0, 2.0, 3.0)

    source = json.loads(root_path.read_text(encoding="utf-8"))
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


def test_cycle_back_edge_becomes_warning_placeholder(tmp_path):
    root_path = _write_dependency_fixture(tmp_path, cycle=True)
    report = import_composite_file(str(root_path), import_fbx=False)
    cycle_warning = [warning for warning in report["warnings"]
                     if warning["code"] == "MH_W_COMPOSITE_CYCLE"]
    assert len(cycle_warning) == 1
    child = _collection_by_uid(CHILD_UID)
    back_edge = next(obj for obj in child.objects
                     if obj.get(PROP_UID)
                     == "80000000-0000-0000-0000-000000000008")
    assert back_edge["mh_resource_uid"] == ROOT_UID
    assert back_edge["mh_unresolved"] is True
    assert back_edge.instance_type == "NONE"
    assert back_edge.instance_collection is None
    assert back_edge.empty_display_type == "CUBE"


def _write_missing_payload_owner_fixture(tmp_path):
    root_source = _source_name(ROOT_UID, "composite", "root_cmp")
    missing_mesh_source = _source_name(
        MESH_UID, "static_mesh", "missing_mesh")
    missing_composite_source = _source_name(
        MISSING_COMPOSITE_UID, "composite", "missing_composite")
    root_path = tmp_path / root_source
    _write_json(root_path, _document(ROOT_UID, "root_cmp", [
        _node(MESH_NODE_UID, "mesh", "mesh_placeholder", MESH_UID),
        _node(
            MISSING_COMPOSITE_NODE_UID, "composite_ref",
            "composite_placeholder", MISSING_COMPOSITE_UID),
    ]))
    _write_json(tmp_path / "export_manifest.json", _manifest([
        _resource(ROOT_UID, "composite", "root_cmp", root_source),
        _resource(
            MISSING_COMPOSITE_UID, "composite", "missing_composite",
            missing_composite_source),
        _resource(
            MESH_UID, "static_mesh", "missing_mesh", missing_mesh_source),
    ]))
    return root_path, tmp_path / missing_mesh_source


def test_global_import_preserves_owned_missing_payload_placeholders(tmp_path):
    root_path, _missing_mesh_path = _write_missing_payload_owner_fixture(
        tmp_path)
    report = import_composite_file(
        str(root_path), source_root=str(tmp_path), import_fbx=False)
    assert set(report["placeholders"]) == {
        MESH_UID, MISSING_COMPOSITE_UID}

    root = _collection_by_uid(ROOT_UID)
    nodes = {obj.get(PROP_UID): obj for obj in root.objects}
    expected = {
        MESH_NODE_UID: MESH_UID,
        MISSING_COMPOSITE_NODE_UID: MISSING_COMPOSITE_UID,
    }
    for node_uid, resource_uid in expected.items():
        node = nodes[node_uid]
        target = _collection_by_uid(resource_uid)
        assert node[PROP_UID] == node_uid
        assert node["mh_resource_uid"] == resource_uid
        assert node["mh_unresolved"] is True
        assert node.instance_type == "COLLECTION"
        assert node.instance_collection == target
        assert target["mh_unresolved"] is True


def test_missing_payload_appearance_during_apply_rolls_back(tmp_path,
                                                            monkeypatch):
    root_path, missing_mesh_path = _write_missing_payload_owner_fixture(
        tmp_path)
    baseline = _blend_id_state()
    module = importlib.import_module("mh4blend.scene.import_composite")
    real_apply = module._apply_document

    def create_payload_after_apply(collection, document, collections,
                                   transaction):
        result = real_apply(collection, document, collections, transaction)
        if document["uid"] == ROOT_UID:
            missing_mesh_path.write_bytes(b"appeared during Blender apply")
        return result

    monkeypatch.setattr(module, "_apply_document", create_payload_after_apply)
    with pytest.raises(CompositeImportError,
                       match="MH_E_INVALID_EXPORT_MANIFEST"):
        import_composite_file(
            str(root_path), source_root=str(tmp_path), import_fbx=False)
    assert _blend_id_state() == baseline
    assert bpy.data.scenes.get("GEOMETRY") is None


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


def test_sibling_composite_changed_after_plan_blocks_before_mutation(
        tmp_path, monkeypatch):
    root_path = _write_dependency_fixture(tmp_path)
    child_path = tmp_path / _source_name(
        CHILD_UID, "composite", "child_cmp")
    baseline = _blend_id_state()
    module = importlib.import_module("mh4blend.scene.import_composite")
    real_preflight = module._preflight_destination

    def mutate_child_after_plan(plan):
        result = real_preflight(plan)
        child_path.write_bytes(child_path.read_bytes() + b" ")
        return result

    monkeypatch.setattr(
        module, "_preflight_destination", mutate_child_after_plan)
    with pytest.raises(CompositeImportError,
                       match="MH_E_INVALID_EXPORT_MANIFEST"):
        import_composite_file(str(root_path), import_fbx=False)
    assert _blend_id_state() == baseline
    assert bpy.data.scenes.get("GEOMETRY") is None


def test_manifest_staged_during_fbx_import_rolls_back_all_blender_data(
        tmp_path, monkeypatch):
    fbx_path = tmp_path / _source_name(
        MESH_UID, "static_mesh", "raced")
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


def test_fbx_changed_during_import_rolls_back_all_blender_data(
        tmp_path, monkeypatch):
    fbx_path = tmp_path / _source_name(
        MESH_UID, "static_mesh", "raced")
    _export_test_fbx(fbx_path)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mesh_row = _resource(
        MESH_UID, "static_mesh", "mesh_a", fbx_path.name)
    root_path = _write_mesh_import_fixture(tmp_path, [mesh_row])
    baseline = _blend_id_state()
    module = importlib.import_module("mh4blend.scene.import_composite")
    real_import = module._import_fbx_into_collection

    def mutate_fbx_after_read(scene, collection, source, resource_uid):
        result = real_import(scene, collection, source, resource_uid)
        with open(source, "ab") as stream:
            stream.write(b"changed")
        return result

    monkeypatch.setattr(
        module, "_import_fbx_into_collection", mutate_fbx_after_read)
    with pytest.raises(CompositeImportError,
                       match="MH_E_INVALID_EXPORT_MANIFEST"):
        import_composite_file(str(root_path))
    assert _blend_id_state() == baseline
    assert bpy.data.scenes.get("GEOMETRY") is None


def test_reserved_random_kind_is_diagnosed_without_scene_mutation(tmp_path):
    path = tmp_path / _source_name(ROOT_UID, "composite", "random")
    _write_json(path, _document(ROOT_UID, "root_cmp", [
        _node(GROUP_UID, "variant_set", "random_variants")]))
    with pytest.raises(CompositeImportError,
                       match="MH_E_UNSUPPORTED_NODE_KIND"):
        import_composite_file(str(path), import_fbx=False)
    assert bpy.data.scenes.get("GEOMETRY") is None


def test_unknown_composite_node_field_is_rejected_before_mutation(tmp_path):
    path = tmp_path / _source_name(ROOT_UID, "composite", "unknown")
    node = _node(GROUP_UID, "group", "group")
    node["future_tag"] = {"not": "a v1 extension bag"}
    _write_json(path, _document(ROOT_UID, "root_cmp", [node]))
    with pytest.raises(CompositeImportError, match="MH_E_INVALID_COMPOSITE"):
        import_composite_file(str(path), import_fbx=False)
    assert bpy.data.scenes.get("GEOMETRY") is None


def _write_material_integrity_fixture(tmp_path):
    mesh_row = _resource(
        MESH_UID, "static_mesh", "mesh_a",
        _source_name(MESH_UID, "static_mesh", "missing"))
    mesh_row["material_slots"] = [
        {"slot_name": "slot_mat", "material_uid": MATERIAL_UID}]
    material = {
        "uid": MATERIAL_UID,
        "name": "slot_mat",
        "shader_class": "rendinst_simple",
        "params": {"sides": 0},
        "textures": {},
    }
    root_path = _write_mesh_import_fixture(tmp_path, [mesh_row], [material])
    material_path = tmp_path / _source_name(
        MATERIAL_UID, "material", "slot_mat")
    return root_path, material_path


@pytest.mark.parametrize(("field", "value", "code"), [
    ("uid", MISSING_MATERIAL_UID, "MH_E_RESOURCE_UID_MISMATCH"),
    ("name", "renamed_elsewhere", "MH_E_NAME_MISMATCH"),
])
def test_material_payload_identity_must_match_manifest_before_mutation(
        tmp_path, field, value, code):
    root_path, material_path = _write_material_integrity_fixture(tmp_path)
    document = json.loads(material_path.read_text(encoding="utf-8"))
    document[field] = value
    _write_json(material_path, document)
    baseline = _blend_id_state()
    with pytest.raises(CompositeImportError, match=code):
        import_composite_file(str(root_path), import_fbx=False)
    assert _blend_id_state() == baseline
    assert bpy.data.scenes.get("GEOMETRY") is None


def test_material_payload_hash_must_match_manifest_before_mutation(tmp_path):
    root_path, material_path = _write_material_integrity_fixture(tmp_path)
    document = json.loads(material_path.read_text(encoding="utf-8"))
    document["params"]["sides"] = 2
    _write_json(material_path, document)
    baseline = _blend_id_state()
    with pytest.raises(CompositeImportError,
                       match="MH_E_INVALID_EXPORT_MANIFEST"):
        import_composite_file(str(root_path), import_fbx=False)
    assert _blend_id_state() == baseline
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
    fbx_path = tmp_path / _source_name(MESH_UID, "static_mesh", "mesh")
    bpy.ops.export_scene.fbx(filepath=str(fbx_path), use_selection=True)
    bpy.data.objects.remove(cube, do_unlink=True)
    bpy.data.materials.remove(source_material)
    bpy.data.materials.remove(unmapped_material)

    root_source = _source_name(ROOT_UID, "composite", "root_cmp")
    root_path = tmp_path / root_source
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
    material_resource, material_document = _write_material_fixture(
        tmp_path, material_row)
    manifest = _manifest([
        _resource(ROOT_UID, "composite", "root_cmp", root_source),
        mesh_resource,
        material_resource,
    ])
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
        material_document
    assert not hasattr(imported_material, "dagormat")
    assert report["rehydrated_materials"] == [MATERIAL_UID]
    warning_codes = {warning["code"] for warning in report["warnings"]}
    assert warning_codes == {
        "MH_W_MATERIAL_NOT_FOUND",
        "MH_W_MATERIAL_PAYLOAD_FALLBACK",
        "MH_W_MATERIAL_SLOT_NOT_FOUND",
        "MH_W_MATERIAL_SLOT_UNMAPPED",
        "MH_W_TEXTURE_OUTSIDE_ROOT",
    }


def test_unrepresentable_dagormat_payload_roundtrips_via_json_fallback(
        tmp_path, dagormat_rna):
    fbx_path = tmp_path / _source_name(
        MESH_UID, "static_mesh", "material")
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
    assert len(reexport["materials"]) == 1
    resource = reexport["materials"][0]
    assert resource.uid == MATERIAL_UID
    assert resource.shader_class == material_row["shader_class"]
    assert resource.params == params
    assert resource.textures == {"tex0": "A:/textures/metal_d.tif"}


def test_second_fbx_failure_rolls_back_every_created_blender_id(
        tmp_path, monkeypatch):
    first_fbx = tmp_path / _source_name(
        MESH_UID, "static_mesh", "first")
    second_fbx = tmp_path / _source_name(
        SECOND_MESH_UID, "static_mesh", "second")
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
    fbx_path = tmp_path / _source_name(
        MESH_UID, "static_mesh", "material")
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
    fbx_path = tmp_path / _source_name(
        MESH_UID, "static_mesh", "renamed")
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
        MESH_UID, "static_mesh", "mesh_a",
        _source_name(MESH_UID, "static_mesh", "missing"))
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
