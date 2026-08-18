"""Blender gates for the production passport-first FBX writer."""

import contextlib
import importlib
from pathlib import Path
import sys
import uuid

import pytest

bpy = pytest.importorskip("bpy")

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.fbx_passport import (  # noqa: E402
    PASSPORT_PROPERTY,
    read_fbx_passport,
)
from mh4blend.core.uid import PROP_UID  # noqa: E402
from mh4blend.core.validate import MHValidationError  # noqa: E402
from mh4blend.scene.export_fbx import export_fbx_collection  # noqa: E402

export_fbx_module = importlib.import_module("mh4blend.scene.export_fbx")


class _FbxTestTextures(bpy.types.PropertyGroup):
    tex0: bpy.props.StringProperty(default="", subtype="FILE_PATH")


class _FbxTestOptional(bpy.types.PropertyGroup):
    pass


class _FbxTestDagormat(bpy.types.PropertyGroup):
    shader_class: bpy.props.StringProperty(default="")
    sides: bpy.props.IntProperty(default=0, min=0, max=2)
    optional: bpy.props.PointerProperty(type=_FbxTestOptional)
    textures: bpy.props.PointerProperty(type=_FbxTestTextures)


@pytest.fixture(scope="module", autouse=True)
def dagormat_rna():
    if hasattr(bpy.types.Material, "dagormat"):
        pytest.skip("test dagormat RNA conflicts with an enabled addon")
    classes = (_FbxTestTextures, _FbxTestOptional, _FbxTestDagormat)
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.Material.dagormat = bpy.props.PointerProperty(type=_FbxTestDagormat)
    yield
    del bpy.types.Material.dagormat
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)


def _mesh_object(name, collection):
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata(
        [(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    return obj


def _empty(name, collection):
    obj = bpy.data.objects.new(name, None)
    collection.objects.link(obj)
    return obj


def _collection(name="Selected Resource"):
    collection = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(collection)
    return collection


def _material(name="M Surface"):
    material = bpy.data.materials.new(name)
    material.dagormat.shader_class = "rendinst_simple"
    return material


def _assign_material(obj, material):
    obj.data.materials.append(material)


def _build_joined(name="Selected Resource"):
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
    render0 = _mesh_object("Body_LOD0", lod0)
    render1a = _mesh_object("Body_LOD1_A", lod1)
    render1b = _mesh_object("Body_LOD1_B", lod1)
    collision = _mesh_object("UCX_Body_00", lod0)
    socket = _empty("SOCKET_Muzzle", lod0)
    ignored_collision = _mesh_object("UCX_Body_LOD1", lod1)
    ignored_socket = _empty("SOCKET_LOD1", lod1)
    return {
        "root": root,
        "lod0": lod0,
        "lod1": lod1,
        "render": (render0, render1a, render1b),
        "collision": collision,
        "socket": socket,
        "ignored": (ignored_collision, ignored_socket),
    }


def _fbx_model_custom_properties(path):
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
            (item for item in model.elems if item.id == b"Properties70"),
            None)
        if props70 is not None:
            for item in props70.elems:
                if item.id == b"P" and len(item.props) >= 5:
                    key = item.props[0].decode("utf-8")
                    value = item.props[4]
                    if isinstance(value, bytes):
                        value = value.decode("utf-8")
                    props[key] = value
        result[name] = {"kind": model.props[2], "properties": props}
    return result


def test_explicit_export_always_rewrites_clean_fbx_without_manifest(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, direct, nested = _build_joined()
    direct[PASSPORT_PROPERTY] = "artist value"
    direct["mh_lod_level"] = 9
    calls = []
    real_export = export_fbx_module._export_selected_fbx

    def counted(path):
        calls.append(path)
        return real_export(path)

    monkeypatch.setattr(export_fbx_module, "_export_selected_fbx", counted)
    first = export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    second = export_fbx_collection(collection, tmp_path, source_root=tmp_path)

    assert Path(first["filepath"]).name == "selected_resource.mesh.fbx"
    assert first["written"] is second["written"] is True
    assert len(calls) == 2
    assert Path(second["filepath"]).is_file()
    assert not (tmp_path / "export_manifest.json").exists()
    assert "__" not in Path(first["filepath"]).stem
    assert direct[PASSPORT_PROPERTY] == "artist value"
    assert direct["mh_lod_level"] == 9
    assert PASSPORT_PROPERTY not in nested
    parsed = read_fbx_passport(first["filepath"])
    assert parsed.document == first["passport"]
    assert parsed.copy_count == 2
    models = _fbx_model_custom_properties(first["filepath"])
    assert "mh_lod_level" not in models["Direct"]["properties"]


@pytest.mark.parametrize("mutation", [
    "material_uid", "slot_and_hint", "properties",
])
def test_each_metadata_mutation_is_rewritten(tmp_path, mutation):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, direct, _nested = _build_joined("Metadata Asset")
    material = _material()
    _assign_material(direct, material)

    first = export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    first_passport = first["passport"]
    if mutation == "material_uid":
        material[PROP_UID] = str(uuid.uuid4())
    elif mutation == "slot_and_hint":
        material.name = "M Renamed"
    else:
        collection["mh_p_role"] = "roof"
    second = export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    actual = read_fbx_passport(second["filepath"]).document

    assert second["written"] is True
    assert actual != first_passport
    if mutation == "material_uid":
        assert actual["material_slots"][0]["material_uid"] == material[PROP_UID]
    elif mutation == "slot_and_hint":
        assert actual["material_slots"][0]["slot_name"] == "M Renamed"
        assert actual["material_slots"][0]["material_name_hint"] == "M Renamed"
    else:
        assert actual["properties"] == {"role": "roof"}


def test_resource_rename_writes_new_clean_path_with_same_uid(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, _direct, _nested = _build_joined("Old Name")
    first = export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    uid = first["passport"]["resource_uid"]
    collection.name = "New Name"
    second = export_fbx_collection(collection, tmp_path, source_root=tmp_path)

    assert Path(first["filepath"]).name == "old_name.mesh.fbx"
    assert Path(second["filepath"]).name == "new_name.mesh.fbx"
    assert second["passport"]["resource_uid"] == uid
    assert second["passport"]["name"] == "New Name"
    assert Path(first["filepath"]).is_file()
    assert Path(second["filepath"]).is_file()


def test_lod_policy_metadata_change_is_rewritten(tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection = _collection("Policy Asset")
    obj = _mesh_object("Body", collection)
    authored = {"enabled": True}
    real_detector = export_fbx_module._dagor_lod_structure

    def detector(value):
        if authored["enabled"]:
            return {
                "resource_name": value.name,
                "levels": [(0, value, [obj])],
                "level0_aux": [],
                "ignored_aux": [],
            }
        return real_detector(value)

    monkeypatch.setattr(export_fbx_module, "_dagor_lod_structure", detector)
    first = export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    authored["enabled"] = False
    second = export_fbx_collection(collection, tmp_path, source_root=tmp_path)

    assert first["passport"]["lod_policy"] == "authored"
    assert second["passport"]["lod_policy"] == "generated"
    assert read_fbx_passport(second["filepath"]).document == second["passport"]


def test_clean_name_collision_with_different_uid_never_overwrites(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    first_collection, _direct, _nested = _build_joined("Same Name")
    first = export_fbx_collection(first_collection, tmp_path, source_root=tmp_path)
    target = Path(first["filepath"])
    original = target.read_bytes()

    bpy.ops.wm.read_factory_settings(use_empty=True)
    second_collection, _direct, _nested = _build_joined("Same Name")
    monkeypatch.setattr(
        export_fbx_module, "mesh_content_hash",
        lambda *_args: pytest.fail("collision must precede geometry hashing"))
    with pytest.raises(ValueError, match="MH_E_NAME_COLLISION_DIFFERENT_UID"):
        export_fbx_collection(second_collection, tmp_path, source_root=tmp_path)
    assert target.read_bytes() == original
    assert not Path(str(target) + ".tmp").exists()


def test_malformed_existing_target_is_not_silently_adopted(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, _direct, _nested = _build_joined("Malformed")
    target = tmp_path / "malformed.mesh.fbx"
    target.write_bytes(b"not an FBX")
    with pytest.raises(ValueError, match="MH_E_PASSPORT_INVALID"):
        export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert target.read_bytes() == b"not an FBX"


def test_combined_lod_has_one_passport_source_and_model_levels(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    built = _build_lods()
    built["render"][0]["mh_lod_level"] = 9
    built["collision"]["mh_lod_level"] = 7
    built["socket"]["mh_lod_level"] = 8
    report = export_fbx_collection(
        built["root"], tmp_path, source_root=tmp_path)
    target = Path(report["filepath"])

    assert target.name == "garage.mesh.fbx"
    assert list(tmp_path.glob("*.mesh.fbx")) == [target]
    assert report["passport"]["lod_levels"] == [0, 1]
    assert report["passport"]["lod_policy"] == "authored"
    assert read_fbx_passport(target).copy_count == 4
    models = _fbx_model_custom_properties(target)
    assert models["Body_LOD0"]["properties"]["mh_lod_level"] == 0
    assert models["Body_LOD1_A"]["properties"]["mh_lod_level"] == 1
    assert models["Body_LOD1_B"]["properties"]["mh_lod_level"] == 1
    assert "mh_lod_level" not in models["UCX_Body_00"]["properties"]
    assert PASSPORT_PROPERTY in models["UCX_Body_00"]["properties"]
    assert PASSPORT_PROPERTY not in models["SOCKET_Muzzle"]["properties"]
    assert "mh_lod_level" not in models["SOCKET_Muzzle"]["properties"]
    assert "UCX_Body_LOD1" not in models
    assert "SOCKET_LOD1" not in models
    warnings = report["validation"]["warnings"]
    assert any(row["code"] == "MH_W_LOD_AUX_NODE_IGNORED"
               for row in warnings)
    assert built["render"][0]["mh_lod_level"] == 9
    assert built["collision"]["mh_lod_level"] == 7
    assert built["socket"]["mh_lod_level"] == 8


def test_lod_slot_set_must_be_subset_of_lod0(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    built = _build_lods("slots")
    _assign_material(built["render"][0], _material("Base"))
    _assign_material(built["render"][1], _material("Only Higher"))
    with pytest.raises(MHValidationError) as exc:
        export_fbx_collection(built["root"], tmp_path, source_root=tmp_path)
    assert exc.value.code == "MH_E_LOD_SLOT_NOT_IN_BASE"


def test_failed_or_nonconsensus_staged_fbx_preserves_old_payload(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, direct, nested = _build_joined("Atomic")
    first = export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    target = Path(first["filepath"])
    original = target.read_bytes()

    @contextlib.contextmanager
    def incomplete_carrier(objects, passport_text):
        direct[PASSPORT_PROPERTY] = passport_text
        try:
            yield
        finally:
            if PASSPORT_PROPERTY in direct:
                del direct[PASSPORT_PROPERTY]
            if PASSPORT_PROPERTY in nested:
                del nested[PASSPORT_PROPERTY]

    monkeypatch.setattr(
        export_fbx_module, "_temporary_passport_properties",
        incomplete_carrier)
    collection["changed"] = True
    with pytest.raises(ValueError, match="MH_E_PASSPORT_INVALID"):
        export_fbx_collection(collection, tmp_path, source_root=tmp_path)
    assert target.read_bytes() == original
    assert not Path(str(target) + ".tmp").exists()


def test_fbx_reports_touched_materials_without_exporting_them(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, direct, _nested = _build_joined("Material Hook")
    material = _material()
    _assign_material(direct, material)
    report = export_fbx_collection(
        collection, tmp_path, source_root=tmp_path)

    assert [row[PROP_UID] for row in report["materials"]] == [
        material[PROP_UID]]
    assert list(tmp_path.glob("*.material")) == []
    assert not (tmp_path / "export_manifest.json").exists()
