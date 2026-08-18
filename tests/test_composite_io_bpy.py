from __future__ import annotations

import importlib
import json
from pathlib import Path
import sys

import pytest

bpy = pytest.importorskip("bpy")


REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.fbx_passport import (  # noqa: E402
    PASSPORT_PROPERTY,
    canonical_passport,
    make_fbx_passport,
)
from mh4blend.core.source_index_v2 import SourceIndexV2Error  # noqa: E402
from mh4blend.core.texture_actualize import TextureActualizeError  # noqa: E402
from mh4blend.core.uid import PROP_UID  # noqa: E402
from mh4blend.scene.composite_extract import (  # noqa: E402
    PROP_KIND,
    PROP_PREFIX_PROPERTIES,
    PROP_RESOURCE_UID,
)
from mh4blend.scene.import_composite import (  # noqa: E402
    CompositeImportError,
    import_composite_file,
)


ROOT_UID = "10000000-0000-0000-0000-000000000001"
CHILD_UID = "20000000-0000-0000-0000-000000000002"
MESH_UID = "30000000-0000-0000-0000-000000000003"
MATERIAL_UID = "40000000-0000-0000-0000-000000000004"
MISSING_UID = "90000000-0000-0000-0000-000000000009"
GROUP_UID = "50000000-0000-0000-0000-000000000005"
ROOT_CHILD_NODE = "60000000-0000-0000-0000-000000000006"
CHILD_MESH_NODE = "70000000-0000-0000-0000-000000000007"
BACK_EDGE_NODE = "80000000-0000-0000-0000-000000000008"


@pytest.fixture(autouse=True)
def clean_blender():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    yield
    bpy.ops.wm.read_factory_settings(use_empty=True)


def _write_json(path: Path, document: dict) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(document, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")
    return path


def _transform(x=0.0, y=0.0, z=0.0):
    return {
        "translation_cm": [x, y, z],
        "rotation_quat": [0.0, 0.0, 0.0, 1.0],
        "scale": [1.0, 1.0, 1.0],
    }


def _node(node_uid, kind, display_name, resource_uid=None, *, parent_uid=None,
          properties=None, x=0.0):
    row = {
        "node_uid": node_uid,
        "parent_uid": parent_uid,
        "kind": kind,
        "display_name": display_name,
        "local_transform": _transform(x=x),
        "properties": dict(properties or {}),
    }
    if kind != "group":
        row["resource_uid"] = resource_uid
    return row


def _document(uid, name, nodes=(), *, properties=None, version=2):
    result = {
        "schema": "mh.composite",
        "schema_version": version,
        "uid": uid,
        "name": name,
        "nodes": list(nodes),
    }
    if version == 2:
        result["properties"] = dict(properties or {})
    return result


def _material(uid=MATERIAL_UID, name="slot_mat", *, textures=None):
    return {
        "schema": "mh.material",
        "schema_version": 1,
        "uid": uid,
        "name": name,
        "shader_class": "rendinst_simple",
        "params": {"sides": 0},
        "textures": dict(textures or {}),
    }


def _export_mesh_fbx(path: Path, uid=MESH_UID, name="mesh_a", *,
                     material_uid=MATERIAL_UID, slot_name="slot_mat",
                     properties=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.mesh.primitive_cube_add()
    obj = bpy.context.object
    obj.name = name
    material_slots = []
    if material_uid is not None:
        material = bpy.data.materials.new(slot_name)
        obj.data.materials.append(material)
        material_slots.append({
            "slot_name": slot_name,
            "material_uid": material_uid,
            "material_name_hint": slot_name,
        })
    passport = make_fbx_passport(
        resource_uid=uid,
        name=name,
        lod_levels=[0],
        lod_policy="generated",
        geometry_hash="xxh3:0123456789abcdef",
        material_slots=material_slots,
        properties=dict(properties or {}),
        exporter="mh4blend test",
    )
    obj[PASSPORT_PROPERTY] = canonical_passport(passport)
    obj["mh_lod_level"] = 0
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    result = bpy.ops.export_scene.fbx(
        filepath=str(path), use_selection=True, use_custom_props=True,
        add_leaf_bones=False)
    assert "FINISHED" in result
    bpy.ops.wm.read_factory_settings(use_empty=True)
    return path


def _import_kwargs(root: Path):
    return {
        "source_root": str(root),
        "index_path": str(root.parent / "cache" / "index.json"),
        "lock_root": str(root.parent / "cache" / "locks"),
    }


def _collection(uid):
    matches = [item for item in bpy.data.collections
               if item.get(PROP_UID) == uid]
    assert len(matches) == 1, (uid, [item.name for item in matches])
    return matches[0]


def _node_object(collection, uid):
    return next(obj for obj in collection.objects if obj.get(PROP_UID) == uid)


def _blend_id_state():
    return {
        name: tuple(sorted(item.name for item in getattr(bpy.data, name)))
        for name in ("scenes", "collections", "objects", "meshes", "materials")
    }


def _write_recursive_fixture(root: Path, *, texture_path=None):
    material_path = _write_json(
        root / "materials" / "slot_mat.material",
        _material(textures={"tex0": texture_path} if texture_path else {}))
    fbx_path = _export_mesh_fbx(
        root / "meshes" / "mesh_a.mesh.fbx",
        properties={"payload_role": "wall"})
    child_path = _write_json(
        root / "composites" / "child.composite",
        _document(CHILD_UID, "child", [
            _node(
                CHILD_MESH_NODE, "mesh", "mesh_instance", MESH_UID,
                properties={"placement_role": "detail"}, x=125.0),
        ], properties={"resource_role": "subassembly"}))
    root_path = _write_json(
        root / "composites" / "root.composite",
        _document(ROOT_UID, "root", [
            _node(GROUP_UID, "group", "group"),
            _node(
                ROOT_CHILD_NODE, "composite_ref", "child_instance",
                CHILD_UID, parent_uid=GROUP_UID),
        ], properties={"resource_role": "assembly"}))
    return root_path, child_path, fbx_path, material_path


def test_v2_recursive_clean_sources_import_fbx_materials_and_properties(
        tmp_path):
    source = tmp_path / "source"
    root_path, _child_path, _fbx_path, _material_path = \
        _write_recursive_fixture(source)

    report = import_composite_file(str(root_path), **_import_kwargs(source))

    assert report["ok"] is True
    assert report["composite_uids"] == [ROOT_UID, CHILD_UID]
    assert report["mesh_uids"] == [MESH_UID]
    assert report["imported_fbx"] == [MESH_UID]
    assert report["rehydrated_materials"] == [MATERIAL_UID]
    assert not list(source.rglob("export_manifest.json"))
    assert Path(_import_kwargs(source)["index_path"]).is_file()

    root_collection = _collection(ROOT_UID)
    child_collection = _collection(CHILD_UID)
    mesh_collection = _collection(MESH_UID)
    assert root_collection[f"{PROP_PREFIX_PROPERTIES}resource_role"] == \
        "assembly"
    assert child_collection[f"{PROP_PREFIX_PROPERTIES}resource_role"] == \
        "subassembly"
    assert mesh_collection[f"{PROP_PREFIX_PROPERTIES}payload_role"] == "wall"
    child_instance = _node_object(root_collection, ROOT_CHILD_NODE)
    mesh_instance = _node_object(child_collection, CHILD_MESH_NODE)
    assert child_instance.instance_collection == child_collection
    assert mesh_instance.instance_collection == mesh_collection
    assert mesh_instance[f"{PROP_PREFIX_PROPERTIES}placement_role"] == "detail"
    assert round(mesh_instance.location.x, 6) == 1.25
    imported_materials = [
        material for material in bpy.data.materials
        if material.get(PROP_UID) == MATERIAL_UID]
    assert len(imported_materials) == 1
    assert json.loads(imported_materials[0]["mh_material_payload_json"])[
        "uid"] == MATERIAL_UID
    pointers = {
        uid: _collection(uid).as_pointer()
        for uid in (ROOT_UID, CHILD_UID, MESH_UID)}
    counts = {
        name: len(getattr(bpy.data, name))
        for name in ("collections", "objects", "meshes", "materials")}

    repeated = import_composite_file(
        str(root_path), **_import_kwargs(source))

    assert repeated["imported_fbx"] == [MESH_UID]
    assert {uid: _collection(uid).as_pointer() for uid in pointers} == pointers
    assert {name: len(getattr(bpy.data, name)) for name in counts} == counts


def test_missing_uid_creates_identity_placeholder_without_manifest(tmp_path):
    source = tmp_path / "source"
    root_path = _write_json(
        source / "root.composite",
        _document(ROOT_UID, "root", [
            _node(CHILD_MESH_NODE, "mesh", "missing_mesh", MISSING_UID),
        ]))

    report = import_composite_file(
        str(root_path), import_fbx=False, **_import_kwargs(source))

    assert report["placeholders"] == [MISSING_UID]
    placeholder = _collection(MISSING_UID)
    assert placeholder[PROP_KIND] == "static_mesh"
    assert placeholder["mh_unresolved"] is True
    instance = _node_object(_collection(ROOT_UID), CHILD_MESH_NODE)
    assert instance[PROP_RESOURCE_UID] == MISSING_UID
    assert instance.instance_collection == placeholder
    assert instance["mh_unresolved"] is True


def test_cycle_back_edge_is_warning_placeholder(tmp_path):
    source = tmp_path / "source"
    root_path = _write_json(
        source / "root.composite",
        _document(ROOT_UID, "root", [
            _node(ROOT_CHILD_NODE, "composite_ref", "child", CHILD_UID),
        ]))
    _write_json(
        source / "child.composite",
        _document(CHILD_UID, "child", [
            _node(BACK_EDGE_NODE, "composite_ref", "back", ROOT_UID),
        ]))

    report = import_composite_file(
        str(root_path), import_fbx=False, **_import_kwargs(source))

    warnings = [row for row in report["warnings"]
                if row["code"] == "MH_W_COMPOSITE_CYCLE"]
    assert len(warnings) == 1
    back_edge = _node_object(_collection(CHILD_UID), BACK_EDGE_NODE)
    assert back_edge.instance_type == "NONE"
    assert back_edge.instance_collection is None
    assert back_edge["mh_unresolved"] is True


def test_divergent_uid_is_hard_before_blender_mutation(tmp_path):
    source = tmp_path / "source"
    root_path = _write_json(
        source / "root.composite",
        _document(ROOT_UID, "root", [
            _node(ROOT_CHILD_NODE, "composite_ref", "child", CHILD_UID),
        ]))
    _write_json(
        source / "a" / "child.composite",
        _document(CHILD_UID, "child", [], properties={"revision": "a"}))
    _write_json(
        source / "b" / "child.composite",
        _document(CHILD_UID, "child", [], properties={"revision": "b"}))
    baseline = _blend_id_state()

    with pytest.raises(SourceIndexV2Error) as caught:
        import_composite_file(
            str(root_path), import_fbx=False, **_import_kwargs(source))

    assert caught.value.code == "MH_E_DIVERGENT_REVISIONS"
    assert _blend_id_state() == baseline
    assert bpy.data.scenes.get("GEOMETRY") is None


def test_divergent_selected_root_uid_is_hard_before_mutation(tmp_path):
    source = tmp_path / "source"
    root_path = _write_json(
        source / "a" / "root.composite",
        _document(ROOT_UID, "root", properties={"revision": "a"}))
    _write_json(
        source / "b" / "root_copy.composite",
        _document(ROOT_UID, "root", properties={"revision": "b"}))
    baseline = _blend_id_state()

    with pytest.raises(SourceIndexV2Error) as caught:
        import_composite_file(
            str(root_path), import_fbx=False, **_import_kwargs(source))

    assert caught.value.code == "MH_E_DIVERGENT_REVISIONS"
    assert _blend_id_state() == baseline


def test_previously_indexed_fbx_with_invalid_passport_is_hard(tmp_path):
    source = tmp_path / "source"
    fbx_path = _export_mesh_fbx(
        source / "mesh.mesh.fbx", material_uid=None)
    root_path = _write_json(
        source / "root.composite",
        _document(ROOT_UID, "root", [
            _node(CHILD_MESH_NODE, "mesh", "mesh", MESH_UID),
        ]))
    kwargs = _import_kwargs(source)
    import_composite_file(
        str(root_path), import_fbx=False, **kwargs)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    fbx_path.write_bytes(b"not an FBX payload")
    baseline = _blend_id_state()

    with pytest.raises(SourceIndexV2Error) as caught:
        import_composite_file(
            str(root_path), import_fbx=False, **kwargs)

    assert caught.value.code == "MH_E_PASSPORT_INVALID"
    assert _blend_id_state() == baseline


def test_snapshot_race_after_apply_rolls_back_all_blender_ids(
        tmp_path, monkeypatch):
    source = tmp_path / "source"
    root_path = _write_json(
        source / "root.composite", _document(ROOT_UID, "root"))
    baseline = _blend_id_state()
    module = importlib.import_module("mh4blend.scene.import_composite")
    real_assert = module._assert_plan_stable
    calls = []

    def fail_after_apply(plan):
        calls.append(len(calls) + 1)
        if len(calls) == 3:
            raise SourceIndexV2Error(
                "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED", "injected race")
        return real_assert(plan)

    monkeypatch.setattr(module, "_assert_plan_stable", fail_after_apply)
    with pytest.raises(SourceIndexV2Error) as caught:
        import_composite_file(
            str(root_path), import_fbx=False, **_import_kwargs(source))

    assert caught.value.code == "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED"
    assert calls == [1, 2, 3]
    assert _blend_id_state() == baseline
    assert bpy.data.scenes.get("GEOMETRY") is None


def test_new_divergent_dependency_after_plan_rolls_back_blender(
        tmp_path, monkeypatch):
    source = tmp_path / "source"
    root_path = _write_json(
        source / "root.composite",
        _document(ROOT_UID, "root", [
            _node(ROOT_CHILD_NODE, "composite_ref", "child", CHILD_UID),
        ]))
    _write_json(
        source / "a" / "child.composite",
        _document(CHILD_UID, "child", [], properties={"revision": "a"}))
    baseline = _blend_id_state()
    module = importlib.import_module("mh4blend.scene.import_composite")
    real_apply = module._apply_document
    injected = False

    def inject_divergent_copy(*args, **kwargs):
        nonlocal injected
        result = real_apply(*args, **kwargs)
        if not injected:
            injected = True
            _write_json(
                source / "b" / "child_copy.composite",
                _document(
                    CHILD_UID, "child", [], properties={"revision": "b"}))
        return result

    monkeypatch.setattr(module, "_apply_document", inject_divergent_copy)
    with pytest.raises(SourceIndexV2Error) as caught:
        import_composite_file(
            str(root_path), import_fbx=False, **_import_kwargs(source))

    assert caught.value.code == "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED"
    assert _blend_id_state() == baseline
    assert bpy.data.scenes.get("GEOMETRY") is None


def test_repeat_import_and_same_uid_rename_update_in_place(tmp_path):
    source = tmp_path / "source"
    root_path = _write_json(
        source / "root.composite", _document(ROOT_UID, "root"))
    kwargs = _import_kwargs(source)
    first = import_composite_file(
        str(root_path), import_fbx=False, **kwargs)
    collection = first["root_collection"]
    pointer = collection.as_pointer()
    counts = (len(bpy.data.collections), len(bpy.data.scenes))

    _write_json(root_path, _document(ROOT_UID, "renamed_root"))
    second = import_composite_file(
        str(root_path), import_fbx=False, **kwargs)

    assert second["root_collection"].as_pointer() == pointer
    assert second["root_collection"].name == "renamed_root"
    assert (len(bpy.data.collections), len(bpy.data.scenes)) == counts


def test_automatic_texture_actualize_rebuilds_index_before_blender_apply(
        tmp_path):
    source = tmp_path / "source"
    root_path, _child, _fbx, material_path = _write_recursive_fixture(
        source, texture_path="old/library/wall_d.tif")
    texture = source / "textures" / "wall_d.tif"
    texture.parent.mkdir(parents=True)
    texture.write_bytes(b"texture")
    kwargs = _import_kwargs(source)

    first = import_composite_file(
        str(root_path), import_fbx=False, **kwargs)

    updated = json.loads(material_path.read_text(encoding="utf-8"))
    assert updated["textures"]["tex0"] == "textures/wall_d.tif"
    assert first["texture_actualization"]["materials_updated"] == [
        MATERIAL_UID]
    assert first["texture_actualization"]["fixed"][0]["slot"] == "tex0"
    second = import_composite_file(
        str(root_path), import_fbx=False, **kwargs)
    assert second["texture_actualization"]["materials_updated"] == []
    assert second["texture_actualization"]["exact"] == 1


def test_import_texture_actualize_race_inside_material_lock_rolls_back(
        tmp_path, monkeypatch):
    source = tmp_path / "source"
    root_path, _child, _fbx, material_path = _write_recursive_fixture(
        source, texture_path="old/library/wall_d.tif")
    texture = source / "textures" / "wall_d.tif"
    texture.parent.mkdir(parents=True)
    texture.write_bytes(b"texture")
    stable = material_path.read_bytes()
    baseline = _blend_id_state()
    module = importlib.import_module("mh4blend.scene.import_composite")
    real_publish = module.atomic_publish_json

    def raced_publish(*args, **kwargs):
        extra = source / "appeared" / "wall_d.tif"
        extra.parent.mkdir()
        extra.write_bytes(b"new")
        return real_publish(*args, **kwargs)

    monkeypatch.setattr(module, "atomic_publish_json", raced_publish)
    with pytest.raises(TextureActualizeError, match="file set changed"):
        import_composite_file(
            str(root_path), import_fbx=False, **_import_kwargs(source))

    assert material_path.read_bytes() == stable
    assert _blend_id_state() == baseline


def test_texture_actualize_ambiguous_and_missing_never_mutate_material(
        tmp_path):
    source = tmp_path / "source"
    root_path, _child, _fbx, material_path = _write_recursive_fixture(source)
    document = _material(textures={
        "tex0": "stale/wall_d.tif",
        "tex1": "stale/missing_n.tif",
    })
    _write_json(material_path, document)
    for folder in ("a", "b"):
        candidate = source / "textures" / folder / "wall_d.tif"
        candidate.parent.mkdir(parents=True, exist_ok=True)
        candidate.write_bytes(folder.encode("ascii"))
    stable = material_path.read_bytes()

    report = import_composite_file(
        str(root_path), import_fbx=False, **_import_kwargs(source))

    actualize = report["texture_actualization"]
    assert actualize["materials_updated"] == []
    assert [row["slot"] for row in actualize["ambiguous"]] == ["tex0"]
    assert [row["slot"] for row in actualize["missing"]] == ["tex1"]
    assert material_path.read_bytes() == stable


def test_composite_v1_is_rejected_before_mutation(tmp_path):
    source = tmp_path / "source"
    root_path = _write_json(
        source / "legacy.composite",
        _document(ROOT_UID, "legacy", version=1))
    baseline = _blend_id_state()

    with pytest.raises(CompositeImportError) as caught:
        import_composite_file(
            str(root_path), import_fbx=False, **_import_kwargs(source))

    assert caught.value.code == "MH_E_UNKNOWN_SCHEMA_VERSION"
    assert _blend_id_state() == baseline
